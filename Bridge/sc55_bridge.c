/*
 * Bridge/sc55_bridge.c — 内蔵 MIDI 音源(Roland SC-55 / Nuked-SC55)の Bridge 層(P826)
 *
 * X68000 の MIDI ボード(CZ-6BM1)が送出する生バイト列を ThirdParty/nuked_sc55
 * (MPX68K 経由で取り込んだ Nuked-SC55)へ流し込み、生成した音声を CoreAudio の
 * コールバックで既存の FM/ADPCM 出力へ加算ミックスする。
 *
 * ★スレッドモデル(Fix Plan P826 §3)— MT-32(P825)とは意図的に異なる
 *   Nuked-SC55 コアは Load/Reset/Send/Render の全呼び出しの直列化をホスト側に要求する
 *   (core/mcu.cpp の "The host serializes all calls."、共有 UART リングのインデックスが
 *   非 atomic)。そこでコアへのアクセスは専用ワーカー(MX68K/App/Services/SC55Worker.swift
 *   の直列 DispatchQueue)だけに閉じ込め、他スレッドとは 2 本の SPSC リングだけでつなぐ:
 *     入力リング: エミュレーションスレッド(sc55_bridge_send_bytes、唯一の書き手)
 *                 → ワーカー(mx68k_sc55_pump_input、唯一の読み手)
 *     出力リング: ワーカー(mx68k_sc55_output_push、唯一の書き手)
 *                 → CoreAudio 実時間スレッド(mx68k_sc55_render_mix、唯一の読み手)
 *   インデックスの memory_order は既存の audio_ring_write/read・rec_audio_ring
 *   (Bridge/EmulatorBridge.c)と同一の規律: 自分のインデックスは relaxed で load、
 *   相手のインデックスは acquire で load、自分のインデックス更新は release で store。
 *   どちらのリングもロックを持たないため、実時間スレッド側はブロックし得ない。
 *
 * ★iOS: P831 で Nuked-SC55 コアを iOS ターゲットにも追加し、両ターゲットで同一実装を
 *   使う(旧来の iOS 用スタブは撤去)。iOS 実機での性能は機種依存のため、有効化するか
 *   どうかは利用者が設定画面で選択する(性能警告文を表示、機種の自動判定はしない)。
 */

#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "EmulatorBridge.h"
#include "sc55_bridge.h"

#include "../ThirdParty/nuked_sc55/SC55Bridge.h"

/* ---- 入力リング(MIDI バイト列)----
 * 容量はコア自身の UART バッファ(core/mcu.h の uart_buffer_size)と同じ 8192 バイト。
 * ワーカーの drain 間隔(約 8ms)に対して十分な余裕がある。満杯と空を区別するため
 * 1 バイトは常に空けておく(実効容量 8191)。
 * オーバーフロー方針: 入るだけ書いて残りを破棄(audio_ring_write と同型)。MIDI バイト列は
 * フレーム境界を持たないので部分破棄でもリング構造は壊れない。破棄量はカウンタで数える。 */
#define SC55_INPUT_RING_BYTES 8192
static uint8_t           s_sc55_in_ring[SC55_INPUT_RING_BYTES];
static _Atomic(uint32_t) g_sc55_in_write = 0;
static _Atomic(uint32_t) g_sc55_in_read  = 0;
static _Atomic(uint32_t) g_sc55_in_drop_bytes = 0;

/* ---- 出力リング(変換後の Int16 L/R インターリーブ、フレーム単位)----
 * 容量はそのまま内蔵 SC-55 音声の遅延になる(フロー制御がほぼ満杯で運用するため)。
 * 2048 フレーム = 44.1kHz で約 46ms / 48kHz で約 43ms / 96kHz で約 21ms。
 * P826 性能実測(Pコア最大 3.15ms の単発スパイク)を吸収できる深さとして選んだ値
 * (Fix Plan 記号表「出力リング容量」)。1 フレームは常に空けておく(実効 2047)。
 * オーバーフロー方針: 空きが足りなければその回のチャンク全体を破棄(rec_audio_ring と
 * 同型)。L/R の境界を跨ぐ部分書込みはチャンネル入替わりの原因になるため行わない。
 * ワーカーは push 前に mx68k_sc55_output_free_frames() で空きを確認するので、
 * 破棄は保険としてのみ起こる。 */
#define SC55_OUT_RING_FRAMES MX68K_SC55_OUTPUT_RING_FRAMES
static int16_t           s_sc55_out_ring[SC55_OUT_RING_FRAMES * 2];
static _Atomic(uint32_t) g_sc55_out_write = 0;   /* フレーム単位 */
static _Atomic(uint32_t) g_sc55_out_read  = 0;   /* フレーム単位 */
static _Atomic(uint32_t) g_sc55_out_drop_chunks = 0;
static _Atomic(uint32_t) g_sc55_underrun_callbacks = 0;
/* 出力リングの破棄要求。読み手インデックスを動かせるのは読み手(実時間スレッド)だけなので、
 * ワーカーは番号を進めるだけにし、読み手が次のコールバックで自分のインデックスを
 * 書き手インデックスへ揃える(audio_ring_discard_all と同じ考え方)。 */
static _Atomic(uint32_t) g_sc55_out_flush_seq = 0;
static uint32_t          s_sc55_out_seen_flush_seq = 0;   /* 実時間スレッド専用 */

/* true の間だけ入力を受け付ける(warm-up 完了後〜無効化/再読込まで)。
 * warm-up 中の MIDI はファームウェアが無視するため、そもそも積まない。 */
static _Atomic(bool)    g_sc55_accepting = false;
static _Atomic(bool)    g_sc55_stop_requested = false;
/* 0 = 未読込/無効, 1 = 動作中, 2 = 適用中(読込済み・warm-up 中),
 * -1 = ROM ファイルが無い/読めない, -2 = ROM のサイズ不正(コアが拒否)。 */
static _Atomic(int32_t) g_sc55_status = 0;

/* ---- 以下はワーカー専用(同期不要)---- */
#define SC55_PUMP_CHUNK_BYTES 1024
static uint8_t  s_sc55_pump_buf[SC55_PUMP_CHUNK_BYTES];
static uint32_t s_sc55_pump_len = 0;   /* >0 = X68SC55_Send が満杯で受け取らず持ち越し中 */

#define SC55_WARMUP_CHUNK_FRAMES 1024
#define SC55_WARMUP_CHUNKS       250   /* 256,000 フレーム = 64kHz で 4 秒分(MPX68K の warmUpFirmware と同量) */
static float s_sc55_warm_l[SC55_WARMUP_CHUNK_FRAMES];
static float s_sc55_warm_r[SC55_WARMUP_CHUNK_FRAMES];

#define SC55_ROM_MAX_BYTES 0x100000

/* ---------------- 入力リング ---------------- */

void sc55_bridge_send_bytes(const uint8_t *data, uint32_t len)
{
	if (data == NULL || len == 0) {
		return;
	}
	if (!atomic_load_explicit(&g_sc55_accepting, memory_order_acquire)) {
		return;   /* 未動作・warm-up 中・無効化済み。受け付けない(破棄カウント対象外) */
	}
	uint32_t w = atomic_load_explicit(&g_sc55_in_write, memory_order_relaxed);
	uint32_t r = atomic_load_explicit(&g_sc55_in_read, memory_order_acquire);
	uint32_t written = 0;
	for (uint32_t i = 0; i < len; i++) {
		uint32_t next = (w + 1) % SC55_INPUT_RING_BYTES;
		if (next == r) {
			break;   /* 満杯: 残りを破棄 */
		}
		s_sc55_in_ring[w] = data[i];
		w = next;
		written++;
	}
	atomic_store_explicit(&g_sc55_in_write, w, memory_order_release);
	if (written < len) {
		atomic_fetch_add_explicit(&g_sc55_in_drop_bytes, len - written, memory_order_relaxed);
	}
}

/* ワーカー専用(入力リングの読み手)。 */
static uint32_t sc55_in_pop(uint8_t *dst, uint32_t max)
{
	uint32_t r = atomic_load_explicit(&g_sc55_in_read, memory_order_relaxed);
	uint32_t w = atomic_load_explicit(&g_sc55_in_write, memory_order_acquire);
	uint32_t n = 0;
	while (n < max && r != w) {
		dst[n++] = s_sc55_in_ring[r];
		r = (r + 1) % SC55_INPUT_RING_BYTES;
	}
	atomic_store_explicit(&g_sc55_in_read, r, memory_order_release);
	return n;
}

/* ワーカー専用。未処理の入力(持ち越し分を含む)を全て捨てる。 */
static void sc55_in_discard_all(void)
{
	uint32_t w = atomic_load_explicit(&g_sc55_in_write, memory_order_acquire);
	atomic_store_explicit(&g_sc55_in_read, w, memory_order_release);
	s_sc55_pump_len = 0;
}

int mx68k_sc55_pump_input(int max_bytes)
{
	int delivered = 0;
	while (delivered < max_bytes) {
		if (s_sc55_pump_len == 0) {
			s_sc55_pump_len = sc55_in_pop(s_sc55_pump_buf, SC55_PUMP_CHUNK_BYTES);
			if (s_sc55_pump_len == 0) {
				break;
			}
		}
		/* X68SC55_Send は空き不足なら 1 バイトも書かず false を返す。その場合は
		 * このチャンクを持ち越し、次の反復(Render でコアが UART を消費した後)に再送する。 */
		if (!X68SC55_Send(s_sc55_pump_buf, s_sc55_pump_len)) {
			break;
		}
		delivered += (int)s_sc55_pump_len;
		s_sc55_pump_len = 0;
	}
	return delivered;
}

/* ---------------- 出力リング ---------------- */

int mx68k_sc55_output_free_frames(void)
{
	uint32_t w = atomic_load_explicit(&g_sc55_out_write, memory_order_relaxed);
	uint32_t r = atomic_load_explicit(&g_sc55_out_read, memory_order_acquire);
	return (int)((r + SC55_OUT_RING_FRAMES - w - 1) % SC55_OUT_RING_FRAMES);
}

int mx68k_sc55_output_push(const int16_t *stereo, int frames)
{
	if (stereo == NULL || frames <= 0) {
		return 0;
	}
	uint32_t w = atomic_load_explicit(&g_sc55_out_write, memory_order_relaxed);
	uint32_t r = atomic_load_explicit(&g_sc55_out_read, memory_order_acquire);
	uint32_t free_frames = (r + SC55_OUT_RING_FRAMES - w - 1) % SC55_OUT_RING_FRAMES;
	if ((uint32_t)frames > free_frames) {
		atomic_fetch_add_explicit(&g_sc55_out_drop_chunks, 1, memory_order_relaxed);
		return 0;
	}
	for (int i = 0; i < frames; i++) {
		s_sc55_out_ring[w * 2]     = stereo[i * 2];
		s_sc55_out_ring[w * 2 + 1] = stereo[i * 2 + 1];
		w = (w + 1) % SC55_OUT_RING_FRAMES;
	}
	atomic_store_explicit(&g_sc55_out_write, w, memory_order_release);
	return frames;
}

/* CoreAudio 実時間スレッド専用。ロック・debug_log・動的確保は一切行わない。 */
void mx68k_sc55_render_mix(int16_t *stereo_buf, int32_t frames)
{
	if (stereo_buf == NULL || frames <= 0) {
		return;
	}
	uint32_t seq = atomic_load_explicit(&g_sc55_out_flush_seq, memory_order_acquire);
	if (seq != s_sc55_out_seen_flush_seq) {
		uint32_t wpos = atomic_load_explicit(&g_sc55_out_write, memory_order_acquire);
		atomic_store_explicit(&g_sc55_out_read, wpos, memory_order_release);
		s_sc55_out_seen_flush_seq = seq;
	}
	uint32_t r = atomic_load_explicit(&g_sc55_out_read, memory_order_relaxed);
	uint32_t w = atomic_load_explicit(&g_sc55_out_write, memory_order_acquire);
	uint32_t avail = (w + SC55_OUT_RING_FRAMES - r) % SC55_OUT_RING_FRAMES;
	uint32_t n = (avail < (uint32_t)frames) ? avail : (uint32_t)frames;
	for (uint32_t i = 0; i < n; i++) {
		for (int ch = 0; ch < 2; ch++) {
			int32_t s = (int32_t)stereo_buf[i * 2 + ch] + (int32_t)s_sc55_out_ring[r * 2 + ch];
			if (s > INT16_MAX) s = INT16_MAX;
			if (s < INT16_MIN) s = INT16_MIN;
			stereo_buf[i * 2 + ch] = (int16_t)s;
		}
		r = (r + 1) % SC55_OUT_RING_FRAMES;
	}
	atomic_store_explicit(&g_sc55_out_read, r, memory_order_release);
	if (n < (uint32_t)frames && atomic_load_explicit(&g_sc55_accepting, memory_order_relaxed)) {
		atomic_fetch_add_explicit(&g_sc55_underrun_callbacks, 1, memory_order_relaxed);
	}
}

/* ---------------- コア操作(ワーカー専用)---------------- */

/* 入力受付を止め、未処理入力と出力リングの中身を捨てる(出力側は読み手へ依頼)。 */
static void sc55_quiesce(void)
{
	atomic_store_explicit(&g_sc55_accepting, false, memory_order_release);
	sc55_in_discard_all();
	atomic_fetch_add_explicit(&g_sc55_out_flush_seq, 1, memory_order_release);
}

/* 戻り値: 0 = 成功, -1 = 無い/読めない, -2 = 空または大きすぎる。 */
static int sc55_read_file(const char *path, uint8_t **out, size_t *out_size)
{
	*out = NULL;
	*out_size = 0;
	if (path == NULL || path[0] == '\0') {
		return -1;
	}
	FILE *fp = fopen(path, "rb");
	if (fp == NULL) {
		return -1;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	long size = ftell(fp);
	if (size <= 0 || size > SC55_ROM_MAX_BYTES) {
		fclose(fp);
		return (size < 0) ? -1 : -2;
	}
	rewind(fp);
	uint8_t *buf = malloc((size_t)size);
	if (buf == NULL) {
		fclose(fp);
		return -1;
	}
	size_t got = fread(buf, 1, (size_t)size, fp);
	fclose(fp);
	if (got != (size_t)size) {
		free(buf);
		return -1;
	}
	*out = buf;
	*out_size = (size_t)size;
	return 0;
}

bool mx68k_sc55_load(const char *control_path, const char *pcm_path,
                     const char *wave1_path, const char *wave2_path, const char *wave3_path)
{
	sc55_quiesce();

	const char *paths[5] = { control_path, pcm_path, wave1_path, wave2_path, wave3_path };
	static const char *names[5] = { "rom1", "rom2", "waverom1", "waverom2", "waverom3" };
	uint8_t *bufs[5] = { NULL, NULL, NULL, NULL, NULL };
	size_t sizes[5] = { 0, 0, 0, 0, 0 };
	int rc = 0;
	int failed = -1;
	for (int i = 0; i < 5 && rc == 0; i++) {
		rc = sc55_read_file(paths[i], &bufs[i], &sizes[i]);
		if (rc != 0) {
			failed = i;
		}
	}
	bool ok = false;
	if (rc == 0) {
		/* X68SC55_Load は 3 つの波形 ROM に共通のサイズを 1 つだけ受け取る。 */
		if (sizes[2] != sizes[3] || sizes[2] != sizes[4]) {
			rc = -2;
		} else {
			/* コアがサイズを検証し(rom1=0x8000、rom2=0x40000/0x80000、波形=0x100000)、
			 * 内部の静的配列へコピーする。呼出し後はこちらのバッファを解放してよい。 */
			ok = X68SC55_Load(bufs[0], sizes[0], bufs[1], sizes[1],
			                  bufs[2], bufs[3], bufs[4], sizes[2]);
			if (!ok) {
				rc = -2;
			}
		}
	}
	for (int i = 0; i < 5; i++) {
		free(bufs[i]);
	}
	atomic_store_explicit(&g_sc55_status, ok ? 2 : rc, memory_order_relaxed);

	if (ok) {
		debug_log("[P826-SC55] loaded rom1=%zu rom2=%zu wave=%zu\n", sizes[0], sizes[1], sizes[2]);
	} else if (failed >= 0) {
		debug_log("[P826-SC55] load failed: %s '%s' %s\n", names[failed],
		          paths[failed] ? paths[failed] : "",
		          rc == -1 ? "not found or unreadable" : "empty or larger than 1MB");
	} else {
		debug_log("[P826-SC55] load rejected by core: sizes rom1=%zu rom2=%zu wave=%zu/%zu/%zu\n",
		          sizes[0], sizes[1], sizes[2], sizes[3], sizes[4]);
	}
	return ok;
}

void mx68k_sc55_reset(void)
{
	sc55_quiesce();
	X68SC55_Reset();   /* 未読込ならコア側で何もしない */
	atomic_store_explicit(&g_sc55_status, 0, memory_order_relaxed);
}

bool mx68k_sc55_render(float *left, float *right, int frames)
{
	if (left == NULL || right == NULL || frames <= 0 || frames > 8192) {
		return false;
	}
	return X68SC55_Render(left, right, (size_t)frames);
}

void mx68k_sc55_warmup(void)
{
	if (atomic_load_explicit(&g_sc55_status, memory_order_relaxed) != 2) {
		return;   /* 読込に成功していない */
	}
	for (int i = 0; i < SC55_WARMUP_CHUNKS; i++) {
		X68SC55_Render(s_sc55_warm_l, s_sc55_warm_r, SC55_WARMUP_CHUNK_FRAMES);
	}
	sc55_in_discard_all();
	atomic_store_explicit(&g_sc55_accepting, true, memory_order_release);
	atomic_store_explicit(&g_sc55_status, 1, memory_order_relaxed);
}

bool mx68k_sc55_test_inject_midi(const uint8_t *bytes, int count)
{
	if (bytes == NULL || count <= 0) {
		return false;
	}
	return X68SC55_Send(bytes, (size_t)count);
}

int32_t mx68k_sc55_get_status(void)
{
	return atomic_load_explicit(&g_sc55_status, memory_order_relaxed);
}

uint32_t mx68k_sc55_get_and_reset_input_drop_bytes(void)
{
	return atomic_exchange_explicit(&g_sc55_in_drop_bytes, 0, memory_order_relaxed);
}

uint32_t mx68k_sc55_get_and_reset_output_drop_chunks(void)
{
	return atomic_exchange_explicit(&g_sc55_out_drop_chunks, 0, memory_order_relaxed);
}

uint32_t mx68k_sc55_get_and_reset_underrun_callbacks(void)
{
	return atomic_exchange_explicit(&g_sc55_underrun_callbacks, 0, memory_order_relaxed);
}

void mx68k_sc55_shutdown(void)
{
	atomic_store_explicit(&g_sc55_accepting, false, memory_order_release);
	atomic_store_explicit(&g_sc55_stop_requested, true, memory_order_release);
}

bool mx68k_sc55_stop_requested(void)
{
	return atomic_load_explicit(&g_sc55_stop_requested, memory_order_acquire);
}

void mx68k_sc55_clear_stop_request(void)
{
	atomic_store_explicit(&g_sc55_stop_requested, false, memory_order_release);
}
