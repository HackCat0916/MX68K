/*
 * Bridge/mt32_bridge.c — 内蔵 MIDI 音源(Roland MT-32 / mt32emu)の Bridge 層(P825)
 *
 * X68000 の MIDI ボード(CZ-6BM1)が送出する生バイト列を、外部 CoreMIDI の代わりに
 * ThirdParty/mt32emu(munt の libmt32emu 2.8.3)へ流し込み、CoreAudio の
 * コールバック内で既存の FM/ADPCM 出力へ加算ミックスする。
 *
 * ★スレッドモデル(Fix Plan P825 §3)
 *   (A) 再構成(context の破棄/作成・ROM 読込・open_synth)はメインスレッドから
 *       直接行わず、mx68k_schedule_hard_reset と同型の「要求だけ出してフレーム境界で
 *       エミュレーションスレッド自身が実行する」方式にする。MIDI 投入
 *       (mt32_bridge_send_bytes)も同じエミュレーションスレッドで走るため、
 *       投入と再構成は逐次実行になり互いに競合しない。
 *   (B) CoreAudio 実時間スレッドのレンダリングと再構成の競合だけを g_mt32_lock で守る。
 *       実時間側は os_unfair_lock_trylock のみ(失敗したらそのバッファは MT-32 無音で
 *       即 return、ブロックしない)。再構成側は低頻度なのでブロッキング取得でよい。
 *   (C) pending フィールド(パス文字列を含む)は書き手(メインスレッド)・読み手
 *       (エミュレーションスレッド)とも g_mt32_lock 保持中にだけ触る(torn read 対策)。
 *   MIDI 投入(エミュスレッド)⇔レンダリング(audio スレッド)の間にはロックを置かない。
 *   mt32emu 自身が「読み手 1・書き手 1 の 2 スレッドなら安全」と明記している
 *   (ThirdParty/mt32emu/src/MidiEventQueue.h:32-34)唯一の構成がまさにこれである。
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <os/lock.h>

#include "../ThirdParty/mt32emu/src/c_interface/c_interface.h"
#include "EmulatorBridge.h"
#include "mt32_bridge.h"

static mt32emu_context g_mt32_ctx = NULL;          /* エミュスレッドが作成/破棄、audio スレッドは lock 下で読む */
static _Atomic(bool)   g_mt32_ready = false;
static os_unfair_lock  g_mt32_lock = OS_UNFAIR_LOCK_INIT;

/* 要求済み・未適用の設定。★すべて g_mt32_lock 保持中にのみ読み書きする。 */
static _Atomic(bool) g_pending_mt32_reconfigure = false;
static bool    g_mt32_pending_enabled = false;
static char    g_mt32_pending_control_path[1024];
static char    g_mt32_pending_pcm_path[1024];
static int32_t g_mt32_pending_sample_rate = 44100;
static int32_t g_mt32_pending_partial_count = 32;  /* 最大パーシャル数。既定32=実機MT-32と同じ上限 */
static int     g_mt32_pending_test_index = 0;     /* >0 = MX68K_TEST_MT32_RECONF 由来の要求番号 */

/* 設定画面向けの直近の適用結果(mx68k_mt32_get_status)。0=無効, 1=動作中, 負値=失敗コード。 */
static _Atomic(int32_t) g_mt32_last_status = 0;

/* mt32emu_configure_midi_event_queue_sysex_storage() に渡す SysEx 格納領域のバイト数。
 * 0 = 動的確保(mt32emu 既定)。サイズ上限なし、SysEx イベントごとに new/delete する。
 * 正の値 = その大きさの固定バイトプールを事前確保する方式で、動的確保へのフォールバックは
 *   無い——プールに収まらない SysEx は何の通知もなく破棄される(「件数」ではなく「バイト数」)。
 * 以前は 4 を指定しており(「不足しても動的確保へ戻る」という誤解に基づく)、P828 の
 * 実行時ログで、実機 MT-32 の SysEx(最短でも 8 バイト、実プレイでは最大 266 バイトを観測)が
 * すべて静かに破棄されていたことが確認されたため、P829 で 0(動的確保)へ変更した。 */
#define MT32_SYSEX_STORAGE_SIZE 0  /* 動的確保(mt32emu既定)。P829参照 */

/* 実時間スレッドでの一時レンダリング先。audio スレッド専用かつ g_mt32_lock 下でのみ使う。 */
#define MT32_MIX_CHUNK_FRAMES 1024
static int16_t s_mt32_mix_buf[MT32_MIX_CHUNK_FRAMES * 2];

/* ---- 診断フック MX68K_TEST_MT32_RECONF(Fix Plan R2-6 / テスト計画 T-1) ----
 * 未設定なら何もしない。値 N(整数)が設定されていると、起動後のフレーム境界ごとに
 * 存在しないパスで有効化/無効化を交互に N 回要求し、適用結果を 1 行ずつログへ出す。
 * エミュレーションスレッドからのみ触るため同期不要。 */
#define MT32_TEST_MAX 1000
static int s_mt32_test_total = -1;   /* -1 = 環境変数をまだ読んでいない */
static int s_mt32_test_done  = 0;

/* ---- P828 診断: SysEx ドロップ観測用カウンタ(挙動変更なし、debug.log 出力のみ) ----
 * g_mt32_sysex_send_count    : mt32_bridge_send_bytes で検出した SysEx(先頭 0xF0)の送信回数(エミュスレッド)
 * g_mt32_sysex_overflow_count: onMIDIQueueOverflow の発火回数(= イベントキュー投入失敗、エミュスレッド)
 * g_mt32_message_played_count: onMIDIMessagePlayed の発火回数(★CoreAudio 実時間スレッドから発火)
 * 実時間スレッド側はアトミック加算のみ行い、ログ出力はエミュスレッド側
 * (mx68k_mt32_apply_pending_reconfigure 先頭)で行う(debug_log は実時間スレッドから呼ばない、P464)。 */
#define MT32_P828_LOG_MAX 20
static _Atomic(uint32_t) g_mt32_sysex_send_count = 0;
static _Atomic(uint32_t) g_mt32_sysex_overflow_count = 0;
static _Atomic(uint64_t) g_mt32_message_played_count = 0;
static uint64_t s_mt32_last_logged_played_count = 0;  /* エミュスレッド専用 */
static int      s_mt32_played_log_count = 0;          /* エミュスレッド専用 */

static mt32emu_report_handler_version MT32EMU_C_CALL mt32_rh_get_version_id(mt32emu_report_handler_i i)
{
	(void)i;
	return MT32EMU_REPORT_HANDLER_VERSION_0;
}

/* 実時間(CoreAudio)スレッドから発火する。★debug_log を呼んではならない——加算のみ。 */
static void MT32EMU_C_CALL mt32_rh_on_midi_message_played(void *instance_data)
{
	(void)instance_data;
	atomic_fetch_add_explicit(&g_mt32_message_played_count, 1, memory_order_relaxed);
}

/* Synth::playSysex/playMsg 経由(MIDI 投入側 = エミュスレッド)から発火する。
 * 戻り値は既定実装と同じ FALSE 固定(リトライしない、観測のみ)。 */
static mt32emu_boolean MT32EMU_C_CALL mt32_rh_on_midi_queue_overflow(void *instance_data)
{
	(void)instance_data;
	uint32_t n = atomic_fetch_add_explicit(&g_mt32_sysex_overflow_count, 1, memory_order_relaxed) + 1;
	if (n <= MT32_P828_LOG_MAX) {
		debug_log("[P828-MT32SYSEX-OVERFLOW] count=%u\n", (unsigned)n);
	}
	return MT32EMU_BOOL_FALSE;
}

/* 指定した 3 関数以外は NULL(ゼロ初期化)——c_interface.cpp の
 * DelegatingReportHandlerAdapter が各フィールドを NULL チェックして既定実装へ戻す。 */
static const mt32emu_report_handler_i_v0 s_mt32_report_handler_v0 = {
	.getVersionID        = mt32_rh_get_version_id,
	.onMIDIMessagePlayed = mt32_rh_on_midi_message_played,
	.onMIDIQueueOverflow = mt32_rh_on_midi_queue_overflow,
};

static void mt32_store_pending_locked(bool enabled, const char *control, const char *pcm,
                                      int32_t sample_rate, int32_t partial_count, int test_index)
{
	g_mt32_pending_enabled = enabled;
	strlcpy(g_mt32_pending_control_path, control ? control : "", sizeof(g_mt32_pending_control_path));
	strlcpy(g_mt32_pending_pcm_path, pcm ? pcm : "", sizeof(g_mt32_pending_pcm_path));
	g_mt32_pending_sample_rate = sample_rate;
	g_mt32_pending_partial_count = partial_count;
	g_mt32_pending_test_index = test_index;
	atomic_store_explicit(&g_pending_mt32_reconfigure, true, memory_order_release);
}

void mx68k_request_mt32_reconfigure(bool enabled, const char *control_rom_path,
                                    const char *pcm_rom_path, int32_t sample_rate,
                                    int32_t partial_count)
{
	os_unfair_lock_lock(&g_mt32_lock);
	mt32_store_pending_locked(enabled, control_rom_path, pcm_rom_path, sample_rate, partial_count, 0);
	os_unfair_lock_unlock(&g_mt32_lock);
}

static const char *mt32_rc_text(int rc)
{
	switch (rc) {
	case MT32EMU_RC_OK:                    return "ok";
	case MT32EMU_RC_ROM_NOT_IDENTIFIED:    return "ROM not identified (unknown SHA1)";
	case MT32EMU_RC_FILE_NOT_FOUND:        return "file not found";
	case MT32EMU_RC_FILE_NOT_LOADED:       return "file not loaded";
	case MT32EMU_RC_MISSING_ROMS:          return "missing ROMs (need both Control and PCM)";
	case MT32EMU_RC_NOT_OPENED:            return "synth not opened";
	case MT32EMU_RC_QUEUE_FULL:            return "queue full";
	case MT32EMU_RC_ROMS_NOT_PAIRABLE:     return "ROMs not pairable";
	case MT32EMU_RC_MACHINE_NOT_IDENTIFIED:return "machine not identified";
	case MT32EMU_RC_FAILED:                return "failed";
	default:                               return (rc > 0) ? "ok (rom added)" : "unknown error";
	}
}

/* g_mt32_lock 保持中に呼ぶ。ready=false を先に落としてから close → free する。 */
static void mt32_destroy_locked(void)
{
	atomic_store_explicit(&g_mt32_ready, false, memory_order_release);
	if (g_mt32_ctx != NULL) {
		mt32emu_close_synth(g_mt32_ctx);
		mt32emu_free_context(g_mt32_ctx);
		g_mt32_ctx = NULL;
	}
}

/* g_mt32_lock 保持中に呼ぶ。戻り値: MT32EMU_RC_OK または失敗コード(負値)。
 * *stage には失敗した段階名を入れる(ログ用)。 */
static int mt32_open_locked(const char *control, const char *pcm, int32_t sample_rate,
                            int32_t partial_count, const char **stage)
{
	/* P828: SysEx ドロップ観測用のレポートハンドラを渡す(挙動は既定と同じ)。 */
	mt32emu_report_handler_i report_handler = { .v0 = &s_mt32_report_handler_v0 };
	mt32emu_context ctx = mt32emu_create_context(report_handler, NULL);
	if (ctx == NULL) {
		*stage = "create_context";
		return MT32EMU_RC_FAILED;
	}
	/* open_synth より前に設定する必要がある(次回 open 時にのみ反映される)。 */
	mt32emu_set_partial_count(ctx, (mt32emu_bit32u)partial_count);
	/* リバーブモード切替やSysEx受信で実時間スレッド側の動的確保が起きないよう事前確保する
	 * (P825 spec_inv §2 の推奨)。 */
	mt32emu_preallocate_reverb_memory(ctx, MT32EMU_BOOL_TRUE);
	mt32emu_configure_midi_event_queue_sysex_storage(ctx, MT32_SYSEX_STORAGE_SIZE);

	/* 2 ファイル固定選択方式。ファイル名は仮定しない(機種は mt32emu が SHA1 で判定)。 */
	mt32emu_return_code rc = mt32emu_add_rom_file(ctx, control);
	if (rc < 0) {
		*stage = "add_rom_file(control)";
		mt32emu_free_context(ctx);
		return (int)rc;
	}
	rc = mt32emu_add_rom_file(ctx, pcm);
	if (rc < 0) {
		*stage = "add_rom_file(pcm)";
		mt32emu_free_context(ctx);
		return (int)rc;
	}
	mt32emu_set_stereo_output_samplerate(ctx, (double)sample_rate);
	rc = mt32emu_open_synth(ctx);
	if (rc != MT32EMU_RC_OK) {
		*stage = "open_synth";
		mt32emu_free_context(ctx);
		return (int)rc;
	}
	g_mt32_ctx = ctx;
	atomic_store_explicit(&g_mt32_ready, true, memory_order_release);
	*stage = "open_synth";
	return MT32EMU_RC_OK;
}

static void mt32_test_hook_maybe_issue(void)
{
	if (s_mt32_test_total < 0) {
		const char *env = getenv("MX68K_TEST_MT32_RECONF");
		long n = (env != NULL) ? strtol(env, NULL, 10) : 0;
		if (n < 0) n = 0;
		if (n > MT32_TEST_MAX) n = MT32_TEST_MAX;
		s_mt32_test_total = (int)n;
		if (n > 0) {
			debug_log("[P825-MT32-TEST] enabled total=%d\n", s_mt32_test_total);
		}
	}
	if (s_mt32_test_done >= s_mt32_test_total) {
		return;
	}
	os_unfair_lock_lock(&g_mt32_lock);
	/* 実際の(Swift 由来の)要求が未適用なら上書きしない。次のフレーム境界で再試行する。 */
	if (!atomic_load_explicit(&g_pending_mt32_reconfigure, memory_order_acquire)) {
		int idx = s_mt32_test_done + 1;
		bool enabled = (idx % 2) == 1;   /* 1 回目=有効化、2 回目=無効化、…と交互 */
		mt32_store_pending_locked(enabled, "/nonexistent/control.rom", "/nonexistent/pcm.rom",
		                          44100, 32, idx);
	}
	os_unfair_lock_unlock(&g_mt32_lock);
}

void mx68k_mt32_apply_pending_reconfigure(void)
{
	/* P828: 実時間スレッドで数えた onMIDIMessagePlayed 回数を、ここ(エミュスレッド)でログ出力する。
	 * 下の早期 return より前に置き、再構成要求の有無にかかわらず毎フレーム確認する。 */
	{
		uint64_t played = atomic_load_explicit(&g_mt32_message_played_count, memory_order_relaxed);
		if (played != s_mt32_last_logged_played_count && s_mt32_played_log_count < MT32_P828_LOG_MAX) {
			debug_log("[P828-MT32SYSEX-PLAYED] count=%llu\n", (unsigned long long)played);
			s_mt32_last_logged_played_count = played;
			s_mt32_played_log_count++;
		}
	}

	mt32_test_hook_maybe_issue();

	if (!atomic_load_explicit(&g_pending_mt32_reconfigure, memory_order_acquire)) {
		return;
	}

	/* ログは lock の外で出す(debug_log は mutex + ファイル I/O を伴い、その間 audio
	 * スレッドの trylock を無駄に失敗させないため)。 */
	char control[sizeof(g_mt32_pending_control_path)];
	char pcm[sizeof(g_mt32_pending_pcm_path)];
	const char *stage = "-";
	int rc = MT32EMU_RC_OK;

	os_unfair_lock_lock(&g_mt32_lock);
	atomic_store_explicit(&g_pending_mt32_reconfigure, false, memory_order_release);
	bool    enabled     = g_mt32_pending_enabled;
	int32_t sample_rate = g_mt32_pending_sample_rate;
	int32_t partial_count = g_mt32_pending_partial_count;
	int     test_index  = g_mt32_pending_test_index;
	memcpy(control, g_mt32_pending_control_path, sizeof(control));
	memcpy(pcm, g_mt32_pending_pcm_path, sizeof(pcm));

	mt32_destroy_locked();
	if (enabled) {
		rc = mt32_open_locked(control, pcm, sample_rate, partial_count, &stage);
	}
	bool ready = atomic_load_explicit(&g_mt32_ready, memory_order_acquire);
	atomic_store_explicit(&g_mt32_last_status,
	                      !enabled ? 0 : (ready ? 1 : (rc < 0 ? rc : MT32EMU_RC_FAILED)),
	                      memory_order_relaxed);
	os_unfair_lock_unlock(&g_mt32_lock);

	if (test_index > 0) {
		s_mt32_test_done = test_index;
		debug_log("[P825-MT32-TEST] reconf#%d enabled=%d rc=%d stage=%s ready=%d\n",
		          test_index, enabled ? 1 : 0, rc, stage, ready ? 1 : 0);
		return;
	}
	if (!enabled) {
		debug_log("[P825-MT32] disabled (context released)\n");
	} else if (rc == MT32EMU_RC_OK) {
		debug_log("[P825-MT32] opened rate=%d control='%s' pcm='%s'\n",
		          (int)sample_rate, control, pcm);
	} else {
		debug_log("[P825-MT32] open failed at %s rc=%d (%s) control='%s' pcm='%s'\n",
		          stage, rc, mt32_rc_text(rc), control, pcm);
	}
}

int32_t mx68k_mt32_get_status(void)
{
	if (atomic_load_explicit(&g_pending_mt32_reconfigure, memory_order_acquire)) {
		return 2;
	}
	return atomic_load_explicit(&g_mt32_last_status, memory_order_relaxed);
}

void mt32_bridge_send_bytes(const uint8_t *data, uint32_t len)
{
	if (!atomic_load_explicit(&g_mt32_ready, memory_order_acquire)) {
		return;
	}
	/* P828: SysEx(先頭 0xF0)の送信長を記録する(分母)。エミュスレッド専用経路なので debug_log 可。 */
	if (len > 0 && data[0] == 0xF0) {
		uint32_t n = atomic_fetch_add_explicit(&g_mt32_sysex_send_count, 1, memory_order_relaxed) + 1;
		if (n <= MT32_P828_LOG_MAX) {
			debug_log("[P828-MT32SYSEX-SEND] len=%u byte0=0x%02x\n", (unsigned)len, data[0]);
		}
	}
	mt32emu_parse_stream(g_mt32_ctx, data, len);
}

void mx68k_mt32_render_mix(int16_t *stereo_buf, int32_t frames)
{
	if (stereo_buf == NULL || frames <= 0) {
		return;
	}
	if (!os_unfair_lock_trylock(&g_mt32_lock)) {
		return;   /* 再構成中。このバッファだけ MT-32 無音(ブロックしない) */
	}
	if (atomic_load_explicit(&g_mt32_ready, memory_order_acquire) && g_mt32_ctx != NULL) {
		int32_t done = 0;
		while (done < frames) {
			int32_t n = frames - done;
			if (n > MT32_MIX_CHUNK_FRAMES) {
				n = MT32_MIX_CHUNK_FRAMES;
			}
			mt32emu_render_bit16s(g_mt32_ctx, s_mt32_mix_buf, (mt32emu_bit32u)n);
			int16_t *dst = stereo_buf + (size_t)done * 2;
			for (int32_t i = 0; i < n * 2; i++) {
				int32_t s = (int32_t)dst[i] + (int32_t)s_mt32_mix_buf[i];
				if (s > INT16_MAX) s = INT16_MAX;
				if (s < INT16_MIN) s = INT16_MIN;
				dst[i] = (int16_t)s;
			}
			done += n;
		}
	}
	os_unfair_lock_unlock(&g_mt32_lock);
}

void mx68k_mt32_shutdown(void)
{
	os_unfair_lock_lock(&g_mt32_lock);
	atomic_store_explicit(&g_pending_mt32_reconfigure, false, memory_order_release);
	mt32_destroy_locked();
	atomic_store_explicit(&g_mt32_last_status, 0, memory_order_relaxed);
	os_unfair_lock_unlock(&g_mt32_lock);
}
