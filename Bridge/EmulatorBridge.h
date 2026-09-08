#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---- Initialization & Shutdown ----
int  mx68k_init(void);
void mx68k_shutdown(void);

// ---- BIOS Path Setup (call before init) ----
void mx68k_set_bios_path(const char* iplrom_path, const char* cgrom_path);
void mx68k_set_bios_path_030(const char* iplrom30_path);

// ---- Hardware Settings (call before init or reset) ----
void mx68k_set_machine_type(int type);
void mx68k_set_memory_size(int mb);
void mx68k_set_clock(int mhz);
void mx68k_set_fpu_enabled(bool en);
// P483: Mercury Unit(MK-MU1 / $ECC000)の装着設定。既定 false(未装着)。
// mx68k_set_machine_type と同じく「設定値」と「配線確定値」を分離する:
// 装着はハードリセット(mx68k_reset_hard)で初めて確定し、それまでは
// $ECC000-$ECDFFF は未装着として bus error のまま(P221d の忠実性を維持)。
void mx68k_set_mercury_enabled(bool enabled);
// P686 (D-70): 外付けFDDユニット(ドライブ2/3 = C:/D:)の装着設定。既定 false。
// Mercury / MIDI と同じく「設定値」と「配線確定値」を分離し、装着は
// init / ハードリセットで初めて確定する。OFF のとき Core の Recalibrate は
// ドライブ2/3へ EC=1(未接続)を返し、Human68k に C:/D: は現れない
// (テクニカルデータブック 印刷 p.157 5-5(3))。
void mx68k_set_ext_fdd_enabled(bool enabled);
// P488: MIDIボード(CZ-6BM1)の装着設定。既定 false。
// Mercury Unitと割込みレベル4を共有するため、両方の同時装着はCore側で未サポート
// (IRQH_CallBack[8]はレベルあたり1コールバックのみ、Core改変禁止のため多重化しない)。
// 相互排他はSwift UI側(.disabled())+Bridge側(本ラッチ処理)の二重ガードで実現する。
void mx68k_set_midi_enabled(bool enabled);
// P493: 内蔵SRAM 64KB化(実機改造相当)。既定 false = 標準16KB。
// Mercury/MIDI と同じく「設定値」であり、配線($ED4000-$EDFFFF を Bridge 所有
// バッファへ振り向ける)はハードリセット(mx68k_reset_hard)で確定する。
// false のときの上位48KBの挙動は従来どおり(read=0xFF・write無視)。
void mx68k_set_sram_64k_enabled(bool enabled);
// P642: Windrv(Mac のフォルダを X68000 ゲストから 1 台のドライブとして見せる
// ホスト共有機能)。既定 false = 未装着。Mercury/MIDI/SRAM64K と同じく「設定値」で
// あり、配線($E9F000/$E9F001 の応答開始とマウントルートの realpath 正規化)は
// ハードリセット(mx68k_reset_hard)で確定する。
// ★この設定だけでは読み取り専用 — 書込み・作成・削除・改名は
//   mx68k_set_windrv_write_enabled(true) を併用して初めて有効になる(P647)。
// ★フォルダ未選択(mx68k_set_windrv_host_path が空文字)または実在しない場合は
//   enabled=true でも未装着へ倒れる($E9E000-$E9FFFF は従来どおりバスエラー)。
void mx68k_set_windrv_enabled(bool enabled);
// P647: Windrv 書込み許可。mx68k_set_windrv_enabled とは **独立したトグル**で、
// 既定 false。true にして初めて $49 Create / $4D Write / $45 Delete /
// $44 Rename / $42 MakeDir / $43 RemoveDir が動作する(false のときは
// 従来どおり FS_CANTWRITE を返す)。共有自体が無効なら本設定は無意味。
// 配線はハードリセット(mx68k_reset_hard)で確定する。
void mx68k_set_windrv_write_enabled(bool enabled);
// 設定値ではなくラッチ後の配線確定値。設定 UI の状態表示に使う。
bool mx68k_get_windrv_write_wired(void);
// 共有する Mac フォルダの絶対パス。NULL / 空文字 = 未選択(= 機能しない)。
// 1023 バイトを超えるパスは未選択として扱う(切り詰めた別ディレクトリを
// 黙って共有しないため)。
void mx68k_set_windrv_host_path(const char* path);
// 設定値ではなくラッチ後の配線確定値。設定 UI が「ハードリセット待ち」を
// 表示するために使う(P457 の wired 値基準 UI ゲートと同じ方針)。
bool mx68k_get_windrv_installed(void);
// P626 (D-62): Core 側音声チップ(ADPCM / OPM / Mercury)の実初期化レートを設定する。
// 機種選択 / メモリ容量 / FPU / BIOS パスと同じく、ハードリセット前に呼ぶこと。
// 次回の mx68k_reset_hard() または mx68k_init() から反映される。
// P627: 受理する値は {22050, 44100, 48000, 62500, 88200, 96000} の 6 値。
// これ以外は安全側として 44100 へ丸める。このホワイトリストが唯一の真実源であり、
// Swift 側は同じ値集合を複製せず mx68k_get_audio_sample_rate() で読み戻すこと。
void mx68k_set_audio_sample_rate(int hz);
// P627: 上で実際に適用されたサンプルレートの読み戻し。Swift 側は AudioUnit の
// 出力レートをこの値に追従させる(mx68k_set_audio_sample_rate 呼出し後に読むこと)。
int mx68k_get_audio_sample_rate(void);
// P490: MIDIボードが実際に配線済みか(設定値ではなくラッチ後の確定値)。
// 「未配線(ハードリセット待ち)」と「配線済みだが実デバイス0台」を区別するために使う。
bool mx68k_midi_is_wired(void);
// P490 (MIDI Stage 2): リセット送信 / 音源種別 / 送信遅延 / 入出力デバイス選択。
// リセット送信・音源種別・デバイス選択は「設定値」で、配線はハードリセットで確定する
// (mx68k_set_midi_enabled と同型)。送信遅延のみフレームループが毎回読むため即時反映。
void mx68k_set_midi_reset_enabled(bool enabled);
void mx68k_set_midi_reset_type(int type);   // 0=LA, 1=GM, 2=GS, 3=XG。範囲外は無視。
void mx68k_set_midi_delay_ms(int ms);       // 0..1000 にクランプ。
// デバイス一覧は MIDI 装着 + init/ハードリセット後にのみ有効(未装着時は count=0)。
int  mx68k_midi_get_output_device_count(void);
bool mx68k_midi_get_output_device_name(int idx, char* buf, int len);
void mx68k_midi_set_output_device(int idx);
int  mx68k_midi_get_input_device_count(void);
bool mx68k_midi_get_input_device_name(int idx, char* buf, int len);
void mx68k_midi_set_input_device(int idx);

/* ---- P693: MIDI Viewer(MIDIボード状態モニタ)向けの読み出し API ---- */
/* 送受信の累積カウンタ(セッション累積、リセットされない)。実体は
 * Bridge/midi_coremidi.c、宣言は Bridge/midi_shadow.h。いずれも atomic
 * スカラを 1 個読むだけで生ポインタの逆参照を伴わないため、呼び出し側で
 * ロックを取る必要は無い(P692 ストレージモニタと異なる点)。
 *
 * ★分母/分子の対として必ず並べて表示すること:
 *   tx_messages はゲストが送信を試みた回数(CoreMIDI デバイス未オープンでも
 *   計上される)、tx_bytes は実際に CoreMIDI へ渡せたバイト数。
 *   tx_messages>0 かつ tx_bytes==0 なら送出層で落ちている、tx_bytes>0 なのに
 *   無音なら外部機器側 —— という切り分けを 1 画面で成立させるための設計。
 * ★rx_messages の単位は CoreMIDI パケット(1 パケットに複数メッセージが
 *   載りうるので厳密なメッセージ数ではない)。rx_bytes の分母として使う。 */
uint64_t mx68k_midi_get_tx_messages(void);
uint64_t mx68k_midi_get_tx_bytes(void);
uint64_t mx68k_midi_get_rx_messages(void);
uint64_t mx68k_midi_get_rx_bytes(void);
/* 直近 1 メッセージのパック値: status | len<<8 | data1<<16 | data2<<24。
 * len は min(実長, 0xff) にクランプされた表示用の目安値(SysEx は最大 1024
 * バイトまで来るため実長そのものではない)。data1/data2 は実長がそこまで
 * 届いていない場合 0(呼び出し元バッファの外は読まない)。 */
uint32_t mx68k_midi_get_tx_last(void);
uint32_t mx68k_midi_get_rx_last(void);

/* YM3802($EAFA00-$EAFA0F)のレジスタ状態スナップショット。
 * ★ここに入れる 7 項目はいずれもエミュレーションスレッドだけが書く値で、
 *   読み出しも同じエミュレーションスレッド(fetchMonitorsAndPerfStats)から
 *   行うためロック不要。
 * ★MIDI_IntFlag / MIDI_IntVect は意図的に含めない —— この 2 つは
 *   mid_In_callback()(CoreMIDI コールバックスレッド)からも書かれており、
 *   エミュレーションスレッド側の書込みと無同期の read-modify-write 競合に
 *   なっている(Docs/09 の D-73、本サイクルのスコープ外)。既知の競合に
 *   読み手を足さないため除外する。 */
typedef struct {
    uint8_t  reg_high;      /* MIDI_RegHigh — レジスタバンク選択(R00 下位4bit) */
    uint8_t  vector;        /* MIDI_Vector — 割込みベクタ上位(R02 の data&0xe0) */
    uint8_t  int_enable;    /* MIDI_IntEnable — 割込み許可(R04) */
    uint8_t  r05;           /* MIDI_R05 */
    uint8_t  r35;           /* MIDI_R35 — bit0=Rx-FIFO 受信許可 */
    uint8_t  r55;           /* MIDI_R55 */
    uint32_t tx_fifo_used;  /* MIDI_Buffered — Tx FIFO に積まれている段数 */
} MX68K_MIDIRegs;

void mx68k_get_midi_regs(MX68K_MIDIRegs* out);

// P483: [P483-MCRYACC] サマリを出力する(m68000_bridge.c に実体)。
// mx68k_reset_hard() と mx68k_shutdown() から呼ぶ。
void mx68k_p483_dump_mcry_counters(const char* tag);

// ---- Emulation Control ----
void mx68k_reset_hard(void);
void mx68k_reset_soft(void);
void mx68k_nmi(void);
void mx68k_pause(bool pause);

// ---- Frame Execution ----
void mx68k_run_frame(void);
/* P503 (c1): consume any queued frame-boundary operation (SRAM clear, hard/soft
 * reset, SASI fd cache invalidation, state save/load) WITHOUT running a frame of
 * CPU. The Swift frame driver calls this instead of mx68k_run_frame() while the
 * emulator is paused (settings sheet open), so a queued reset / Clear SRAM takes
 * effect at the next display-link callback rather than waiting for the sheet to
 * close. Same thread as mx68k_run_frame(). */
void mx68k_pump_pending(void);

// ---- Video ----
const uint8_t* mx68k_get_framebuffer(int* width, int* height);
/* P595 (D-55): 公開フレームと同一スナップショットの表示ジオメトリを返す。
 * 単位は「標準表示窓=1.0」。標準ラスタでは h_scale=v_scale=1.0, off=0.0(P212と同一)。
 * 引数は全てNULL可。mx68k_get_framebuffer() はこの関数の薄いラッパ。 */
const uint8_t* mx68k_get_framebuffer_geom(int* width, int* height,
                                          float* h_scale, float* v_scale,
                                          float* off_x, float* off_y);
/* P595 (D-55): 上記ジオメトリの由来を示す診断用モード値(モニタ表示専用)。
 * 0=恒等 / 1=標準R00・R04(P571由来の窓ずらしオフセット) /
 * 2=非標準R00(対称中央寄せ) / 9=妥当性ゲート外またはR04非標準(恒等へフォールバック)。 */
int mx68k_get_display_geo_mode(void);

// ---- FDD ----
int  mx68k_fdd_insert(int drive, const char* path);
void mx68k_fdd_eject(int drive);
/* P271: FDライトプロテクト。protect!=0 で FDD_SetReadOnly(drive) を呼ぶ(一方向セット)。
 * protect==0 は no-op(Core に解除関数が無く、eject でのみ ROnly がクリアされるため)。
 * 挿入直後に呼ぶことを想定。詳細は EmulatorBridge.c の実装コメント参照。 */
void mx68k_fdd_set_write_protect(int drive, int protect);
bool mx68k_fdd_is_write_protected(int drive);
bool mx68k_fdd_is_inserted(int drive);
/* P160: 1 if the drive was accessed (read/seek) within the last few frames —
 * drives the status-bar "accessing" (red) indicator so disk activity is visible
 * even while the emulated screen is blank. */
int  mx68k_fdd_accessing(int drive);
/* P443 (D-7): 1 if media is currently present in the drive, tracked from Core's
 * StatBar_ParamFDD() notifications — so a guest-initiated eject (Core fdd.c
 * FDD_EjectFD) is reflected too. Unlike FDD_IsReady() this has no post-insert
 * SetDelay grace window, so it is safe to sample from the 1 Hz status poll. */
int  mx68k_fdd_media_present(int drive);
bool mx68k_fdd_is_active(int drive);
const char* mx68k_fdd_get_path(int drive);
/* P557: FD アクセス高速化(XM6 の Config::floppy_speed「フロッピーディスク高速化」
 * 相当の ON/OFF)。0 = 既定 OFF で Core/px68k/x68k/fdd.c のシーク/読込/書込の実時間
 * ウェイトは従来どおり(= P557 以前と完全に同一)。1 = ON で XM6 の fast mode 相当の
 * 64µs 固定へ短縮する。クロック速度と同じく即時反映(リセット不要)。
 * ★FD 回転タイミングに依存するコピープロテクトを持つタイトルでは ON にすると
 * 挙動が変わり得るため、既定は OFF。実体は Bridge/fdd_timing_shim.h(fdd.c 専用の
 * -include)経由のマクロ差し替えで、Core は無改変。 */
void mx68k_set_fd_fast_access(int enabled);
void mx68k_schedule_hard_reset(void);
void mx68k_schedule_soft_reset(void);

// ---- HDD (SASI .hdf) ----
/* P455: MX が公開する SASI 論理ユニット数。実機 SASI バスの device ID 0-7
 * (Inside X68000 p.429「バス上に最大 8 つのコントローラ」)に対応する。
 * 論理 unit n -> Config.HDImage[n*2](LUN0 スロット)。
 * ★8 が上限である一次拘束: Core sasi.c:426 の在席判定が
 *   Config.HDImage[dev*2+1] まで読むため、dev=7 で index 15 に達し、
 *   prop.h:19 の HDImage[16] をちょうど使い切る。9 以上は OOB。 */
#define MX68K_SASI_UNIT_COUNT 8
// Logical unit 0..7 maps to SASI device ID 0..7 (physical Config.HDImage index
// unit*2 — LUN1 slots are unused). Human68k probes the SASI bus per device ID,
// so an additional drive must be a distinct device (index 2, 4, …), not LUN1
// (odd index), or it stays invisible (P200 hands-on finding). P201: insert/eject
// only set/clear the path; the IPL re-probes the SASI bus on the next manual
// reset (Cmd+R) — no automatic restart. Changes apply after the user resets.
// Returns 0=ok, <0=error:
//   -1 unit out of range / NULL path / path too long for Config.HDImage[]
//   -2 refused because the wired machine type is SCSI (P268)
//   -3 stat failed, or the file size is not a valid SASI capacity (10/20/40MB) (P458)
//   -4 refused because the image starts with the "X68SCSI1" signature, i.e. it is
//      SCSI-formatted and was inserted into a SASI slot (P687 / D-29 B). The
//      reverse direction (a SASI image inserted into a SCSI slot) is NOT detected:
//      the absence of a signature has several legitimate interpretations, so it
//      cannot serve as a rejection condition.
int  mx68k_hdd_insert(int unit, const char* path);
// P503 (b): returns 0=ok, <0=error — -1 unit out of range, -2 refused because the
// wired machine type is SCSI (symmetric with mx68k_hdd_insert()'s P268 gate).
int  mx68k_hdd_eject(int unit);
bool mx68k_hdd_is_inserted(int unit);
// P447 (C2'): record the config-persisted SASI HDD path in the Bridge-side shadow
// that mx68k_reset_hard() re-applies to Config.HDImage[unit*2] once the machine
// type is wired. Call it unconditionally from pushConfig (NO machine-type gate on
// the Swift side — the Bridge decides). unit 0..7; NULL or "" clears the slot.
// mx68k_hdd_insert/_eject update the same shadow themselves, so the immediate-
// effect settings-screen path (P239) stays in sync without any Swift change.
void mx68k_set_hdd_path(int unit, const char* path);
/* P201: 1 if the SASI/HDD bus was accessed within the last few frames — drives
 * the status-bar "HD BUSY" indicator. */
int  mx68k_hdd_accessing(void);
/* P269: HDD mount lamp — internal SCSI (SPC, ID0) present under the P268 triple
 * gate, and external CZ-6BS1 (any ID 0..6) mounted. Both OR into hdd0_inserted. */
bool mx68k_scsi_in_disk_present(void);
bool mx68k_scsi_ext_is_inserted(void);
/* P269/P510: external SCSI ($EA0000, MemTable index 0x50) install.
 * Gate not satisfied → the P269 observation hook (pure pass-through to Core
 * SCSI_Read/Write, HDD busy pulse on DREG access), exactly as before.
 * Gate satisfied (ext_wired && ext_rom_loaded) → a sub-decode dispatcher:
 * offset 0x0000-0x001F (SPC registers) goes to the ported XM6 MB89352
 * implementation, offset 0x0020-0x1FFF (ROM) stays on the Core path — the
 * Core SCSIIPL[] buffer is stored LE16-swapped and scsi.c compensates with
 * `adr^1`, a convention the ported implementation does not share.
 * wired_machine_type is passed for the log denominator only. */
void scsi_ext_bridge_install(int ext_wired, int ext_rom_loaded, int wired_machine_type);
/* P510: $EA000D (SSTS) read total — the denominator for [P510-SCTL]. */
uint32_t p510_scsi_ext_ssts_read_count(void);

// ---- SCSI (external CZ-6BS1) ----
// SCSI ID 0..6 maps directly to Config.SCSIEXHDImage[id] (7=host reserved).
// insert/eject only set/clear the path. Requires SCSIEXROM.DAT loaded + a SCSI
// boot image carrying the "X68SCSI1" signature to actually boot.
// P692: the previous wording here ("scsi.c re-opens per block access (no cache)
// — no reset needed") has been stale since P510. When the external board is
// actually wired (ext_wired && ext_rom_loaded), $EA0000 offsets 0x00-0x1F are
// dispatched to the ported XM6 SPC, so Core scsi.c's SCSI_BlockRead/Write
// per-block File_Open path is no longer reached; the real disk is opened once,
// inside scsi_real_install_construct(), and its fd is held by DiskCache. A path
// changed afterwards therefore takes effect only on the next hard reset (⌘R) —
// the same rule as internal SCSI. The old "live, no reset needed" behaviour
// survives only when the wiring gate is NOT satisfied (i.e. when external SCSI
// does not actually function).
int  mx68k_scsi_insert(int id, const char* path);  // 0=ok, <0=error
void mx68k_scsi_eject(int id);
bool mx68k_scsi_is_inserted(int id);
void mx68k_set_scsi_ext_rom_path(const char* path); // optional; absent = ext SCSI disabled

// ---- Internal SCSI (SUPER+ built-in SPC MB89352) — P247 Stage 1 skeleton ----
// Stage 1: path/ROM held in Bridge only; NOT yet wired to real I/O. The actual
// SPC (MB89352) transfer logic and $FC0000 IPL mapping arrive in Stage 2/4.
// mx68k_scsi_in_insert/eject/is_inserted only set/clear a held path (id=SCSI
// ID 0..6). mx68k_set_scsi_in_rom_path loads SCSIINROM.DAT (8KB) into a static
// Bridge buffer (held only, not mapped). These have no runtime effect yet.
int  mx68k_scsi_in_insert(int id, const char* path);  // 0=ok, <0=error
void mx68k_scsi_in_eject(int id);
bool mx68k_scsi_in_is_inserted(int id);
void mx68k_set_scsi_in_rom_path(const char* path);
void scsi_in_bridge_install(int machine_type); // gate: installs index0x4b passthrough when machine_type==4 (SCSI)
// P447 Phase 1 diagnostic probe [P447-SASIWIRE]. Emits ONE unconditional line per
// hard reset holding the raw MemRead/MemWriteTable[0x4b] function-pointer values
// alongside every candidate handler address (the denominator), plus the SASI
// slot occupancy over all MX68K_SASI_UNIT_COUNT units (hdimg_map/n_hdimg — P455;
// hdimg0=/hdimg2= are kept only as a grep-compatible subset, never the
// denominator). Lives in scsi_in_bridge.c because the candidate
// handlers there have internal linkage. Call it from mx68k_reset_hard() right
// after the wiring pair runs and BEFORE g_wired_machine_type is overwritten
// (wired_prev must carry the previous session's machine type).
void scsi_in_bridge_log_slots(int machine_type, int wired_prev);
void scsi_real_install_teardown(void);   // P275: 電源OFF時に内蔵SCSIを解放(次回電源ONで新ディスクパス反映)
// P253 Stage 2d: internal-SCSI HD disk-image path (id=SCSI ID 0..6; ID0 wired).
// Takes effect on the next hard reset (scsi_real_install_construct pulls it in
// before Reset()->Construct() opens the disk). mx68k_get_machine_type returns
// g_machine_type (4 == SCSI); used by the $FC0000 fetch overlay helper.
void        mx68k_set_scsi_in_disk_path(int id, const char* path);
const char* mx68k_get_scsi_in_disk_path(int id);
int         mx68k_get_machine_type(void);
// P457 (D-34): 配線確定機種(実際に mx68k_reset_hard() が走って初めて更新される
// g_wired_machine_type)を Swift 側から読み取るための読み取り専用 getter。
// mx68k_get_machine_type() が返すのは設定直後の pending 値(Apply で即時変化)で
// あるのに対し、こちらは「今まさに配線されている機種」を返す。4 == SCSI。
// 設定 UI の有効/無効判定をこちらへ揃えることで、機種切替+Apply 直後に
// 「操作できるのに挿入は必ず拒否される」窓(P268 由来)を解消する。
int         mx68k_get_wired_machine_type(void);

// ---- P692: live SPC state accessors for the Storage Monitor (read-only) ----
// Every other SCSI/MO/CD getter above returns a Bridge-side shadow (the pending
// configuration), which drifts from what the running SPC actually has open until
// the next hard reset. These four read the running instance itself
// (s_scsi_instance->GetSCSI()) and are the single source of truth for "what is
// mounted right now". Defined in scsi_spc_bridge.cpp.
//   id is 0..6; out-of-range, no SPC instance, or an empty slot all return the
//   documented safe default (mask bit clear / kind -1 / ready false / path "").
//   kind: 0=HD, 1=MO, 2=CD, -1=not attached.
// ★mx68k_scsi_live_path() returns a pointer to a SHARED static buffer that the
//   next call overwrites — the caller must copy the string before calling again.
// ★These dereference Disk* objects that MO/CD live insert/eject deletes from the
//   main thread, so callers on the emulation thread must hold the same lock that
//   brackets mx68k_run_frame() (Swift: EmulatorEngine.emulationLock).
uint32_t    mx68k_scsi_live_attached_mask(void);
int         mx68k_scsi_live_kind(int id);
bool        mx68k_scsi_live_ready(int id);
const char* mx68k_scsi_live_path(int id);

// ---- SCSI MO (magneto-optical) — P668, dedicated slot at SCSI ID5 ----
// A single dedicated MO slot at SCSI ID5, matching XM6's default layout
// (HD on ID0-4, MO on ID5, CD on ID6, initiator on ID7 — XM6:vm/scsi.cpp:3952-3995).
// The path is held in a Bridge-side shadow and pulled into the ported SPC by
// scsi_real_install_construct() -> SCSI::SetMOPath() -> Reset() -> Construct().
// P674: when the MO drive is ALREADY attached (a MO path was set at the time of
// the last hard reset), insert/eject now take effect LIVE — SCSI::Open()/Eject()
// swap only the medium, leaving the drive itself on the bus, and the guest is
// notified via UNIT ATTENTION. When no MO drive is attached yet (no MO path at
// boot), the shadow is updated as before and takes effect on the NEXT HARD RESET
// (Cmd+R), the same as the internal SCSI HD path (P450/D-30).
// If SCSI ID5 already carries a hard-disk image, the HD keeps ID5 and the MO is
// NOT attached (a configured HD is never displaced); the Swift settings UI
// refuses the MO selection in that case, and Construct() logs the conflict.
// ★Guest-side requirement (NOT an MX defect — real hardware behaves the same):
// the real SCSIINROM.DAT contains only the SCSI IOCS + boot routine, no Human68k
// block-device driver, so a resident SCSI device driver such as SUSIE.X must be
// loaded on the guest for the MO to get a drive letter. The user supplies it.
int         mx68k_mo_insert(const char* path);
// 0=live (took effect immediately) / 1=shadow only (takes effect on the next hard reset, Cmd+R)
// / -1=bad argument / -3=size is not one of the 4 standard MO capacities
// / -4=open failed (live path) / -5=the guest has the medium locked (live path)
int         mx68k_mo_eject(void);
// 0=live (took effect immediately) / 1=shadow only (takes effect on the next hard reset, Cmd+R)
// / -5=the guest has the medium locked (live path)
bool        mx68k_mo_is_inserted(void);
void        mx68k_set_mo_path(const char* path); // pushConfig path; no size validation (mirrors mx68k_set_hdd_path)
const char* mx68k_get_mo_path(void);

// ---- P676: SCSI CD-ROM (ID6 fixed slot) ----
// Mounts a host CD-ROM image file (ISO / Mode1, 2048B or RAW 2352B sectors) on
// SCSI ID6. Structurally identical to the MO path above (P668/P674); the drive
// is created by Construct() from the shadow path and, once attached, insert /
// eject take effect LIVE via SCSI::Open()/Eject() with the drive left on the
// bus (the guest is notified via UNIT ATTENTION). When no CD drive is attached
// yet (no CD path at boot), the shadow is updated and takes effect on the NEXT
// HARD RESET (Cmd+R).
// If SCSI ID6 already carries a hard-disk image, the HD keeps ID6 and the CD is
// NOT attached (a configured HD is never displaced; XM6's "fall back to ID7"
// behaviour is deliberately NOT adopted because MX's ID7 is the initiator and
// HDMax=7 (P576/D-52) is incompatible with it). The Swift settings UI refuses
// the CD selection in that case, and Construct() logs the conflict.
// Size validation is a RANGE check, not the MO's exact-capacity match:
// a multiple of 2352 up to 912579600, or a multiple of 2048 up to 0x2bed5000.
// Mode1 verification (the sector header's mode byte) is NOT done here — it is
// SCSICD::OpenIso's job, and duplicating it would diverge from the XM6 port.
// ★Guest-side requirement (NOT an MX defect — real hardware behaves the same):
// a resident SCSI device driver such as SUSIE.X must be loaded on the guest for
// the CD-ROM to get a drive letter. The user supplies it.
// Scope: data (Mode1) tracks only — CD-DA audio playback and booting from CD
// are out of scope and not implemented (upstream XM6 leaves CD-DA unimplemented
// as well).
int         mx68k_cd_insert(const char* path);
// 0=live (took effect immediately) / 1=shadow only (takes effect on the next hard reset, Cmd+R)
// / -1=bad argument / -3=size is not a valid CD-ROM image size
// / -4=open failed (live path) / -5=the guest has the medium locked (live path)
int         mx68k_cd_eject(void);
// 0=live (took effect immediately) / 1=shadow only (takes effect on the next hard reset, Cmd+R)
// / -5=the guest has the medium locked (live path)
bool        mx68k_cd_is_inserted(void);
void        mx68k_set_cd_path(const char* path); // pushConfig path; no size validation (mirrors mx68k_set_mo_path)
const char* mx68k_get_cd_path(void);

// ---- P450: Memory-switch auto-update + SASI-side memory-switch clear ----
// P450: メモリスイッチ自動更新(XM6「メモリスイッチ自動更新」相当)。既定 true。
// ON  = ハードリセットのたびに、SRAM のメモリスイッチ $ED006F/$ED0070/$ED0071 を
//       現在の機種/ボード構成へ合わせて更新する。
// OFF = MX はこの3バイトに一切書き込まない。
// ★RAM サイズ($ED0008-0B, P220)はこの設定の対象外(従来どおり常時反映)。
void mx68k_set_memsw_auto_update(bool enabled);      /* 1 */
bool mx68k_get_memsw_auto_update(void);              /* 2 */

// P450: 外付 CZ-6BS1 SCSI ボードが「装着」設定か(scsiMode=="external")。
// XM6 の Memory::GetMemType()==SCSIExt に相当する構成情報。Bridge は scsiMode を
// 知らないため Swift から push する。ディスク在席・ROM ロード有無とは無関係
// (=「使えるか」ではなく「装備されているか」)。既定 false。
void mx68k_set_scsi_ext_board_installed(bool installed);   /* 3 */

// P450: XM6 SASI::Reset() 相当のメモリスイッチクリア(実体は sasi_bridge.c)。
// scsi_present は「SCSI I/F が装備されているか」= 機種/ボード構成のみで決まる述語
// (XM6 sasi.scsi_type と同じ意味論)。判定は呼び出し側(mx68k_reset_hard)が行い、
// この関数は渡された bool に従うだけでロジックを持たない。
void sasi_bridge_apply_memsw(int machine_type, bool memsw_enabled, bool scsi_present); /* 4 */

// P456 (D-33): XM6 SASI::Reset() 相当の $ED005A(SASI ドライブ index の排他的
// 上限値)自動同期。実体は sasi_bridge.c。同じ「メモリスイッチ自動更新」
// チェックボックス配下だが、sasi_bridge_apply_memsw() とは**別関数・別述語**。
// ★sasi_if_present は p450_scsi_present とは別の述語である。
//   $ED005A      : XM6 の scsi_type < 2  = 「SASI I/F を搭載しているか」
//                  → 呼び出し側は (g_wired_machine_type != 4) 単独で判定する。
//   $ED006F/70/71: XM6 の scsi_type == 0 = 「SCSI I/F が一つも無いか」
//                  → p450_scsi_present(内蔵SCSI機 OR 外付CZ-6BS1)の OR 合成。
//   外付 CZ-6BS1 装着の SASI 機(scsi_type==1)は SASI と共存する構成なので
//   $ED005A は書かねばならない。p450_scsi_present を流用すると全 SASI が沈黙する
//   (Docs/09 D-30 の「将来 Pxx のレビュー必須条件」に抵触)。
// unit_mask は SASI unit 0..7 の在席ビットマスク(bit u = unit u)。在席依存は
// $ED005A の「値算出」に閉じており、p450_scsi_present には一切波及しない。
void sasi_bridge_apply_sasi_count(int machine_type, bool memsw_enabled,
                                  bool sasi_if_present, uint8_t unit_mask); /* 4b */

// P450: [P450-SCSIRECON] ログの自己反証性のため。teardown の直前に呼ぶ。
// 「teardown が走らなかった」が「インスタンスが無かった」のか「呼ばれなかった」
// のかを、ログ行単体で区別できるようにする。
bool scsi_real_instance_exists(void);                /* 5 */

// ---- P510 (D-32): 移植済み XM6 SPC 実装へ渡す SCSI 構成モード ----
// ★この値空間は g_machine_type(0=SASI / 4=SCSI)とも Memory::memtype
//   (None=0 / SASI=1 / SCSIInt=2 / SCSIExt=3)とも数値が異なる第 3 の空間である。
//   生 int で受け渡すと定数取り違えを誘発するため、必ずこの名前付き定数を使うこと。
//   対応関係: SASI → Memory::SASI → SCSI::Reset() の scsi.type=0
//             EXT  → Memory::SCSIExt → scsi.type=1(外付 CZ-6BS1)
//             INT  → Memory::SCSIInt → scsi.type=2(内蔵 SCSI 機)
enum {
    P510_SCSI_MODE_SASI = 0,
    P510_SCSI_MODE_EXT  = 1,
    P510_SCSI_MODE_INT  = 2
};
// 実体は scsi_spc_bridge.cpp。★呼び出しは mx68k_reset_hard() 内の
// scsi_in_bridge_install() より**前**に 1 回だけ、3 モードのいずれかを必ず
// 明示設定すること(片道書換え = P447/D-31 型欠陥の回避)。scsi_in_bridge_install()
// はその場で scsi_real_install_construct() → SCSI::Reset() を走らせ、内部で
// Memory::GetMemType() を読むため、後から設定したのでは間に合わない。
void scsi_real_set_scsi_mode(int mode);
int  scsi_real_get_scsi_mode(void);       /* [P510-SCTL] の標本素性用(生値) */
uint32_t scsi_real_get_sctl(void);        /* SPC の SCTL レジスタ生値(未構築時 0xFFFFFFFF) */
// P510: 外付 CZ-6BS1 のディスクイメージパス(Config.SCSIEXHDImage[id])。
// mx68k_get_scsi_in_disk_path() と対で、scsi_real_install_construct() が
// モードに応じてどちらかを引く。id=SCSI ID 0..6。
const char* mx68k_get_scsi_ext_disk_path(int id);
// P510: [P510-SCTL] — 300 フレームごとの無条件ダンプ。フレーム末尾から毎フレーム呼ぶ。
void p510_ext_scsi_dump(void);

// ---- State Save / Load (Phase 2 #4) ----
// Snapshot / restore the full emulated-machine state to a file.
// Thread-safe: the request is queued and executed at the next run_frame
// boundary on the emulation thread. Returns 0 when queued, negative on a
// bad argument.
int  mx68k_save_state(const char* path);
int  mx68k_load_state(const char* path);

// P481 (D-42): completion notification. Because the two calls above only report
// "queued", the actual result is published here once the emulation thread has
// run the operation. Snapshot mx68k_state_op_seq() before queueing, then poll
// until it changes; mx68k_last_state_rc() is that operation's result
// (0 = success) and mx68k_last_state_kind() is 0=none / 1=save / 2=load.
unsigned int mx68k_state_op_seq(void);
int          mx68k_last_state_rc(void);
int          mx68k_last_state_kind(void);

// ---- Sound ----
int  mx68k_audio_read(int16_t* buffer, int frames);
void mx68k_set_sound_enabled(bool en);
void mx68k_set_opm_volume(int vol);
void mx68k_set_adpcm_volume(int vol);
/* P624: turbo mode audio playback rate (replaces P555's mx68k_set_turbo_audio_mute).
 *
 * ratio_q16 is a Q16 fixed-point playback-speed multiplier: 65536 = 1.0x.
 * The consumer side (mx68k_audio_read, CoreAudio real-time thread) decimates the
 * ring buffer with a phase accumulator + linear interpolation, so turbo playback
 * is pitch-shifted (tape fast-forward) instead of silent — the same variable-rate
 * resampling approach WebX68k uses. Sound generation and chip state are untouched.
 *
 * Reserved values:
 *   MX68K_TURBO_AUDIO_RATE_UNITY (65536) — normal speed. mx68k_audio_read() takes
 *       a bypass path that is byte-for-byte identical to the pre-P624 code.
 *   MX68K_TURBO_AUDIO_RATE_MUTE  (0)     — silence (ring is drained, output ramps
 *       to zero via the existing P463/P465 decay). Kept as a general-purpose API
 *       primitive; as of P625 no UI path reaches it (No-Wait uses AUTO below).
 *   MX68K_TURBO_AUDIO_RATE_AUTO  (1)     — P625: for No-Wait turbo, whose effective
 *       speed is host-load dependent and therefore not expressible as a fixed
 *       ratio. mx68k_audio_read() derives the playback rate itself from the audio
 *       ring's fill level (proportional feedback + exponential smoothing) and feeds
 *       the result into the same decimation path used by fixed-multiplier turbo.
 *       1 is safe to reserve: it would otherwise mean 1/65536x playback, a ratio no
 *       caller can legitimately request (fixed turbo passes N<<16, N=2..5).
 * Negative values are invalid and are coerced to MX68K_TURBO_AUDIO_RATE_UNITY. */
#define MX68K_TURBO_AUDIO_RATE_UNITY 65536
#define MX68K_TURBO_AUDIO_RATE_MUTE  0
#define MX68K_TURBO_AUDIO_RATE_AUTO  1
void mx68k_set_turbo_audio_rate(int ratio_q16);

/* ---- P698: 動画録画への音声同梱(録画専用リングバッファ) ----
 *
 * 再生用の mx68k_audio_read() が使うリングとは完全に独立した第2のリング。
 * 再生用リングは SPSC(単一生成者/単一消費者)専用設計のため録画側が二重消費
 * できず、録画には専用リングを持たせる。実装/設計の詳細は EmulatorBridge.c の
 * 「P698: 録画専用オーディオリング」ブロックのコメントを参照。
 *
 * データの流れ:
 *   AudioEngine.audioCallback (CoreAudio 実時間スレッド、volume/mute 適用後)
 *     -> mx68k_rec_audio_write()
 *       -> [録画専用リング]
 *         -> mx68k_rec_audio_read()  (CVDisplayLink スレッドのドレイン経路)
 *           -> VideoRecordingService の音声トラック */

/* 録画用リングへ L/R インターリーブの Int16 PCM を frames フレーム書き込む。
 * ★CoreAudio 実時間スレッドから毎コールバック無条件に呼んでよい —— 録画中で
 *   なければ atomic load 1 回で即 return する。ロックも同期 I/O も行わない。
 * リングに frames 分の空きが無い場合、この回の書込みは**全体がドロップ**され
 * (部分書込みはしない)、内部のドロップ回数カウンタだけが加算される。 */
void mx68k_rec_audio_write(const int16_t* buffer, int frames);

/* 録画用リングから最大 max_frames フレーム読み出し、実際に読めたフレーム数を返す。
 * buffer は max_frames * 2 個の int16_t を格納できること。 */
int  mx68k_rec_audio_read(int16_t* buffer, int max_frames);

/* リング満杯で書込みを捨てた回数を読み出して同時にゼロクリアする
 * (get-and-reset 方式)。★非リアルタイムコンテキストから呼ぶこと —— 実時間
 *   スレッドではログ出力できないため、ドロップの記録はこの戻り値を見た
 *   呼出し側(録画の音声エンコードキュー)が行う。 */
uint32_t mx68k_rec_audio_get_and_reset_drop_count(void);

/* 録画セッション中の「1 回の CoreAudio コールバックで書き込まれたフレーム数」の
 * 最大値。リング容量 8192 フレームがコールバックの実サイズに対して十分かを
 * 実時間スレッドでログを出さずに検証するための観測値(P698 残留リスク表)。
 * mx68k_rec_audio_reset() でゼロへ戻る。 */
int  mx68k_rec_audio_get_max_write_frames(void);

/* リングとドロップカウンタを初期状態へ戻す。
 * ★mx68k_rec_audio_set_enabled(false) の状態でのみ呼ぶこと。 */
void mx68k_rec_audio_reset(void);

/* 録画用リングへの書込みを有効化/無効化する。false の間は
 * mx68k_rec_audio_write() が atomic load 1 回で即 return する。 */
void mx68k_rec_audio_set_enabled(bool enabled);

// ---- Keyboard Input ----
void mx68k_key_down(uint8_t x68k_keycode);
void mx68k_key_up(uint8_t x68k_keycode);

/* P228: software-keyboard LED indicator. Returns the raw keyLED byte captured
 * by mfp.c. Negative logic (0=lit, 1=off) for the 7 lock LEDs:
 * bit0=かな bit1=ローマ字 bit2=コード入力 bit3=CAPS bit4=INS bit5=ひらがな
 * bit6=全角 (D7 is the command-identify bit). Read-only. */
uint8_t mx68k_get_key_led(void);

// ---- Mouse Input ----
void mx68k_mouse_move(int dx, int dy);
void mx68k_mouse_button(int button, bool pressed);

// ---- Joystick Input ----
// bits: bit0=Up, bit1=Down, bit2=Left, bit3=Right, bit5=TRIG2, bit6=TRIG1
// bit4 / bit7 are unused and read as 1. 0 = pressed (active Low), idle = 0xFF.
void mx68k_joy_set(int port, uint8_t bits);
// P500: 多ボタンパッドの第2バンク(ストローブ線 High 側)。実機の多ボタン化は
// 8番ピン(PPI PortC bit4/5)の L/H による静的 2 バンク多重で実現されており、
// High 側のバイトをここへ書く。標準 2 ボタンパッドでは呼ぶ必要がない
// (未書込みなら 0xFF = idle のまま)。0 = 押下(負論理)。
// CPSF-MD(6ボタン): bit0=Z bit1=Y bit2=X bit3=MODE bit5=C bit6=START
// マジカルパッド(4ボタン): bit2=L bit3=R bit6=B、bit0/bit1 は常時 0(固定パターン)
void mx68k_joy_set1(int port, uint8_t bits);

// ---- SRAM ----
void mx68k_sram_save(void);
void mx68k_sram_load(void);
void mx68k_sram_clear(void);
// P454: schedule the SRAM zero-clear at the next mx68k_run_frame() boundary
// (avoids racing the emulation thread; mirrors mx68k_schedule_hard_reset).
void mx68k_schedule_sram_clear(void);
// P204: guest SRAM $ED0029 (XEiJ SRAM_EJECT) bit0 — eject FD at power-off.
bool mx68k_sram_eject_on_poweroff(void);

// ---- Status ----
typedef struct {
    uint32_t pc;
    uint32_t d[8];
    uint32_t a[8];
    uint16_t sr;
    uint32_t usp;
    uint32_t isp;
    int      clock_mhz;
    int      machine_type;
    int      memory_mb;
    bool     fpu_enabled;
    bool     fdd0_inserted;
    bool     fdd0_active;
    bool     fdd1_inserted;
    bool     fdd1_active;
    bool     paused;
    bool     hdd_busy;       // P201: SASI/HDD access indicator
    bool     hdd0_inserted;  /* P203 / P455: ランプ用の集約在席フラグ。
                              * ★P455 以降「unit 0 が装着」ではなく
                              *   「SASI unit 0..7 のいずれか OR 内蔵SCSI OR 外付SCSI」。
                              *   unit 別の在席は hdd_inserted_mask を見ること。 */
    bool     hdd1_inserted;  /* P203: SASI unit 1 の在席(意味は従来どおり不変)。
                              * P455 以降 hdd0_inserted が unit1 も OR 済みのため
                              * ランプ表示上は冗長。互換のため残置。 */
    /* P408: 実行粒度(CLOCK_SLICE)の可視化。既存 [P385-CHUNK] 計測値の読み取り専用
     * 横流し。末尾追加のため既存の初期化コードは無改修。 */
    int32_t  clock_slice;        // CLOCK_SLICE 現行値
    int32_t  clkdiv;             // クロック係数 生値
    int32_t  clk_total;          // 1フレームCPU予算 生値(line_budget の分子)
    int32_t  vline_total;        // 走査線数 生値(line_budget の分母)
    int32_t  chunks_last_frame;  // 直近フレームのチャンク数
    int32_t  cum_zero;           // 起動来の n==0 累積(0 が健全)
    /* P443 (D-7): ゲスト側イジェクトにも追従するメディア有無(s_fdd_present[] 由来)。
     * Swift 側 EmulatorViewModel が自身の fdd0Path/fdd1Path をこれと突き合わせる。
     * 末尾追加のため既存フィールドの並び順は不変。 */
    bool     fdd0_media_present;
    bool     fdd1_media_present;
    /* P684: FDD 2 台 → 4 台。
     * ★挿入位置について: この構造体は Swift/C が同一ビルドで再コンパイルされる
     * 実行時ブリッジ用であり、ディスクへシリアライズされるバイナリフォーマット
     * ではない。よって後続フィールド(hdd_inserted_mask / rtc_clkout_select)の
     * オフセットがずれても実害は無く、意味的に関連する fdd1_media_present の
     * 直後へ置く。 */
    bool     fdd2_media_present;
    bool     fdd3_media_present;
    /* P689: CPU モニタ表示用(赤 = アクセス中)。P684 時点ではドライブ 2/3 に
     * 表示先が無かったため追加を見送っていたが、P686 (D-70) でドライブ 2/3 が
     * 実際に機能するようになり、CPUMonitorView が表示先になった。 */
    bool     fdd2_active;
    bool     fdd3_active;
    /* P455: SASI 論理 unit 0..7 の装着マスク(bit n = unit n が装着)。
     * ステータスバーの HDD ランプは実機/XM6 同様 1 個のままで、この生マスクは
     * 表示に使わない — 将来の per-drive UI・モニタパネル用の生値。
     * ★このマスクに SCSI(内蔵/外付)の在席は含めない。「SASI が何台か」を
     *   曖昧さなく読めることがこのフィールドの存在理由であり、SCSI を OR すると
     *   その意味が壊れる。SCSI 在席は hdd0_inserted 側に含まれる。
     * 末尾追加のため既存フィールドの並び順は不変。 */
    uint8_t  hdd_inserted_mask;
    /* P653: TIMER-LED(RTC CLKOUTセレクトレジスタ、BANK1 $E8A001下位3bit)の現在値。
     * Inside X68000 p147-150 / テクニカルデータブック p191 §5-3 で確認済みの
     * 実機仕様(TIMER-LEDは物理LED、CLKOUT端子で直接駆動)。
     * 末尾追加のため既存フィールドの並び順は不変。 */
    uint8_t  rtc_clkout_select;   // 0-7、値の意味は P653 Fix Plan 記号表を参照
} MX68KStatus;

void mx68k_get_status(MX68KStatus* status);

/* P286: developer monitor panels — CRTC / Video Controller / BG・Sprite.
 * All three read existing extern globals only (no Core changes), snapshot-copied
 * into the out-param under the same 1Hz gate as mx68k_get_status. */
typedef struct {
    double   hsync_khz;
    double   vsync_hz;
    uint32_t text_dot_x;
    uint32_t text_dot_y;
    uint32_t text_scroll_x;
    uint32_t text_scroll_y;
    uint32_t vline_total;
    uint16_t crtc_hstart, crtc_hend, crtc_vstart, crtc_vend;
} MX68KCRTCStatus;
void mx68k_get_crtc_status(MX68KCRTCStatus* status);

typedef struct {
    uint8_t vc_reg0_0, vc_reg0_1;
    uint8_t vc_reg1_0, vc_reg1_1;
    uint8_t vc_reg2_0, vc_reg2_1;
    int     mode;          // VCReg0[1] & 3  (0=16色 1/2=256色 3=65536色)
    int     pri_gr, pri_tx, pri_sp;   // 優先度 (小さいほど手前)
    bool    text_on;
    bool    sp_on;          // VCReg2[1] & 0x40 (BG/Sprite面 全体の有効)
    bool    sp_cond_256;    // 256色page0のグラフィック特殊優先(SP)発動条件(P283/P284)
} MX68KVCStatus;
void mx68k_get_vc_status(MX68KVCStatus* status);

typedef struct {
    bool     bg0_on, bg1_on;
    int      bg_chrsize;    // 8 or 16
    uint32_t bg0_scroll_x, bg0_scroll_y;
    uint32_t bg1_scroll_x, bg1_scroll_y;
    uint16_t bg0_top, bg1_top;
    int      sprite_active_count;   // posx/posy が (0,0) でないエントリ数 (0-128, P278と同じ判定)
} MX68KBGStatus;
void mx68k_get_bg_status(MX68KBGStatus* status);

/* P742: DMAC(HD63450 / MC68450 相当)レジスタモニタ。
 * 既存モニタ API(mx68k_get_crtc_status 等)と同じ「Core の extern 実体を
 * ロックなしでスナップショットコピーする」契約。read-only —— Core 状態は
 * 一切変更しない(dmac_ch DMA[4] の単純なメンバ読み出しのみで、
 * Memory_ReadB / BusErrFlag のいずれも経由しない)。
 * 値は生のレジスタ値をそのまま渡す(ビット単位の意味的デコードは
 * 本サイクルのスコープ外 —— Swift 側で 16 進表示する)。 */
typedef struct {
    uint8_t  csr, cer, dcr, ocr, scr, ccr;
    uint16_t mtc;
    uint32_t mar;
    uint32_t dar;
    uint16_t btc;
    uint32_t bar;
    uint8_t  niv, eiv, mfc, cpr, dfc, bfc, gcr;
} MX68KDMACChannelStatus;

typedef struct {
    MX68KDMACChannelStatus ch[4];
} MX68KDMACStatus;

void mx68k_get_dmac_status(MX68KDMACStatus* status);

/* P550: パレットモニタ。既存モニタAPI(mx68k_get_crtc_status 等)と同じ
 * 「ロックなしスナップショットコピー」契約。read-only(Core状態は一切変更しない)。
 * text_pal/grph_pal の各要素は px68k ネイティブの 32bit パレット語 0xRRGGBB0A —
 *   R = bits 31-24 / G = bits 23-16 / B = bits 15-8 / bit0 = 半透明用 Abit。
 * (この配置は Bridge 側 mx68k_init() の WinDraw_Pal32R/G/B = 0xFF000000 /
 *  0x00FF0000 / 0x0000FF00 で決まる。表示用フレームバッファの BGRA へは
 *  px68k_color_to_rgba() が (c>>8)|0xFF000000 で変換している。)
 * 最下位バイトは表示用アルファではないので、スウォッチ描画には使わないこと。 */
typedef struct {
    uint32_t text_pal[256];   /* TextPal32 — テキスト/BG/スプライト面パレット */
    uint32_t grph_pal[256];   /* GrphPal32 — グラフィック面パレット */
    uint8_t  contrast;        /* Contrast_Value (0-15) */
} MX68KPaletteStatus;
void mx68k_get_palette_status(MX68KPaletteStatus* status);

/* P694: RTC モニタ(RICOH RP5C15 = Core/px68k/x68k/rtc.c)。既存モニタ API
 * (mx68k_get_crtc_status 等)と同じ「ロックなしスナップショットコピー」契約で、
 * read-only(RTC_Regs[][] を一切変更しない。RTC_Read() とは別の非破壊経路)。
 *
 * ★このチップの実装には 2 つの「書いた値が読み戻されない」罠がある。パネル側で
 *   誤読させないよう、構造体の時点で「ゲストが書いた値」と「RTC_Read() が実際に
 *   返す値」を別フィールドに分けてある:
 *
 *   (1) 日時本体(sec/minute/hour/wday/mday/mon/year)—— px68k の RTC_Read() は
 *       ゲストが日時レジスタを読む度に time(NULL)/localtime() でホストの現在時刻を
 *       都度算出して返す。チップ内部にエミュレータ独自の日時カウンタは存在せず、
 *       ゲストが日時レジスタへ書いた値(RTC_Regs[0][0..12])は読み出しに一切
 *       反映されない(rtc.c:108 のコメント「日付や時間のSetは行わない。保存する
 *       だけで使用しない」のとおり)。よってこの 7 フィールドは
 *       **ホスト時計のスナップショットであってゲスト内部状態ではない**。
 *   (2) leap_year_ctr / leap_year_effective —— ゲストの書込み先
 *       (RTC_Regs[1][11]、rtc.c:153)と RTC_Read() の BANK1 case 0x17(rtc.c:95)が
 *       食い違う。読み側は RTC_Regs[1][11] を一切参照せず (tm_year-80)%4 を
 *       ホスト年から都度計算して返すため、実効的に書込み専用レジスタになっている。
 *       両方を公開して並べて見せる(P694 Code Review 指摘)。
 *
 * 上記以外(clkout_select / adj / alarm_* / hour_mode_24 / reg_*)は RTC_Write() が
 * 実際に RTC_Regs[][] へ格納し RTC_Read() が読み戻す真の制御状態。 */
typedef struct {
    uint8_t bank;            /* RTC_Bank(現在選択中のレジスタバンク 0/1) */
    uint8_t clkout_select;   /* RTC_Regs[1][0] & 0x07(MX68KStatus.rtc_clkout_select と同値。
                              * RTC パネルを自己完結させるための意図的な重複配線) */
    uint8_t adj;             /* RTC_Regs[1][1] & 0x01 */
    uint8_t alarm_min;       /* RTC_Regs[1][3](10の位)*10 + RTC_Regs[1][2](1の位)= 10進値 */
    uint8_t alarm_hour;      /* RTC_Regs[1][5]/[1][4] を同様に 10進合成 */
    uint8_t alarm_wday;      /* RTC_Regs[1][6] & 0x07 */
    uint8_t alarm_day;       /* RTC_Regs[1][8]/[1][7] を同様に 10進合成 */
    uint8_t hour_mode_24;    /* RTC_Regs[1][10] & 0x01 (0=12時間制 / 1=24時間制) */
    uint8_t leap_year_ctr;      /* RTC_Regs[1][11] & 0x03 —— ゲストが書いた保存値。上記 (2) */
    uint8_t leap_year_effective;/* ((tm_year-80)%4)&0x03 —— RTC_Read() が実際に返す値。上記 (2) */
    uint8_t reg_bank_ctrl;   /* RTC_Regs[0][13] 生値(Alarm/Timer Enable 制御 + BANK 選択 bit0) */
    uint8_t reg_test;        /* RTC_Regs[0][14] 生値(TEST モード) */
    uint8_t reg_alarm_out;   /* RTC_Regs[0][15] 生値(ALARM 端子出力制御) */
    /* ---- 日時本体 = ホスト時計のスナップショット(上記 (1))。BCD ニブルではなく
     *      10進値で入れる(Swift 側で BCD 復元をやり直さなくて済むように)。 ---- */
    uint8_t sec;             /* 0-59 */
    uint8_t minute;          /* 0-59 */
    uint8_t hour;            /* RTC_Read() の tm24 と同一の値。24時間制なら 0-23、
                              * 12時間制なら 0-11(午後は +20 された 20-31)—— これは
                              * rtc.c:50-51 のチップ表現をそのまま保っている */
    uint8_t wday;            /* 0=日曜 … 6=土曜(tm_wday) */
    uint8_t mday;            /* 1-31 */
    uint8_t mon;             /* 1-12(tm_mon+1) */
    uint8_t year;            /* 1980 からの経過年数(tm_year-80)。RTC_Read() の
                              * BANK0 case 0x17/0x19 と同じ基準 —— 例: 2026年 → 46 */
} MX68K_RTCStatus;
void mx68k_get_rtc_status(MX68K_RTCStatus* out);

/* P484: サウンドモニタ(ADPCM section)。既存モニタAPIと同じ
 * 「ロックなしスナップショットコピー」契約。read-only(Core状態は一切変更しない)。
 * 既存9モニタの1秒ゲートとは別に、パネル表示中のみ 0.1秒ゲートで呼ばれる
 * (呼出しスレッドは既存モニタAPIと同一 = CVDisplayLink 駆動)。 */
#define MX68K_ADPCM_WAVEFORM_SAMPLES 256

typedef struct {
    int16_t waveform[MX68K_ADPCM_WAVEFORM_SAMPLES];  /* 時系列順(先頭=古い、末尾=新しい) */
    int32_t peak_level;        /* 上記スナップショット内の絶対値ピーク(0-32768) */
    bool    playing;           /* ADPCM_Read(0xE92001) の 0xC0 判定 */
    int32_t sample_rate_hz;    /* ADPCM_ClockRate から算出 */
    int32_t clock_divider;     /* ADPCM_Clock の分周値(0-7) */
    bool    dma_active;        /* DMA[3].CCR の STR ビットから判定 */
    int32_t pan;               /* g_ppi_portc_shadow の bit0-3 */
} MX68K_ADPCMStatus;

void mx68k_get_adpcm_status(MX68K_ADPCMStatus* out);

/* P485: サウンドモニタ(OPM = YM2151 8ch のコンパクト表示)。
 * mx68k_get_adpcm_status() と同じ「ロックなしスナップショットコピー」契約、
 * read-only(Core状態は一切変更しない)。値は全て Bridge 側 OPM シャドウ
 * (P479 由来 g_opm_shadow[] / g_opm_written[] と P485 の g_opm_keyon[])から
 * 導出する — OPM::GetReg は宣言のみで未定義のため Core からは読めない。 */
typedef struct {
    uint8_t keyon;   /* bit0-3 = op0..op3 (1=キーオン中)、g_opm_keyon[ch] そのまま */
    uint8_t alg;     /* 0-7 */
    uint8_t fb;      /* 0-7 */
    uint8_t pan;     /* 0=mute, 1=L, 2=R, 3=LR */
    int8_t  note;    /* 0-11 (C=0..B=11)、-1 = 無効nibbleまたは未書込み */
    int8_t  octave;  /* 0-7 */
} MX68K_OPMChannel;

typedef struct {
    MX68K_OPMChannel ch[8];
} MX68K_OPMStatus;

void mx68k_get_opm_status(MX68K_OPMStatus* out);

/* P631: OPM シンセサイザーパネル(独立ウィンドウ、XM6 風の 8ch 縦並び表示)用の
 * 詳細ステータス。既存の mx68k_get_opm_status() は P485 のサウンドモニタが
 * 引き続き使うため無改造で残し、こちらを別APIとして追加する。
 * mx68k_get_opm_status() と同じ「ロックなしスナップショットコピー」契約、
 * read-only(Core状態は一切変更しない)。値は全て Bridge 側 OPM シャドウ
 * (P479 由来 g_opm_shadow[] / g_opm_written[] と P485 の g_opm_keyon[])から導出する。
 *
 * ★`written` の必要性: OPM は全レジスタ 0 でリセットされる(opm.cpp:78)ため、
 * 一度も書込みが無いチャンネルでも kc_raw/kf_raw/tl[] は 0 に見える。表示側は
 * written==false のとき KCF/V/PAN を「--」等のプレースホルダにし、生の 0 埋め値を
 * 意味のあるレジスタ値であるかのように見せないこと。 */
typedef struct {
    bool     written;     /* g_opm_written[0x28+ch] — false ならこのチャンネルは
                             一度も KC 書込みが無い(表示側は「--」等で区別すること) */
    uint8_t  keyon;       /* bit0-3 = op0..op3、g_opm_keyon[ch] そのまま */
    uint8_t  kc_raw;      /* reg $28+ch の生バイト */
    uint8_t  kf_raw;      /* reg $30+ch の生バイト */
    int8_t   note;        /* 0-11 (C=0..B=11)、-1 = 無効nibbleまたは未書込み。
                             既存 mx68k_get_opm_status() と同じ算出式 */
    int8_t   octave;      /* 0-7、同上 */
    uint8_t  alg;         /* 0-7: reg $20+ch の bit0-2 */
    uint8_t  fb;          /* 0-7: reg $20+ch の bit3-5 */
    uint8_t  pan;         /* 0=mute, 1=L, 2=R, 3=LR: reg $20+ch の bit6-7 */
    uint8_t  tl[4];       /* reg $60+(slot<<3)+ch の bit0-6、slot=0..3(レジスタ上の
                             スロット順。fmgen 内部の op[] 順とは slottable[4]=
                             {0,2,1,3} でずれる — opm.cpp:278-281) */
    int32_t  volume_est;  /* ★推定値(確定仕様ではない): 127 - min(キャリアのTL)。
                             0-127 にクランプ。詳細は EmulatorBridge.c 側の実装コメント */
} MX68K_OPMDetailChannel;

typedef struct {
    MX68K_OPMDetailChannel ch[8];
} MX68K_OPMDetailStatus;

void mx68k_get_opm_detail_status(MX68K_OPMDetailStatus* out);

/* P491: サウンドモニタ(Mercury Unit の FM 部 = YMF288 / OPN3-L)。
 * mx68k_get_opm_status() と同じ「ロックなしスナップショットコピー」契約、
 * read-only(Core状態は一切変更しない)。値は全て Bridge 側 OPN シャドウ
 * (Bridge/mercury_opn_shadow.h)から導出する — Y288::GetReg は FM/リズムに対して
 * 常に 0 を返し、SSG を読める唯一の経路はゲストのアドレスラッチを壊すため。
 * Mercury 未装着(g_mercury_installed==0)のときは installed=false + 全ゼロ。
 * ★表示は 1 チップ分(FM 6ch + SSG 3ch)。2 個目の YMF288 へはゲストから
 * 到達できない(上流 px68k 由来の既知の制約、mercury_opn_shadow.h 参照)。 */
typedef struct {
    uint8_t  keyon;   /* reg $28 のチャンネル別スロットゲート(0 = 全スロットキーオフ) */
    uint16_t fnum;    /* F-Number 11bit: reg $A0+c + (reg $A4+c & 7) * 0x100 */
    uint8_t  block;   /* Block 0-7: (reg $A4+c >> 3) & 7 */
    uint8_t  alg;     /* 0-7: reg $B0+c & 7 */
    uint8_t  fb;      /* 0-7: (reg $B0+c >> 3) & 7 */
    uint8_t  pan;     /* 0=mute, 1=R, 2=L, 3=LR: (reg $B4+c >> 6) & 3 */
} MX68K_MercuryOPNChannel;

typedef struct {
    bool     tone_on;   /* reg $07 は負論理(psg.cpp:222-224) */
    bool     noise_on;
    uint16_t period;    /* トーン周期 12bit: reg[2c] + reg[2c+1] * 256 */
    uint8_t  volume;    /* 0-15: reg $08+c の bit0-3 */
    bool     env_on;    /* reg $08+c の bit4 */
} MX68K_MercurySSGChannel;

typedef struct {
    bool     installed;        /* g_mercury_installed(配線確定値) */
    MX68K_MercuryOPNChannel fm[6];
    MX68K_MercurySSGChannel ssg[3];
    uint8_t  noise_period;     /* reg $06 の bit0-4 */
    uint8_t  rhythmkey;        /* 累積 bit0-5 = BD,SD,TOP,HH,TOM,RIM */
    uint8_t  rhythm_total_level; /* reg $11: ~data & 63 */
    uint32_t write_count;      /* シャドウが受理した累積書込回数(分母) */
} MX68K_MercuryOPNStatus;

void mx68k_get_mercury_opn_status(MX68K_MercuryOPNStatus* out);

/* P635 / P636: サウンドモニタ(Mercury Unit の PCM 部)。上の
 * mx68k_get_mercury_opn_status() と同じ「ロックなしスナップショットコピー」契約、
 * read-only(Core 状態は一切変更しない)。Mercury 未装着
 * (g_mercury_installed==0)のときは installed=false + 全ゼロ。
 *
 * stereo / l_enabled / r_enabled は Mcry_Status のビット由来
 * (Core/px68k/x68k/mercury.c:160 `Mcry_Status&2` = Stereo、:177 `Mcry_Status&4` =
 * L 有効、:163 `Mcry_Status&8` = R 有効 — いずれも 0 のとき該当チャンネルは
 * data=0 へ差し替えられる = ミュート)。
 * last_out_l / last_out_r は Mcry_OutDataL / Mcry_OutDataR(同 :29-30、int16_t)、
 * すなわちゲストが最後に書いた PCM サンプル値そのもの。
 * write_count は「PCM は動いているが無音」と「そもそも PCM 活動が無い」を
 * 区別するための分母。P636 以降その実現手段は、毎スキャンラインの
 * サンプル値ポーリングで前回値からの変化を検出した累積回数
 * (Bridge 側カウンタ、Bridge/mercury_opn_shadow.h の
 * g_mcry_pcm_sample_change_count)であり、CPU 書込み・DMAC 書込みの
 * いずれの経路でも検出できる(DMAC 駆動の書込みも捕捉できる)。
 * ★フィールド名 write_count は構造体の後方互換のため維持しているが、
 *   意味は「書込み回数」ではなく「活動(サンプル値変化)回数」。 */
typedef struct {
    bool     installed;       /* g_mercury_installed(配線確定値) */
    bool     stereo;          /* Mcry_Status bit1 */
    bool     l_enabled;       /* Mcry_Status bit2 */
    bool     r_enabled;       /* Mcry_Status bit3 */
    int32_t  clock_rate;      /* Mcry_ClockRate (Hz) */
    int16_t  last_out_l;      /* Mcry_OutDataL(直近の PCM サンプル値、符号付き) */
    int16_t  last_out_r;      /* Mcry_OutDataR */
    uint32_t write_count;     /* P636: サンプル値変化を検出した累積回数(活動の分母)。
                               * 名前は後方互換で write_count のまま。 */
} MX68K_MercuryPCMStatus;

void mx68k_get_mercury_pcm_status(MX68K_MercuryPCMStatus* out);

/* P549: サウンドモニタ — CoreAudio バッファのアンダーラン統計(セッション累積、
 * リセットされない)。mx68k_get_mercury_opn_status() と同じ
 * 「ロックなしスナップショットコピー」契約、read-only(Core状態は一切変更しない)。
 * 値の更新元は CoreAudio 実時間スレッド、読み出しは既存モニタAPIと同じ
 * CVDisplayLink スレッド。3フィールドは個別のアトミックから読むため厳密には
 * 同一瞬間のスナップショットではない — 表示側で underrun/total の割合を
 * クランプすること。 */
typedef struct {
    uint64_t callbacks_total;       /* オーディオコールバック総数(割合の分母) */
    uint64_t underrun_callbacks;    /* うちアンダーランが発生した回数 */
    uint64_t samples_zero_filled;   /* 無音ランプで埋めたサンプル総数 */
} MX68K_AudioBufferStatus;

void mx68k_get_audio_buffer_status(MX68K_AudioBufferStatus* out);

typedef struct {
    int     slot;        // 0-127 (Sprite_Regs内のスロット番号)
    int     x, y;         // posx & 0x3ff, posy & 0x3ff
    int     pattern;      // ctrl & 0xff (BGCHR16内のパターン番号、bg.c:334-345参照)
    int     palette_hi;   // (ctrl >> 4) & 0xf0 (パレット上位4bit、bg.c:349参照)
    bool    hflip, vflip; // ctrl & 0x4000 / ctrl & 0x8000 (bg.c:326-333参照)
    int     priority;     // ply & 3 (bg.c:308参照、小さいほど手前)
} MX68KSpriteEntry;

/* out_entries には呼出し側が確保した配列(最低128要素)を渡す。
 * 稼働中(posx/posy が(0,0)でない)のスプライトのみを詰め、実際に
 * 書き込んだ件数を返す(0-128)。read-only(Sprite_Regsは一切書き込まない)。 */
int mx68k_get_sprite_list(MX68KSpriteEntry* out_entries, int max_entries);

/* mx68k_get_sprite_list と同じだが、フィルタせず常に全128スロット
 * (スロット0-127を順に、未使用スロットも含む)を返す。書き込んだ
 * 件数(通常128)を返す。read-only。 */
int mx68k_get_sprite_table_full(MX68KSpriteEntry* out_entries, int max_entries);

/* out_rgba には呼出し側が確保した16*16*4=1024バイトのバッファを渡す。
 * 指定スロットのスプライトパターン(Core BGCHR16参照、H/V反転・パレット
 * 適用済み)をBGRA8888(既存mx68k_get_framebufferと同じバイト順)で
 * 書き込む。ドット値0(透過)は alpha=0 とする。read-only。 */
void mx68k_get_sprite_pattern_rgba(int slot, uint8_t* out_rgba);

/* P343: out_rgba には呼出し側が確保した1024*1024*4バイトのバッファを渡す。
 * 派生テキストバッファ TextDrawWork(1024x1024)の内容を BGRA8888 で書き込む。
 * パレット索引0は、P356トグル(既定ON)でTextPal32[0](背景色)、
 * OFFなら透過(alpha=0)。read-only。 */
void mx68k_get_text_plane_rgba(uint8_t* out_rgba);

/* P356: Text Plane Viewerのインデックス0描画方式を切り替える。
 * enabled=1(既定)ならTextPal32[0]で塗る(XM6/px68k本家のopaqueな
 * 単独面バッファ表示と一致)。enabled=0なら旧来の透過(黒背景)表示。 */
void mx68k_set_text_plane_backdrop(int enabled);

/* P348: out_rgba には呼出し側が確保した1024*1024*4バイトのバッファを渡す。
 * 指定BGページ(0=BG0/1=BG1)を、既存のCoreデコード関数
 * (bg_drawline_loopx16/x8)を1行ずつ呼び出す形で画像化しBGRA8888で書き込む。
 * out_sizeには実際の有効サイズ(CHRSIZE=16なら1024、8なら512)を返す
 * ——out_rgbaの左上out_size x out_size領域のみ内容があり、残りは透過(0)。
 * パレットバンク由来の非表示ピクセルも含め、実際の合成ループと同じ二重条件
 * (Text_TrFlag bit1 かつ 色が非ゼロ)で透過判定する。read-only、呼出し前後
 * でVLINEBG/BG_VLINE/TextDotXを退避・復元するためエミュレーション本体には
 * 影響しない。 */
void mx68k_get_bg_page_rgba(int page, uint8_t* out_rgba, int* out_size);

/* P353: BG0/BG1の表示をモニタUIから手動でON/OFFする(デバッグ専用)。
 * ゲーム自身が読むBG_Regs[9]の実体は変更しない(描画直前に一時的に
 * ビットクリアし、描画直後に復元する既存P281と同じ安全パターン)。
 * デフォルトは両方表示(visible=1)、実機と同じ挙動。 */
void mx68k_set_bg_layer_visible(int layer, int visible);   /* layer: 0=BG0, 1=BG1 */
int mx68k_get_bg_layer_visible(int layer);   /* layer: 0=BG0, 1=BG1。現在の実際の値を返す */

void mx68k_diag_set_last_frame_ms(double ms);   /* P526: 直前フレームの実経過ms(Swift計測値) */

/* P348/P690: out_rgba には呼出し側が確保した512*512*4バイトのバッファを渡す。
 * 16色(4面)/256色(2面)/65536色(1面)の各モードで、指定グラフィック面ページを
 * GVRAMから直接読み出しBGRA8888で書き込む(透過ドットは alpha=0)。
 * 16色1024dotモードは非対応で、out_rgbaをゼロ埋めしたまま0を返す
 * (呼出し側は「非対応」として扱うこと)。成功時は1を返す。read-only。 */
int mx68k_get_grp_page_rgba(int page, uint8_t* out_rgba);

/* P690: 現在の色モードで有効なグラフィック面ページ数。
 * 0=非対応(16色1024dotモード) / 1=65536色 / 2=256色 / 4=16色。read-only。 */
int  mx68k_get_grp_page_count(void);

/* P690: CRTC R20 の D11(拡張VRAM配置、px68k実装が「Nemesis技法」と呼ぶ)経路で
 * 復号しているか。true の間はモニタの色解釈が参考表示である旨をUIへ出す。read-only。 */
bool mx68k_get_grp_page_uses_nemesis(void);

/* P365: BG+スプライト合成バッファ(最終GRP/テキスト優先度合成"前"の中間状態)。
 * out_rgba には呼出し側が確保した 1024*1024*4 バイトのバッファを渡す。実データは
 * 先頭から out_w x out_h ぶんが詰めて書き込まれる(ストライドは out_w*4。BG/GRP
 * Page の固定サイズとは違い表示モードで変化する)。read-only。 */
void mx68k_get_bgsp_composite_rgba(uint8_t* out_rgba, int* out_w, int* out_h);

void mx68k_set_bgsp_composite_visible(int visible);   /* P521: ⌘⌥C可視性ゲート(s_bgsp_bufferの書込み側) */

/* P373: 現在のフレーム番号(g_mx68k_frame_num)。read-only。 */
int mx68k_get_frame_num(void);

/* P383: 状態スタンプ用のレジスタ生値。~1Hzゲート越しの mx68k_get_vc_status /
 * mx68k_get_bg_status と違い、呼出し時点の値をそのまま返す。read-only。 */
uint8_t mx68k_get_vc_reg1_0_live(void);
uint8_t mx68k_get_vc_reg2_1_live(void);
uint8_t mx68k_get_bg_regs9_live(void);

/* ---- P600: メモリダンプビューア(⌘⌥2)/ メモリマップビューア(⌘⌥R) -------------
 *
 * 既存モニタ API(mx68k_get_crtc_status 等)と同じ「Core の extern 実体を
 * スナップショットコピーするだけ」の read-only 契約。
 *
 * ★この 2 関数は MemReadTable / cpu_readmem24 を一切呼ばない。あの経路は実機同等の
 *   副作用を伴い(FDC のデータ FIFO 前進 Core:x68k/fdc.c:449-458、SASI のホスト
 *   ディスク I/O 起動 sasi.c:157-200、SCC のレジスタ選択ラッチ消去 scc.c:171-190、
 *   MFP のキー割込フラグクリア mfp.c:215-218、未マップ窓での BusErrFlag セット
 *   mem_wrap.c:604-613)、モニタを開くだけでゲスト状態が進んでしまう。
 *   したがって読めるのは Bridge がフラットな実体を把握している領域だけで、
 *   デバイス I/O 窓・未装着ボード窓・未マップ窓は「読まずに理由を返す」。 */
typedef enum {
    MX68K_MEMKIND_FLAT_SWAP     = 0,  /* MEM / TVRAM / SRAM / SRAM拡張 / FONT — ^1 スワップ格納 */
    MX68K_MEMKIND_FLAT_RAW      = 1,  /* IPL — スワップ無し(rm16_main 規約) */
    MX68K_MEMKIND_DECODED       = 2,  /* GVRAM — CPU窓ではなく物理512KBプレーンを見せる */
    MX68K_MEMKIND_IO_UNSAFE     = 3,  /* デバイス I/O 窓 — 読み出し不可(専用モニタへ誘導) */
    MX68K_MEMKIND_NOT_INSTALLED = 4,  /* 未装着ボード窓 */
    MX68K_MEMKIND_BUSERR        = 5   /* 未マップ窓 */
} MX68KMemKind;

/* addr から len バイトを out[] へ、各バイトの種別(MX68KMemKind)を out_kind[] へ書く。
 * out / out_kind は呼出し側が len バイト確保する(out_kind は NULL 可)。
 * 読み出し不可の種別(IO_UNSAFE / NOT_INSTALLED / BUSERR)のバイトは out=0xFF 固定。
 * リージョン表に明示列挙されていないアドレスは全て BUSERR へ倒れる(安全側 catch-all)。 */
void mx68k_read_memory_bytes(uint32_t addr, uint8_t* out, uint8_t* out_kind, uint32_t len);

typedef struct {
    uint32_t base;
    uint32_t size;
    char     name[32];   /* UTF-8。ロケール非依存の短い識別名 */
    int      status;     /* 0=readable / 1=io_unsafe / 2=not_installed / 3=buserr */
} MX68KRegionInfo;

/* リージョン表を out[] へ最大 max 件書き、書いた件数を返す。装着状態は設定値ではなく
 * 配線確定値(g_wired_machine_type / g_scsi_ext_board_wired / g_mercury_installed /
 * g_midi_installed / sram_ext_is_enabled)から構成する — P457 の wired 値基準 UI ゲートと
 * 同じ基準にするため。 */
int mx68k_get_region_map(MX68KRegionInfo* out, int max);

/* P740: addr の 1 命令を逆アセンブルして out_text へ書き、その命令長(バイト数)を返す。
 * 中身は Core 同梱の Debabelizer(d68k.c、m68k_disassemble())で、読み取りは
 * cpu_readmem24 を経由する。そのため上の mx68k_read_memory_bytes() とは異なり
 *   (a) P600 のリージョン分類で「読める」種別(FLAT_SWAP / FLAT_RAW / DECODED)に
 *       限定し、I/O 窓・未装着・未マップ窓では逆アセンブラを一切呼ばない
 *   (b) 命令が領域境界を越えて隣の未安全領域まで読み進むことが無いよう、
 *       領域末尾から最悪ケース命令長ぶんの余裕が無い開始アドレスも拒否する
 * という 2 段の安全ゲートを内側に持つ。拒否時は out_text に "(unreadable)" を書き 2 を返す。
 * ★呼出し側は必ず EmulatorEngine.withEmulationLock で囲むこと — cpu_readmem24 は
 *   グローバル可変状態(BusErrFlag)を無条件にリセットするため、mx68k_run_frame() 実行中の
 *   DMAC/MIDI/SCSI がそのフラグを読む処理と競合しうる。 */
uint32_t mx68k_disassemble_line(uint32_t addr, char* out_text, uint32_t out_text_len);

/* P211: current guest VSYNC rate in Hz (CRTC R20 bit4: hi-res 55.46 / lo-res 61.46).
 * Used by the Swift frame driver to pace mx68k_run_frame() to wall-clock. */
double mx68k_get_vsync_hz(void);

/* ----  P51-B ablation switches (L3 chunk-internal SSP exception-push scrub) ----
 * Consumed in both EmulatorBridge.c and m68000_bridge.c. Hoisted to header so a
 * single source of truth governs both translation units. C99 §6.10.1 evaluates
 * undefined identifiers in #if as 0; defining these only in one .c would
 * silently disable the consumer in the other → "fail-quiet dead code".
 * See /tmp/mx68k_P51B_plan.md §0 / §2.1. */
#define P51B_ENABLE              0
#define P51B_SIGC_ENABLE         1   /* BasePC-dirty gate (primary) */
#define P51B_SIGA_ENABLE         1   /* SSP-relative + supervisor gate (action) */
#define P51B_SIGB_ENABLE         1   /* vec-area fallback (no BasePC dirty req) */
#define P51B_SR_GATE_ENABLE      1   /* Spec Proposal A diagnostic only — see plan §5.4 */
#define P51B_LOG_CAP             64

/* Optional summary dumper — call once at session end. */
void m68000_p51b_dump_summary(void);

/* ---- P52 ablation switches (chunk-entry SSP guard, diagnostic-first) ----
 * Single source of truth for both translation units. C99 §6.10.1 evaluates
 * undefined identifiers in #if as 0. See /tmp/mx68k_P52_plan.md §2. */
#define P52_ENABLE                       0
#define P52_SNAPSHOT_ENABLE              0
#define P52_DETECT_ENABLE                0
#define P52_DETECT_SR_BYTE_ENABLE        0   /* sigA: (stk_sr & 0x58E0) != 0 */
#define P52_DETECT_PC_RANGE_ENABLE       0   /* sigB: stk_PC odd or hi ∉ {0x00,0xFF} */
#define P52_DETECT_SINGLE_PUSH_ENABLE    0   /* 1=strict delta==6; 0=relaxed multiple-of-6 */
#define P52_RESTORE_ENABLE               0   /* Phase 1: OFF (diagnostic-only) */
#define P52_LOG_CAP                      64
#define P52_RAM_LO                       0x00000000u
#define P52_RAM_HI                       0x00C00000u

void m68000_p52_dump_summary(void);

/* ---- P53 ablation switches (graceful-shutdown wiring) ----
 * Single source of truth for both C and Swift translation units.
 * - P53_ENABLE: master gate for idempotency + atexit body.
 * - P53_ATEXIT_ENABLE: gate for atexit registration + summary body.
 * - P53_APPDELEGATE_ENABLE: gate for AppDelegate.applicationWillTerminate
 *   body. NOTE: the @NSApplicationDelegateAdaptor field is a compile-time
 *   decoration in MX68KApp.swift and is not runtime-removable; this switch
 *   only short-circuits the willTerminate body. See /tmp/mx68k_P53_plan.md §4.
 * - P53_SIGSRC_ENABLE: gate for DispatchSourceSignal (SIGTERM/SIGINT) install
 *   in applicationDidFinishLaunching. Default disposition restored when 0.
 */
#define P53_ENABLE              1
#define P53_ATEXIT_ENABLE       1
#define P53_APPDELEGATE_ENABLE  1
#define P53_SIGSRC_ENABLE       1

/* P53 atexit-safe summary helper (NOT mx68k_shutdown — avoids double-free).
 * See /tmp/mx68k_P53_plan.md §1.3 / §3.2. */
void mx68k_atexit_summary(void);

/* P53 ablation queries (exposed to Swift). */
int  mx68k_p53_appdelegate_enabled(void);
int  mx68k_p53_sigsrc_enabled(void);

/* P53 C bridge logging helpers — route Swift markers through debug_log
 * (no NSLog: no stderr→debug.log redirect exists in this codebase).
 * See /tmp/mx68k_P53_plan.md §1.4 (Code Review C-2). */
void mx68k_log_delegate_fire(void);
void mx68k_log_sig_catch(const char* signame);
void mx68k_log_marker(const char* msg);

/* ---- P54 ablation switches (gate-3 relaxation + reject diagnostics) ----
 * /tmp/mx68k_P54_plan.md §4. C99 §6.10.1: undefined macros eval to 0.
 *
 * Semantics:
 *  - P54_ENABLE: master compile gate. 0 = identical to Test#64 baseline.
 *  - P54_RELAX_DELTA_GATE: when 1, gate-3 delta criterion is
 *      (delta != 0 && delta % 6 == 0) regardless of
 *      P52_DETECT_SINGLE_PUSH_ENABLE. When 0, falls back to existing
 *      P52_DETECT_SINGLE_PUSH_ENABLE selection (identity).
 *  - P54_DIAG_GATE3_EXIT_SR: ring-log gate-3 reject events with exit SR.
 *  - P54_DIAG_GATE45_RING: ring-log gate-4/5 reject events with sigs.
 *  - P54_RING_CAP: per-ring depth.
 *
 * Default values reflect the P54-A1 measurement step. To revert to Test#64
 * behavior in one line, set P54_ENABLE 0.
 */
#define P54_ENABLE                       0
#define P54_RELAX_DELTA_GATE             1
#define P54_DIAG_GATE3_EXIT_SR           1
#define P54_DIAG_GATE45_RING             1
#define P54_RING_CAP                     8

/* ---- P55 ablation switches (direction-discriminated DETECT + vector-fetch seed hook) ----
 * /tmp/mx68k_P55_plan.md §4. C99 §6.10.1: undefined macros eval to 0.
 *
 * Semantics:
 *  - P55_ENABLE: master compile gate. 0 = identical to Test#65 baseline.
 *  - P55_A1_SIGNAL_ENRICH: append signed_delta, dir, exit_sr fields to
 *      [P52-DETECT] log line. Does NOT change which events fire — purely
 *      additional fields in the same printf.
 *  - P55_REQUIRE_PUSH_DIRECTION: when 1, gate the detect bucket on
 *      (signed_delta > 0) — i.e. only count/log push-real events.
 *      H6 quantitative confirmation toggle. Default 0 keeps Test#65 parity.
 *  - P55_A3_VECREAD_HOOK: ring-log even reads in [0x0000, 0x0400) whose
 *      returned word's hi-byte is non-zero (suspected seed of BasePC mask
 *      leak per c68kmac.inc:70-73).
 *  - P55_A3_RING_CAP: A3 ring depth (deeper than P54 because 256 vector
 *      slots × possible repeats).
 *  - P55_DEBUG_VERBOSE: optional per-chunk direction logging. Default 0.
 *
 * Default values reflect the P55-A1+A3 measurement step. To revert to
 * Test#65 behavior in one line, set P55_ENABLE 0.
 */
#define P55_ENABLE                       0
#define P55_A1_SIGNAL_ENRICH             1
#define P55_REQUIRE_PUSH_DIRECTION       0
#define P55_A3_VECREAD_HOOK              0
#define P55_A3_RING_CAP                  16
#define P55_DEBUG_VERBOSE                0

/* ---- P56 ablation switches (vec#$BC/$114 snapshot + verify-on-fetch drift detect) ----
 * /tmp/mx68k_P56_plan.md §4. C99 §6.10.1: undefined macros eval to 0.
 *
 * Semantics:
 *  - P56_ENABLE: master compile gate. 0 = identical to Test#66 baseline.
 *  - P56_DIAG_WHK: trace_Memory_WriteW hook on vec$BC/$114 (4 word addrs).
 *      log-only (val 改変なし). [P56-WHK] tag, pre/post phase 区別.
 *  - P56_DIAG_DRIFT: post-snapshot trace_Memory_ReadW で snapshot との
 *      diff 比較 -> [P56-VECDRIFT] log. snapshot 取得自体は本マクロ非依存
 *      (snapshot は MEM 直読みで取得され、本マクロは比較・log を gate する).
 *  - P56_SNAPSHOT_TRIGGER_FRAME: mx68k_run_frame 内 frame counter が
 *      この閾値に到達したとき snapshot を取得 (1 回限り). default 60
 *      (≈1 sec @60Hz; IPL ROM IOCS init + MFP init 完了想定).
 *  - P56_LOG_CAP: [P56-WHK] / [P56-VECDRIFT] の cumulative cap.
 *  - P56_DEBUG_VERBOSE: 1 で snapshot 4-byte hex dump 追加.
 *
 *  vec#$BC (TRAP#15 = IOCS dispatcher) RTE スタブ化禁止 — Spec Inv §4 / §7.6.
 *  本 Plan の WHK / DRIFT は全て log-only.
 */
#define P56_ENABLE                       0
#define P56_DIAG_WHK                     1
#define P56_DIAG_DRIFT                   0
#define P56_SNAPSHOT_TRIGGER_FRAME       60
#define P56_LOG_CAP                      128
#define P56_DEBUG_VERBOSE                0

/* ---- P57-A ablation switches (Bridge-side BasePC × vect × ReadW triangulation) ----
 * /tmp/mx68k_P57_plan.md §3 / §4 (v2).  Diagnostic-only — no behavior mutation.
 *
 * Semantics:
 *  - P57A_ENABLE: master compile gate. 0 = identical to Test#67 baseline.
 *  - P57A_DIAG_SETPC:        chunk-pair BasePC entry/exit + delta log (H1).
 *  - P57A_DIAG_BPCDRIFT:     trace_Memory_ReadW BasePC drift sampling (H1/H4).
 *  - P57A_DIAG_VECT_SANITIZE: mx68k_diag_irqh_callback vect range check (H2).
 *  - P57A_DIAG_READW:        Memory_ReadW return-value upper-byte watch (H3).
 *  - P57A_FRAME_SAMPLE:      per-frame BasePC sample at focus_frame (case D).
 *  - P57A_FOCUS_FRAME:       observation window (current Pattern A1 stuck frame).
 *  - P57A_LOG_CAP:           per-tag cumulative cap for each P57A log family.
 *  - P57A_LOG_CAP_READW:     dedicated cap for [P57A-READW-POST] (vec-table read 高頻度のため、
 *                            H3 rate analysis 用に大きく設定; Code Minor-2).
 *  - P57A_DEBUG_VERBOSE:     1 emits clean-range samples (default 0).
 */
#define P57A_ENABLE                       1   /* P407で復帰: must-stay-green #4-#7が依存(s_p59g2_frame_id宣言の外側ゲート)、P403で誤って休止。
                                                 * ★共有インフラ注意(P407b調査確認済み): このマクロ配下で宣言される
                                                 * s_p59g2_frame_id は P60/61/62/63/64/65/66/67/70/71/72/74A/75A/76A/
                                                 * 77A/78A/79A/82A/82B の各プローブからも参照される「フレーム番号
                                                 * タグ付け」共有カウンタ。これらが1個でも有効なら本マクロを0に
                                                 * 戻すとコンパイルエラーになる — 次回プローブ整理時は必ず
                                                 * `grep -n "s_p59g2_frame_id"`で参照元の有効マクロを確認すること。 */
#define P57A_DIAG_SETPC                   1
#define P57A_DIAG_BPCDRIFT                0
#define P57A_DIAG_VECT_SANITIZE           1
#define P57A_DIAG_READW                   0
#define P57A_FRAME_SAMPLE                 0
#define P57A_FOCUS_FRAME                  345
#define P57A_LOG_CAP                      128
#define P57A_LOG_CAP_READW                256
#define P57A_DEBUG_VERBOSE                0

/* ---- P58-Z ablation switches (ReadW upper-byte sanitiser; treatment layer
 *      for P57A_DIAG_READW = H3 hypothesis confirmed by Test#68 Path Z) ----
 * /tmp/mx68k_P58_plan.md §3 / §5. Goal: enforce the implicit contract
 * `(Memory_ReadW return >> 16) == 0` at the c68k callback boundary so the
 * dirty hi-byte cannot reach SET_PC via READ_LONG_F (c68kmac.inc:88-90 HI
 * half未 mask, Core 改変禁止のため Bridge で吸収).
 *
 *  P58Z_ENABLE:        master compile gate. 0 = identical to Test#68 baseline.
 *  P58Z_MASK_HI:       1 = actually apply `val & 0xFFFFu` at return.
 *                      0 = log-only (no mutation, paired with marker).
 *  P58Z_DIAG_MARKER:   emit [P58Z-INSTALL] once on first ReadW (wiring proof)
 *                      and [P58Z-MASK-APPLIED] for first P58Z_LOG_CAP events
 *                      where the mask actually drops bits.
 *  P58Z_LOG_CAP:       cap for the mask-applied marker log (v2: 32,
 *                      raised from v1's 4 per Code Review Minor-2).
 */
#define P58Z_ENABLE                       0
#define P58Z_MASK_HI                      1
#define P58Z_DIAG_MARKER                  1
#define P58Z_LOG_CAP                      32

/* ---- P59-γ2 passive observation layer (BasePC pollution single-path locator) ----
 * /tmp/mx68k_P59_plan.md v3 §3. Goal: pin the BasePC-pollution origin to ONE of
 * candidate A (exception/vector fetch READ_LONG_F<<16),
 * candidate B (SET_PC macro re-installing an already-dirty logical PC),
 * candidate Spec (C68k_Set_PC missing the `& 0xFF000000` mask).
 *
 * PASSIVE observation only. The C68k_Exec call cadence is NOT changed (no
 * single-instruction stepper) — so CHECK_INT frequency, cycle accounting and
 * the 60fps timing stay byte-identical to the Test#69 baseline. The hook only
 * READS C68K.PC / C68K.BasePC after each chunk; it never mutates them.
 * Core (c68k / px68k) is NOT modified.
 *
 * Decision axis: the bit24-31 of the LOGICAL PC (`C68K.PC - C68K.BasePC`).
 * The raw 64-bit BasePC host pointer upper bits are NOT used for the verdict
 * (they are non-zero even in healthy operation — see v3 §2.1).
 *
 * DEPENDENCY: P59G2_ENABLE requires P57A_ENABLE. The per-chunk observation
 * hook (Edit C) additionally requires P57A_DIAG_SETPC, because it reuses the
 * existing `p57a_entry_basepc` / `p57a_entry_pc_raw` / `p57a_entry_pc_log`
 * chunk-entry snapshots, which are declared under
 * `#if P57A_ENABLE && P57A_DIAG_SETPC`. The hook is therefore guarded by
 * `#if P59G2_ENABLE && P57A_ENABLE && P57A_DIAG_SETPC`, so a build with either
 * P57A_ENABLE=0 or P57A_DIAG_SETPC=0 simply disables P59G2 without a compile
 * error.
 *
 *  P59G2_ENABLE:       master compile gate. 0 = identical to Test#69 baseline.
 *  P59G2_LOG_CAP:      cap for the [P59G2-CHUNK-PC] cumulative trail.
 *  P59G2_RESET_CHUNKS: chunk_id threshold below which a first-drift is judged
 *                      to be the reset-seed path (candidate Spec).
 */
#define P59G2_ENABLE                      1   /* P407で復帰: must-stay-green #4-#7が依存(s_p59g2_frame_idの宣言+毎フレームincrement)、P403で誤って休止 */
#define P59G2_LOG_CAP                     128
#define P59G2_RESET_CHUNKS                2

/* ---- P59-γ3/γ4 drift-triggered ring-buffer capture (BasePC drift context) ----
 * /tmp/mx68k_P59g4_plan.md (γ4 supersedes /tmp/mx68k_P59g3_plan.md). Supersedes
 * the P59G2 fixed first-128-chunk trail (P59G2_LOG_CAP), which Test#70 showed is
 * consumed inside frame=1 BEFORE the BasePC drift appears.
 *
 * P59-γ4 records EVERY chunk into a circular ring buffer of depth
 * P59G3_RING_DEPTH (power-of-two). The trigger now watches the registered-base
 * equality invariant: when a chunk EXITS with C68K.BasePC not equal to the
 * registered C68K.Fetch[] base for the page of the current logical PC — i.e.
 * the c68k SET_PC upper-byte fold (`BasePC -= A & 0xFF000000`) has run and
 * BasePC has drifted off every registered region base — the ring (RING_DEPTH
 * chunks of BEFORE context) is dumped, the trigger chunk is flagged, and
 * P59G3_AFTER_CHUNKS chunks of AFTER context are logged. One-shot per boot;
 * re-armed on hard reset.
 *
 * This registered-base-equality axis is INDEPENDENT of the [P59G2-FIRST-DRIFT]
 * logical-PC-upper-byte axis: the logical-PC upper byte is observed always 0x00
 * (PC and BasePC re-base together and cancel via Get_PC = PC - BasePC), so γ4
 * watches the raw host BasePC against Fetch[page] instead. The dump tags remain
 * [P59G3-*] — same diagnostic feature, corrected observation axis.
 *
 * PASSIVE observation only: no extra C68k_Exec, no PC/BasePC/SSP mutation, no
 * masking (the trigger adds one plain array load of C68K.Fetch[]). #if
 * P59G3_ENABLE 0 => byte-identical to the Test#70 baseline.
 *
 * DEPENDENCY: P59G3 reuses the P57A chunk-entry snapshots (p57a_entry_pc_log
 * etc.) and the P59G2 exit-side locals, so it is guarded by the SAME triple as
 * the P59G2 Edit-C hook: #if P59G3_ENABLE && P57A_ENABLE && P57A_DIAG_SETPC.
 *
 *  P59G3_ENABLE:       master compile gate. 0 = identical to Test#70 baseline.
 *  P59G3_RING_DEPTH:   N — circular ring depth (BEFORE context). MUST be a
 *                      power of two (the ring uses `& (DEPTH-1)` masking).
 *  P59G3_AFTER_CHUNKS: M — number of chunks logged AFTER the trigger chunk.
 */
#define P59G3_ENABLE                      0
#define P59G3_RING_DEPTH                  64   /* N — power of two */
#define P59G3_AFTER_CHUNKS                16   /* M */

/* P59-γ10 FIX: vector table hi-word top-byte clear.
 * IPLROM 0xFF05BE init loop stores the vector number in bits 31-24 of each
 * handler address. Real X68000 24-bit external bus ignores the top byte;
 * c68k retains it internally → SET_PC dirtifies BasePC. When enabled (=1),
 * trace_Memory_WriteW clears bits 15-8 of val before storing into hi-word
 * vector table slots (addr < 0x400, addr & 2 == 0).
 * Set to 0 to revert to Test#76 (P59-g9) baseline.
 * P82-X-T (Outcome C confirmed non-culprit, /tmp/mx68k_P82-X-T_plan.md §4.1):
 *   revived to 1 — baseline restored to P82-X-R era (3 patches enabled) so
 *   the new CP-T-* probes observe alongside the original patch signal. */
#define P59G10_FIX_ENABLE                 1
#define P206_VACANT_SENTINEL_RESTORE      1   /* P206: 1 = restore IPLROM vacant-vector sentinel (disable pin/strip cluster); 0 = original Phase-1 behavior */

/* P48-C-REFIRE-WORKAROUND (NEW macro, P82-X-S §3.1).
 * Wraps the IOC bit1 0->1 rising-edge IRQ refire at
 * Bridge/m68000_bridge.c trace_Memory_WriteB hook (~line 11045). When 0,
 * the outer block compiles out; static state s_p48c_ioc_intstat_prev /
 * s_p48c_refire_count remain (unused-static warnings tolerated) so the
 * CP-R-8 audit emitter and the reset hook continue to compile.
 * P82-X-T (Outcome C confirmed non-culprit, /tmp/mx68k_P82-X-T_plan.md §4.1):
 *   revived to 1 — baseline restored to P82-X-R era. */
/* P154: disabled — obsolete re-fire workaround. It re-injected an FDD-insert
 * level-1 one-shot that MPX/real-HW legitimately drop, landing a spurious IPL1
 * in the IPLROM boot-device poll (icount 1026837, guest PC FF0F36) -> FF145C ->
 * FF063C halt. Predates the P68 reset drain + P143b Timer-C + P144 preset
 * removal, so its compensation is stale. Bridge-only, Core untouched. */
#define P48C_REFIRE_WORKAROUND_ENABLE     0

/* P60 probe: SR IPL-field force-clear on first entry to the IPLROM FDC
 * boot-sector read routine (0xff756c). SR=0x2614 (IPL=6) inherited from an
 * MFP exception context permanently masks the level-1 FDC completion IRQ
 * so RAM $000974 is never updated. Force IPL=0 to let the IRQ through.
 * Set to 0 to revert to Test#77 (P59-g10) baseline.
 * P82-X-T (Outcome C confirmed non-culprit, /tmp/mx68k_P82-X-T_plan.md §4.1):
 *   revived to 1 — baseline restored to P82-X-R era. */
#define P60_PROBE_ENABLE                  0

/* P61 probe: track writes to guest RAM $000974 (FDC completion flag).
 * The IPLROM FDC boot-sector read subroutine (0xff756c) reads this flag
 * immediately after sending FDC command bytes, with no polling loop.
 * If the FDC completion IRQ fires after the read, the flag is set too late.
 * This probe records when $000974 is written (up to P61_WRITE_LOG_MAX times)
 * to determine whether the FDC completes at all, and whether it completes
 * before or after the IPLROM reads the flag.
 * Set to 0 to revert to Test#78 (P60) baseline. */
#define P61_PROBE_ENABLE                  0
#define P61_WRITE_LOG_MAX                 8

/* P62 probe: FDC command-loop execution check.
 * Probe-A: on first WriteW with PC in 0xff756c..0xff7800, log A1 register
 *          to confirm the param-block pointer value.
 * Probe-B: on first WriteW with PC in 0xff7576..0xff757b, log to confirm
 *          the FDC command loop body (BSR $ff779e) was entered at least once.
 * If probe-B never fires, the A1 param block first byte is 0x00 and the
 * FDC command loop is skipped entirely, explaining why no FDC commands
 * are ever sent to the hardware registers (P42-DIAG-FDCW = 0 events).
 * Set to 0 to revert to Test#79 (P61) baseline. */
#define P62_PROBE_ENABLE                  0

/* P63 probe: IOCS FDD disk-read failure root cause identification.
 * Probe-A: on first WriteW with PC in 0xff0624..0xff062f (error handler TRAP#14
 *          call region), read guest RAM 0xBC-0xBF (TRAP#15 vector) and log it.
 *          If 0xff05e4 (panic placeholder): H1 confirmed -- IOCS handler never
 *          installed. If other address: real handler exists, look at H2/H3.
 * Probe-B: on reads to guest addr 0xe94001 (FDC Status Register), log the
 *          returned value (expect 0x80=ready, 0x00=not-ready, 0xff=bug).
 * Probe-C: per-frame FDD_IsReady(0) state log in mx68k_run_frame -- shows when
 *          (if ever) the FDD drive becomes ready for IOCS disk-read commands.
 * Set to 0 to revert to Test#80 (P62) baseline. */
#define P63_PROBE_ENABLE                  1   /* P407で復帰: must-stay-green #4供給元、P403で誤って休止 */
#define P63_FDCST_LOG_MAX                 6
#define P63_FDDREADY_LOG_MAX              30

/* P64 probe: identify the IOCS function number that triggers the error path.
 * Probe-A: on first WriteW with C68K.PC at 0xff0632 (TRAP#14 handler entry),
 *          log C68K.D[0] (the IOCS function number, preserved across the
 *          panic-placeholder call chain) and the current TRAP#15 vector value.
 * Probe-B: on the first call to mx68k_run_frame (frame=1), snapshot the
 *          TRAP#15 vector value from guest RAM for baseline comparison.
 * Together they identify which IOCS call fails and whether the TRAP#15
 * vector is still the panic placeholder at the time of failure.
 * Set to 0 to revert to Test#81 (P63) baseline. */
#define P64_PROBE_ENABLE                  1   /* P407で復帰: must-stay-green #5供給元、P403で誤って休止 */

/* P65 probe: determine whether the IOCS handler table slot for func 0x81
 * (RAM 0x0604) holds the real handler or the Stage-A default filler when
 * the failing IOCS dispatch occurs. Also snapshots SRAM boot-config fields.
 * Probe-A: WriteW monitor on addr 0x0604 (captures Stage A/B table writes).
 * Probe-B: ReadW monitor on addr 0x0604 (captures dispatcher read at dispatch time).
 * Probe-C: SRAM[0x1E..0x21] / [0x26] snapshot at frame=1.
 * Set to 0 to revert to Test#82 (P64) baseline. */
#define P65_PROBE_ENABLE                  0
#define P65_IOCSTBL_LOG_MAX               8

/* P65 SRAM fix: kept SEPARATE from P65_PROBE_ENABLE so Test#83 can run
 * probes-only (fix=0) to cleanly determine H-A vs H-B.
 * Default: 0 (probes only, no guest-behavior change in Test#83). */
#define P65_SRAM_FIX_ENABLE               0

/* --- P66: IOCS function-number identification probe (Probe D + Probe E) --- */
#define P66_PROBE_ENABLE      0   /* set 0 to revert to P65/Test#83 baseline */
#define P66_IOCSFN_LOG_MAX    8   /* ring buffer capacity */

/* P67 probe: capture TRAP#14 exception source via vector fetch (RAM 0x0B8).
 * One-shot: fires on the first TRAP#14 and logs stacked PC + D7. Set to 0
 * to revert to Test#84 (P66) baseline. */
#define P67_PROBE_ENABLE    0

/* P69-A: FDC/DMAC ブート停止の診断 probe 群。
 * 1 = 有効（診断、挙動変更なし）。0 = 全 probe をコンパイル除外（rollback）。
 * Probe-A/B は m68000_bridge.c、Probe-C は EmulatorBridge.c run_frame 内。 */
#define P69_PROBE_ENABLE                  1   /* P407で復帰: must-stay-green #6供給元、P403で誤って休止 */

/* P70-A: 0xff063c パニック診断計測（割り込みベクタ監視 + 0xE94005 ワード読み
 *        計測 + パニック経路メモリアクセストレース）。全プローブ完全 read-only。
 *        =0 で全プローブを除去。 */
#define P70_PROBE_ENABLE                   0

/* P71-A: FDD IRQ1 (vec#0x61) dispatch 時の vector-table 実値診断 probe。
 *        Probe-F1 = vec#0x61 受理スナップショット、
 *        Probe-F2 = 0x184/0x186 への書き込みウォッチ。
 *        全 probe は観測専用（CPU/メモリ/割り込み状態を変更しない）。
 *        =0 で全 P71 probe をコンパイル除外（P70/Test ベースラインへ revert）。 */
#define P71_PROBE_ENABLE                   1   /* P407で復帰: must-stay-green #7供給元、P403で誤って休止 */

/* P72-A: vec#0x61 スロット (0x184/0x186) の全 WriteW について全フック適用後の
 *        最終値と 32-bit slot 全体を観測する post-write probe。
 *        =0 で全 P72 probe をコンパイル除外。 */
#define P72_PROBE_ENABLE                   0

/* P73-A: IOC vec slot (0x180/0x184/0x188/0x18C) pre-initialization fix.
 *        mx68k_reset_hard() の m68000_reset_p47d_counters() 完了後に
 *        4 スロット全てを P49-A handler (0x000FFF20) へ向ける。
 *        =0 でコンパイル除外。 */
#define P73A_ENABLE                        0

/* P74-A: panic-path diagnosis probes.
 *  G1 — extend P47-D-DIAG-H trigger to include 0xff063c (direct panic-terminus
 *        entry without going through 0xff0632/0xff05e4; gives PC ring trail).
 *  G2 — add full 32-bit dispatch address to P67 TRAP#14 vector-fetch log
 *        (only vec_hi was logged previously; need full_vec for target analysis).
 *  =0 でコンパイル除外。 */
#define P74A_ENABLE                        0

/* P75-A: TRAP#14 panic origin diagnosis probes.
 *  G1 — first TRAP#14 vector-table read observer (fires in trace_Memory_ReadW
 *        at addr=0x0B8; exception frame already on supervisor stack)
 *  G2 — first entry to ISR/dispatcher/error-handler chunk-start trigger
 *  =0 でコンパイル除外。 */
#define P75A_ENABLE   0

/* P76-A: TRAP#14 vector slot (0x0B8/0x0BA) write observer.
 *  Observes every WriteW to the TRAP#14 exception vector to determine
 *  when and what installs the handler address. P75-A observed
 *  vec14=0x00FF0632 at first dispatch — this probe traces all writes
 *  to find the install source.
 *  =0 でコンパイル除外。 */
#define P76A_ENABLE   0

/* P77-A: TRAP vector table (0x080-0x0BE, TRAP#0-#15) write observer.
 *  Captures every WriteW to TRAP exception vectors to map the full
 *  handler installation. Specifically tracks TRAP#15 (0x0BC/0x0BE)
 *  which should be the IOCS dispatcher entry point.
 *  =0 でコンパイル除外。 */
#define P77A_ENABLE   0

/* P78-A: TRAP#14 entry deep stack-walk probe.
 *  When PC first enters 0xFF0632 (TRAP#14 error handler), reads 32 bytes
 *  (8 longwords) from the supervisor stack to reconstruct the call chain.
 *  Fires up to 2 times (once per TRAP#14 event). =0 でコンパイル除外。
 *  DISABLED: P73-A以降 trace_Memory_WriteW でPC==0xFF0632 の条件が成立しないため
 *  デッドプローブ。P64-TRAP14ENTRYと同条件のため同様に不発。 */
#define P78A_ENABLE   0

/* P79-A: TRAP#14 entry stack-walk co-located inside P64 detection block.
 *  Guaranteed to fire whenever P64-TRAP14ENTRY fires (same PC detection).
 *  Reads 8 longwords (32 bytes) from SSP to reconstruct the call chain
 *  leading to the error handler. =0 でコンパイル除外。
 *  DISABLED: P64-TRAP14ENTRYがP73-A以降発火しないためデッドプローブ。
 *  (Test#101で確認: P64-TRAP14ENTRY=0, P79-A=0) */
#define P79A_ENABLE   0

/* P82-A: Timer-C firing / speed-table-service diagnostic probes.
 *  -1: speed-table-svc entry (PC=0xFF05E4 chunk-start) — D7/D0/A6/MFP snapshot
 *  -2: TCDCR write interceptor (WriteB to 0xE8801D; cap P82A_TCDCR_LOG_CAP)
 *  -3: Timer-C IPRB[5] rise detector (per-H-line; cap P82A_TIMERC_RISE_CAP) */
#define P82A_ENABLE             0
#define P82A_TCDCR_LOG_CAP     20
#define P82A_TIMERC_RISE_CAP   10

/* P82-I: IPLROM TRAP/IOCS ディスパッチ帯域 (0xFF05E8-0xFF0640) の
 *  データアクセス粒度トレース。frame>=20 で武装、PC 帯域内の各 read/write を
 *  256 エントリリングに記録（pc/sr/kind/d0/d7/a5/a6/addr/val）。
 *  pc が 0xFF0632-0xFF063C に到達時 one-shot ダンプ。純粋 read-only 診断。 */
#define P82I_ENABLE   0

/* P82-J: CPU 例外ベクタフェッチ観測プローブ。IPLROM ブートが致命エラー
 *  ハンドラ guest 0xFF0632 へ落ちる際、どの CPU 例外が・どの faulting PC で
 *  発生し・ハンドラ 0xFF0632 へ遷移したかを一次証拠として捕捉する。
 *  c68k の例外/TRAP 処理は READ_LONG_F(vect*4) で必ず登録済みフック
 *  trace_Memory_ReadW を経由するため、ベクタフェッチを DATA read として
 *  観測可能。faulting PC は例外スタックフレーム (A7+2) から直読する。
 *  低位領域 0x000000-0x0007FF の 4 整列 word read を 128 エントリリングに
 *  記録し、pc が 0xFF0632-0xFF063C 到達時 one-shot ダンプ。純粋 read-only。 */
#define P82J_ENABLE   0

/* P82-K: IPLROM Line-F→TRAP#14 panic 分類データ捕捉プローブ。P82-J が観測実証
 *  済みのベクタフェッチ点（read フック内）に、致命エラー報告ゲート TRAP#14
 *  (vec#46, guest 0xB8) ディスパッチ時の D7 分類コード全 longword + D0/A5/A6/A7、
 *  および直前の vec#11 (Line-F, guest 0x2C) を発火させた faulting PC / オペコード
 *  を追加記録する。vec#46 フェッチ検出時に 48 エントリリングを one-shot ダンプ。
 *  純粋追加・read-only 診断。frame>=18 で武装。 */
#define P82K_ENABLE   0

/* P86-F: IPLROM メモリ fill ループ ($FF7AD4/$FF7AE2 系、MOVE.L D2,(A0)+ / DBRA D0)
 *  の書込先 EA 範囲を捕捉する診断プローブ。frame90 panic 直前に fill が稼働中の
 *  SSP ($1ff6 近傍) / 低位ベクタテーブル (0x0-0x3ff) を上書きし自己破壊している
 *  という仮説 (§1.3) を、書込 EA の min/max と SSP/ベクタ域 overlap で検証する。
 *  既存 P82-I write callback idiom を 1:1 流用 (PC-gate された write 記録 + chunk-hook
 *  0xFF0632-0xFF063C one-shot dump)。加えて仮説非依存の犯人 PC 特定のため、EA<0x400
 *  (ベクタ域) への write を直近 8 件リングに記録する (R2 反映: first-only でなくリング)。
 *  純粋追加・read-only 診断。frame>=60 で武装。 */
#define P86F_ENABLE   0

/* P82-L: IPLROM Line-F 上流の RTE 復帰 PC 破壊点を絞り込むための診断プローブ。
 *  P82-K Test#113 で確定した vec#11 fault_pc=0x1FCC・vec#46 D7=0・Timer-C
 *  entry stkPC=0x187e187e の連鎖 (P82-J ring) から、「直前の RTE が壊れた PC を
 *  pop した」ことが c68k PUSH/POP 監査結果と整合する唯一の上流原因と確定。
 *  本プローブは (1) 各例外エントリ時の A7..A7+0x1C 8-longword スタック窓と
 *  (2) Timer-C ハンドラ帯域 (0xFF15A2-0xFF1700 / 0xFF0B5A-0xFF0C00) 退出直後の
 *  PC 遷移を独立リングに記録し、PC=0xFF0632-0xFF063C 到達で one-shot ダンプ。
 *  純粋追加・read-only。frame>=18 で武装 (P82-J と同期)。 */
#define P82L_ENABLE   0

/* P82-M: per-chunk fine-grained stacktop observation probe.
 *  P82-L Test#114 GO で「user mode SR=0x0000 + 破損 PC を pop した RTE」
 *  経路が確定したが、L-RTE は per-chunk 粒度で 0 件捕捉。本プローブは
 *  per-chunk hook 内で PC ∈ IPLROM かつ A7 ∈ [0x1F80, 0x2000) の時のみ
 *  A7 と (A7+0..3, A7+4..7) を 64-entry ring に (A7,top0,top1) tuple
 *  dedup で記録し、panic-band 到達時 one-shot ダンプ。純粋追加・read-only。
 *  frame >= 18 で arm (P82-J と同期)。P82-J/K/L 構造体・リングは未変更。 */
#define P82M_ENABLE   0

/* P82-O: IPLROM CPU-clock self-measurement calibration probe.
 *
 * Diagnostic-only. Samples MFP Timer-C state (TCDCR/TCDR/IPRB/IMRB),
 * D-register / SR / A-register evolution, IPRB[5] rising edges, and
 * ISR vector-table fetches (addr=0x114) during the IPLROM calibration
 * routine 0xFF0AA6-0xFF0B58. Latches `reached_FF0B34` (RTS) and
 * captures A7 / *A7 there so the caller of 0xFF0AA6 can be identified.
 * One-shot dump at panic-band PC ∈ [0xFF0632, 0xFF063C].
 *
 * Constraints: side-effect-free reads (direct MFP[] / Timer_Count[]),
 * no change to chunk size / gating / interrupt delivery. Setting
 * P82O_ENABLE 0 reverts to baseline byte-equivalent.
 */
#define P82O_ENABLE       0
#define P82O_FRAME_ARM    18
#define P82O_RING_SIZE    64

/* P82-P: MFP IRQ delivery diagnostics. Four independent rings record
 *   - chunk-PC trace (non-dedup, N=256) for entry artifact confirmation
 *   - A7 word-dump (dedup by (a7,stk0,stk1), N=32) for canonical stack signature
 *   - $114/$116 vector-fetch watchpoint (N=16) — H-D1/H-D2 partition
 *   - TCDCR-write watchpoint (N=16) — 0x30 write at 0xFF0B42 confirmation
 * One-shot dump at panic-band PC ∈ [0xFF0632, 0xFF063C]. Read-only, no
 * MFP/CPU state mutation. P82P_ENABLE=0 reverts to baseline byte-equivalent. */
#define P82P_ENABLE              0
#define P82P_FRAME_ARM           18
#define P82P_VFETCH_FRAME_ARM    16
#define P82P_TRACE_RING_SIZE     256
#define P82P_STACK_RING_SIZE     32
#define P82P_VFETCH_RING_SIZE    16
#define P82P_TCDCR_RING_SIZE     16

/* P82-Q: IRQ-pending lifecycle probe. Five rings observe the temporal
 * sequence of Timer-C IRQ delivery (assert → mask → accept → vector
 * fetch → ISR → EOI) per chunk. Read-only sibling of P82-P.
 *   Ring A — MFP state per chunk (IPRA/IPRB/IMRA/IMRB/ISRA/ISRB/VR + TCDR snapshot)
 *   Ring B — CPU pending (IRQLine, ipl_mask, IRQH_IRQ[6])
 *   Ring C — ISR band entry/exit (vec 0x114 / 0x116 target bands)
 *   Ring D — MFP IPR/ISR/IMR ReadB piggyback ($E8800B/D/F/$E88011/13/15)
 *   Ring E — MFP IPR/ISR/IMR/VR WriteB piggyback (same + $E88017)
 * One-shot dump at panic-band PC ∈ [0xFF0632, 0xFF063C]. No state
 * mutation. P82Q_ENABLE=0 reverts to baseline byte-equivalent. */
#define P82Q_ENABLE              0
#define P82Q_FRAME_ARM           16
#define P82Q_MFP_RING_SIZE       128
#define P82Q_CPU_RING_SIZE       64
#define P82Q_ISR_RING_SIZE       32
#define P82Q_IPRBRD_RING_SIZE    16
#define P82Q_IPRBWR_RING_SIZE    16

/* P82-R: Timer-C tick rate + calibration cycle probe.
 * Three rings observe the IPLROM CPU-clock self-measurement routine
 * (0xFF0AA6 entry → 0xFF0B34 RTS) to partition H-D3-γ:
 *   Ring F — TCDR/TCDCR/IPRB/IMRB + abs_cycle, non-dedup, calibration band
 *   Ring G — chunk boundary cycle delta (IPLROM scope)
 *   Ring H — key-event abs_cycle snapshot (cal entry / cal RTS+D0 / Timer-C
 *            fire / panic). ev=2 d0 is the primary datum for §4.3 confirmation.
 * Read-only, side-effect-free reads only. One-shot dump at panic-band
 * PC ∈ [0xFF0632, 0xFF063C]. P82R_ENABLE=0 reverts to baseline byte-equivalent. */
#define P82R_ENABLE              0
#define P82R_FRAME_ARM           16
#define P82R_TCDR_RING_SIZE      128
#define P82R_CHUNK_RING_SIZE     32
#define P82R_EVENT_RING_SIZE     16

/* P82-S-A: calibration spin D1 trace + cal_entry->Timer-C fire delta probe.
 * P82-R 有効が前提（Ring H・s_p82r_abs_cycles を read 流用）。
 * P82S_ENABLE=1 かつ P82R_ENABLE=0 はビルド不可（m68000_bridge.c で #error）。
 * Ring I — D1 spin trace (non-dedup, 64 slot) over PC ∈ [0xFF0B48,0xFF0B58].
 * cal_entry edge detector — own PC-range landmark [0xFF0AA6,0xFF0B48).
 * DELTA footer — cal_entry→Timer-C fire / →panic delta cycle 実測。
 * Read-only diagnostic. P82S_ENABLE=0 reverts to baseline byte-equivalent. */
#define P82S_ENABLE        0
#define P82S_FRAME_ARM     16   /* この frame 以降のみ記録 (P82-R と同値) */
#define P82S_D1_RING_SIZE  64   /* Ring I (D1 spin trace) slot 数 */

/* ---- P82-T-A: interrupt accept -> ISR jump 経路 段階特定 probe ----
 * 1987 X68000 emulator debug probe. 診断専用、完全 read-only。
 * Ring A — interrupt accept ring (mx68k_diag_irqh_callback piggyback)
 * Ring B — accept 後 chunk-PC trace ring (per-chunk hook)
 * 段判定 — vect==0x45 厳密一致 + first-Timer-C one-shot ラッチ。
 * P82T_ENABLE 0 で P82-S 時点と byte-equivalent に戻る。
 * Plan: /tmp/mx68k_P82T_plan.md §4 / §9. */
#define P82T_ENABLE            0
#define P82T_FRAME_ARM         16   /* P82-R/S と同値 */
#define P82T_ACCEPT_RING_SIZE  32
#define P82T_PCTRACE_RING_SIZE 32

/* P82-B: Timer-C vector slot write/read observer.
 *  -1: WriteW to 0x114/0x116 (Timer-C vec slot) — who installs the handler
 *  -2: ReadW from 0x114/0x116 (Timer-C interrupt dispatch) — what handler is fetched
 *  -3: WriteW to 0xE8801D (TCDCR word-write path) — catch missed TCDCR writes */
#define P82B_ENABLE   0

/* ---- P82-U-A: guest $114/$116 (Timer-C vec #0x45) WriteW 全件記録 probe ----
 * 1987 X68000 emulator debug probe。診断専用・完全 read-only。
 * 不正値 0x00FF0B5A の write 側を捕捉し書込元 PC を特定する。
 *   ring   — 非 dedup 全件 WriteW ring (64 slot)
 *   latch  — first-bad (slot32 初 0x00FF0B5A) を ring eviction 耐性付きで保存
 *   dump   — panic band 0xFF0632-0xFF063C で one-shot (P82-T dump 直後に相乗り)
 * P82U_ENABLE 0 で P82-T 時点と byte-equivalent に戻る。
 * Plan: /tmp/mx68k_P82U_plan.md §4 / §9. */
#define P82U_ENABLE      0
#define P82U_FRAME_ARM   16   /* dump trigger 専用 gate（記録側は frame 0 から） */
#define P82U_RING_SIZE   64   /* WriteW ring slot 数（= longword 32 件分） */

/* ---- P82-W: calibration ルーチン全経路 chunk 粒度 band トレース probe ----
 * 1987 X68000 emulator debug probe。診断専用・完全 read-only。
 * 2 段構え:
 *   Stage 1  per-chunk band trace — calibration 9 band (ROM/RAM/ISR 区別)
 *            を run-length dedup ring に記録 (m68000_execute per-chunk hook)
 *   Stage 2  IRQ callback piggyback — calibration 中の interrupt accept を
 *            記録・計数 (mx68k_diag_irqh_callback)。M3/M4 切り分け。
 * frame [ARM,END] 限定の専用 chunk 縮小ゲートを併設 (g_trace_enable 非流用)。
 * P82-W は P82-R の s_p82r_abs_cycles を read 流用 (時間軸)。
 * P82W_ENABLE 0 で現 main(1783f2f) と byte-equivalent に戻る。
 * Plan: /tmp/mx68k_P82W_plan.md §3-§7. */
#define P82W_ENABLE          0
#define P82W_FRAME_ARM       16   /* P82-R/S/T と同値 — calibration entry 取り逃し防止 */
#define P82W_FRAME_END       24   /* chunk 縮小帯の上限 (panic frame ~21-22 + 余裕) */
#define P82W_TRACE_CHUNK     100  /* 縮小 chunk cycle 数 (≒25 命令) */
#define P82W_BAND_RING_SIZE  128  /* Stage 1 band ring slot (run-length dedup) */
#define P82W_IRQ_RING_SIZE   16   /* Stage 2 Timer-C accept ring slot */

/* ---- P82-X-Y: trap#14 cascade full snap (CP-Y-1 単独) ----
 * 1987 X68000 emulator debug probe。read-only / behaviour-change なし。
 *   CP-Y-1 (PRIMARY)   : trap#14 vec ($000000B8) fetch 時に A7 64 bytes + 全
 *                        Dx/Ax + IOC/MFP latch を 1-shot snap (P67 並列, 隣接 block)。
 * Sub-cycle P82-X-Y-fix1 で CP-Y-2 (chunk shrink + PC-window ring) は削除済み。
 * 削除理由: chunk=16 を frame band [80,95] (trap 帯) に適用すると
 * c68k CHECK_INT 頻度が ~256 倍化し IRQ delivery timing が変わり、IPL panic
 * phase の cmp/beq 分岐方向が反転して canonical $FF062A path が消失した
 * (Test#149 NO-GO)。precedent 主張 ("P82W chunk=16") は事実誤認 (実値
 * P82W_TRACE_CHUNK=100)。本 fix1 は CP-Y-1 単独投入で P67 invariant 復帰を
 * 最優先する。
 * P82XY_ENABLE 0 で全 #if ブロックが消滅し pre-P82-X-Y とバイト等価に戻る。
 * Plan: /tmp/mx68k_P82-X-Y-fix1_plan.md §2-§3. */
#define P82XY_ENABLE              0

/* ---- P82-X-B: 0x1fcc 領域 RAM バイト計測プローブ ----
 * 1987 X68000 emulator debug probe。純粋追加・完全 read-only。
 * 0x1fcc 近傍の guest RAM 実バイト像を 3 経路で計測する:
 *   C1  write-watcher — 0x1fcc 領域への CPU 書き込みイベントを逐次記録
 *   C2  at-fault snapshot — Line-F 例外発生時点の MEM[] window dump
 *   C3  calibration コピー snapshot (副次) — 転送元/先バイト比較
 * ゲストメモリへの書き込みゼロ・制御フロー非変更・trace_Memory_ReadW 非再入。
 * P82XB_ENABLE 0 で全ブロックが消滅しバイト等価に戻る。
 * Plan: /tmp/mx68k_P82-X-B_plan.md 付録 A.
 *
 * P82-X-C (下記) 開始に伴い 0 へ設定 — P82-X-B の C1/C2/C3 出力が
 * debug.log を埋めないようにするため。コードはツリーに残置 (削除せず)。 */
#define P82XB_ENABLE     0

/* ---- P82-X-C: ROM バイト読み取り経路 計測プローブ ----
 * 1987 X68000 emulator debug probe。純粋追加・完全 read-only。
 * IPLROM クロック校正コピーの ROM バイト読み取りが trace_Memory_ReadB に
 * 到達するか否かを「仮定せず実測」し、C 側で算出した単一 VERDICT
 * (H2/H3/H4/DOWNSTREAM/AMBIGUOUS) を emit する。5 構成要素:
 *   A  Read_Byte/Read_Word コールバック登録ログ (one-shot)
 *   B  trace_Memory_ReadB 内テンプレート帯バイト読みリング
 *   C  Line-F (vec#11) フォールト時の dump + C 算出 VERDICT
 *   D  RAM バッファ窓 [0x1FCA,0x1FF8) 再 dump
 *   E  ビルド由来トークン (init 時 1 回出力・スタールビルド検出)
 * ゲストメモリへの書き込みゼロ・制御フロー非変更。
 * P82XC_ENABLE 0 で全ブロックが消滅しバイト等価に戻る。
 * P82-X-B シンボルは一切参照しない (両ブロック独立)。
 * Plan: /tmp/mx68k_P82-X-C_plan.md §4. */
#define P82XC_ENABLE     0

/* ---- P82-X-E: frame 87 FDC 失敗判定の入口計装プローブ ----
 * 1987 X68000 emulator debug probe。純粋追加・完全 read-only・挙動非変更。
 * frame 87 の FDC コマンド処理失敗 → TRAP#14 (vec#46) panic に至る入口を計測する。
 * 構成:
 *   ① FDC アクセス履歴リング 2 本（Ring-W = コマンド/パラメータ/ワークエリア
 *      書き込み, Ring-R = ステータス/結果/データ読み込み, 各 64 段）
 *   ② TRAP#14 (vec#46) dispatch 時の全レジスタ + 両リングの per-run ワンショット
 *      ダンプ（vec#46 ベクタフェッチ addr==0xB8 を独立判定ブロックで検出）
 *   ③ build-provenance マーカー（init 時 1 回 + ダンプヘッダに併記）
 *   ④ DMAC ch0 レジスタ書き込み捕捉（Ring-W に併記）
 * ゲストメモリへの書き込みゼロ・c68k レジスタは context 直読みのみ・
 * trace_Memory_Read* / Write* 非再入。per-run リセットは m68000_reset_p47d_counters()。
 * P82XE_ENABLE 0 で全ブロックが消滅しバイト等価に戻る。
 * 既存 P82-K / P67 / P63-FDCST / P61 / P43 / P42 のコードは一切変更しない。
 * Plan: /tmp/mx68k_P82-X-E_plan.md §2-§5. */
#define P82XE_ENABLE     0

/* ---- P82-X-F: frame 87 FDC 割り込み配送経路の判別計装プローブ ----
 * 1987 X68000 emulator debug probe。純粋追加・完全 read-only・挙動非変更。
 * frame 87 で IPLROM が Recalibrate 後に前進できない原因を「FDC 完了割り込みの
 * 配送経路のどこで止まっているか」に絞って実測判別する（候補 C1=未配送 /
 * C2=配送済みだが不整合）。構成:
 *   ① IOC 割り込みレジスタ アクセスリング（guest 0xe9c000-0xe9c003 read/write, 32 段）
 *   ② FDC 割り込みベクタ フェッチ トレイル（guest 0x180-0x18F read, 16 段）
 *   ③ Recalibrate 発行時 SR スナップショット（0xe94003 write frame>=80, 8 段）
 *   ④ TRAP#14 (vec#46) dispatch 時の per-run ワンショットダンプ + VERDICT
 *      （vec#46 ベクタフェッチ addr==0xB8 を P82-X-F 独立判定ブロックで検出）
 *   ⑤ build-provenance マーカー（init 時 1 回 + ダンプヘッダに併記）
 * ゲストメモリへの書き込みゼロ・Core 関数呼び出しゼロ・c68k レジスタは
 * context 直読みのみ・trace_Memory_Read* / Write* 非再入。per-run リセットは
 * m68000_reset_p47d_counters()。P82XF_ENABLE 0 で全ブロックが消滅し
 * バイト等価に戻る。既存 P82-X-E / P82-K / P67 / P63-FDCST / P70-IRQVEC の
 * コードは一切変更しない。Plan: /tmp/mx68k_P82-X-F_plan.md §3-§5. */
#define P82XF_ENABLE     0

/* P82-X-H: vec 0x60 FDC 割り込みハンドラ内部の命令経路トレース診断プローブ。
 * タイプ A（挙動非変更・read-only・純粋追加）。frame 87 boot 停止の真因が
 * (a) ハンドラが SenseInterruptStatus を発行したか (b) early-return したか
 * (c) ISR が書き本体が読む RAM 完了フラグの所在 — を観測のみで確定する。
 * これは権威定義（単一）。m68000_bridge.c / EmulatorBridge.c は双方とも
 * EmulatorBridge.h を include 済みなので mirror 定義は不要（P82XF と同方式）。
 * P82XH_ENABLE 0 で全 #if ブロックが消滅し pre-P82-X-H とバイト等価に戻る。
 * Plan: /tmp/mx68k_P82-X-H_plan.md §3-§5. */
#define P82XH_ENABLE     0

/* P82-X-K: good FDC ST0=0x20 と IPLROM panic 終端 0xff063c の間の boot-path
 * 分岐点を観測のみで切り分ける診断プローブ。タイプ A（挙動非変更・read-only・
 * 純粋追加）。CP-1 SIS result-phase バイト対（ordinal-tagged）/ CP-3 panic
 * 終端への PC trail / CP-4 FDC ReadData・DMAC ch0 到達 / CP-5 IPLROM RAM
 * work-area write/read 相関 を観測し、(a)/(b)/(c)/(d) を分類する。
 * これは権威定義（単一）。m68000_bridge.c / EmulatorBridge.c は双方とも
 * EmulatorBridge.h を include 済みなので mirror 定義は不要（P82XH と同方式）。
 * P82XK_ENABLE 0 で全 #if ブロックが消滅し pre-P82-X-K とバイト等価に戻る。
 * Plan: /tmp/mx68k_P82-X-K_plan.md §2-§5. */
#define P82XK_ENABLE     0

/* P82-X-K VERDICT one-shot — m68000_bridge.c で定義、EmulatorBridge.c の
 * per-frame 経路から frame>=96 到達時に呼ばれる（header prototype のため
 * 各 .c での個別 extern 宣言は不要）。 */
void p82xk_emit_verdict(void);

/* ---- P82-X-L diagnostic probe: Recalibrate->ReadData transition ---- */
/* Recalibrate が good ST0=0x20 で完了したのに IPLROM がセクタ読み出しルーチン
 * 0xff909a に入らず trap#14(0xff0628) へ分岐する — その判定枝を観測のみで切り
 * 分ける診断プローブ。タイプ A（挙動非変更・read-only・純粋追加）。CP-A $E94003
 * FDC コマンド書き込み / CP-B DMAC ch0 レジスタ書き込み（厳密スコープ）/ CP-C
 * post-Recalibrate チャンクバンド PC trail / CP-D FDC ドライバ RAM work-area
 * read/write 相関 / CP-E $E9C001 IOC 割り込みステータス読み出し を観測する。
 * これは権威定義（単一）。m68000_bridge.c / EmulatorBridge.c は双方とも
 * EmulatorBridge.h を include 済みなので mirror 定義は不要（P82XK と同方式）。
 * P82XL_ENABLE 0 で全 #if ブロックが消滅し pre-P82-X-L とバイト等価に戻る。
 * Plan: /tmp/mx68k_P82-X-L_plan.md §2-§5. */
/* P82-X-L disabled this cycle (P82-X-M, plan /tmp/mx68k_P82-X-M_plan.md):
 * the prior cycle's findings are superseded — see plan §1.1; running P82XL
 * alongside P82XM doubles instrumentation in the perturbation-sensitive
 * frame-86 FDC window. Setting to 0 compiles out all P82XL guards (verified
 * orphan-free: m68000_bridge.c:3664/6586/7399/7659/11163 and
 * EmulatorBridge.c:1641). */
#define P82XL_ENABLE      0
#define P82XL_PROBE_TOKEN "P82XL-PROBE-L1"
#if P82XL_ENABLE
/* P82-X-L VERDICT one-shot — m68000_bridge.c で定義、EmulatorBridge.c の
 * per-frame 経路から frame>=96 到達時に呼ばれる。 */
void p82xl_emit_verdict(void);
#endif

/* ---- P82-X-M diagnostic probe: boot-sector DMA delivery / IPLROM
 *      bootability-reject pinpoint (TYPE A — read-only, pure addition) ---- */
/* IPLROM は cmd 0x46 (MFM ReadData, C=0/H=0/R=1/N=3) で boot sector を発行し
 * DMA で 1024 バイトを RAM に転送する手はずを整える。それでも frame 90 付近で
 * IPLROM 自身が trap #14 (0xff0628) へ分岐 — 「disk not bootable」の意図枝へ。
 * boot sector が実際に MAR の指す RAM に届いたか、届いていれば中身は intact か、
 * IPLROM の result-status / signature 判定はどう転んだのか — を観測のみで切り
 * 分ける診断プローブ。タイプ A（挙動非変更・read-only・純粋追加）。
 *   CP-A′: DMAC ch0 レジスタ writeB+writeW（MAR 再構成・MTC・OCR・CCR）
 *   CP-B′: $E94003 read 全件 + ReadData(0x46)-armed result-phase latch
 *   CP-C′: MAR が指す RAM 領域 (1024 バイト) の guest read 観測
 *   CP-D′: チャンクバンド PC trail + FDC ドライバ RAM-flag read + vec60 fetch
 * 観測窓 frames 80-95、verdict one-shot at frame>=96。
 * これは権威定義（単一）。m68000_bridge.c / EmulatorBridge.c は双方とも
 * EmulatorBridge.h を include 済みなので mirror 定義は不要（P82XK/-L と同方式）。
 * P82XM_ENABLE 0 で全 #if ブロックが消滅し pre-P82-X-M とバイト等価に戻る。
 * Plan: /tmp/mx68k_P82-X-M_plan.md §3-§5. */
#define P82XM_ENABLE      0
#define P82XM_PROBE_TOKEN "P82XM-PROBE-M1"
#if P82XM_ENABLE
/* P82-X-M VERDICT one-shot — m68000_bridge.c で定義、EmulatorBridge.c の
 * per-frame 経路から frame>=96 到達時に呼ばれる。内部に g_verdict_done ガード
 * があり (TU-local to m68000_bridge.c)、多重呼び出しでも 1 回だけダンプする。 */
void p82xm_emit_verdict(void);
#endif

/* ---- P82-X-N diagnostic probe: boot-sector DMA delivery proof / boot-stub
 *      execution tripwire (TYPE A — read-only, pure addition) ----
 * P82-X-M Test#136 の verdict `classification=M-DMA-MISDIR` は probe false
 * positive (cf. /tmp/mx68k_P82-X-N_plan.md §1) で本サイクルで撤回。P82XM は
 * §3.2 perturbation 抑制方針により本サイクルで disable (P82XM_ENABLE 1->0)。
 * CP-A: MEM[0x2000..0x23FF] スナップショット (boot-event triggered)
 * CP-B: host .xdf 1024 byte と CP-A の byte-for-byte 比較
 * CP-C: IPLROM signature 領域 (0x2000..0x201F) read 監視 (ReadW 含む)
 * CP-D: FDC-driver RAM-flag ($0970 等) read/write 交差観測
 * CP-E: chunk-PC sampling 方式 boot stub 実行 tripwire (0x2000..0x207F)
 * 観測窓 frames 0-95、verdict one-shot at frame>=96。
 * これは権威定義 (単一)。m68000_bridge.c / EmulatorBridge.c は双方とも
 * EmulatorBridge.h を include 済みなので mirror 定義は不要 (P82XK/-L/-M と同方式)。
 * P82XN_ENABLE 0 で全 #if ブロックが消滅し pre-P82-X-N とバイト等価に戻る。
 * Plan: /tmp/mx68k_P82-X-N_plan.md §3-§5. */
#define P82XN_ENABLE      0
#define P82XN_PROBE_TOKEN "P82XN-PROBE-N1"
#if P82XN_ENABLE
/* P82-X-N VERDICT one-shot — m68000_bridge.c で定義、EmulatorBridge.c の
 * per-frame 経路から frame>=96 到達時に呼ばれる。内部に p82xn_verdict_done
 * ガード (TU-local to m68000_bridge.c) があり、多重呼び出しでも 1 回だけ出力。 */
void p82xn_emit_verdict(void);
void p82xn_cpa_tick(void);
#endif

/* ---- P82-X-O diagnostic probe: IPLROM 0xFF0628 panic 経路の例外配信 vec /
 *      直前メモリアクセス ピンポイント診断プローブ (TYPE A — read-only,
 *      pure addition) ----
 * P82-X-N Test#137 確定: DMA は MAR=0x2000 にバイト完全一致で配信済 (CP-B
 *  1024/1024) だがゲストは 0x2000-0x23FF を実行せず IPLROM PC=0xFF0628 で
 *  trap#14 発火 (P67 stacked_pc=0xFF062A, trap_src=0xFF0628, D7=0)。
 * 投資フェーズで判明: 0xFF0628 はパニックハンドラ (シグネチャ check では
 *  ない)。Lff05e4 → Lff05fc → magic-word recovery 失敗 → Lff0622 →
 *  Lff0628 trap#14 経路に byte-for-byte 一致。つまり vec 2 (bus error) /
 *  vec 3 (address error) が _B_READ tail で発火、override されていない
 *  default vector で panic に到達した可能性が最有力 (Spec Inv H5)。
 *   CP-O-1: ベクタテーブル longword フェッチ (vec value==0xFF05E4 で latch)
 *   CP-O-2: ベクタテーブル longword フェッチ広 ring (filter 無、補強)
 *   CP-O-3: 例外ベクタテーブル snapshot (frame anchor 3 点)
 *   CP-O-4: IPLROM 内 PC trail filter for _B_READ 系帯
 *   CP-O-5: 直前メモリアクセス ring (RAM-shadow / DMAC ch0 / FDC 限定)
 * 観測窓: arm from frame 5 (P82XO_ARM_FRAME), verdict one-shot at frame>=96。
 * これは権威定義 (単一)。m68000_bridge.c / EmulatorBridge.c は双方とも
 * EmulatorBridge.h を include 済みなので mirror 定義は不要。
 * P82XO_ENABLE 0 で全 #if ブロックが消滅し pre-P82-X-O とバイト等価。
 * Plan: /tmp/mx68k_P82-X-O_plan.md §3-§5. */
/* P82-X-O: dormant since P82-X-P (1→0 累積摂動排除イディオム — P82XL/M/N と同)。
 * 全 #if P82XO_ENABLE ブロックは 0 評価で消滅し pre-P82-X-O とバイト等価。
 * 既存 hook 呼び出し（p82xo_on_vector_fetch / p82xo_cpo2_record / p82xo_cpo3_take /
 *   p82xo_cpo4_note_chunk / p82xo_cpo5_record / p82xo_tick / p82xo_emit_verdict /
 *   p82xo_reset）は全て #if P82XO_ENABLE で wrap 済 (orphan-free)。 */
#define P82XO_ENABLE      0
#define P82XO_PROBE_TOKEN "P82XO-PROBE-O1"
#define P82XO_ARM_FRAME   5u
#if P82XO_ENABLE
void p82xo_emit_verdict(void);
void p82xo_tick(void);          /* per-frame: CP-O-3 snapshot anchor driving */
#endif

/* ====================================================================
 * P82-X-P diagnostic probe (TYPE A — read-only, pure addition)
 *  P82-X-O VERDICT=O-OTHER-VEC-0x00 (pc=0x7A0496) の probe artifact 切り分け:
 *   CP-P-A: BasePC drift 直接検証 (vec fetch 間 BasePC tracker, enum 3 値分類)
 *   CP-P-B: c68k group-0 14-byte frame stacking 実装有無メタプローブ
 *           (a7 起点 14-byte raw dump + pattern_verdict 分類)
 *   CP-P-C: chunk-PC trail filter for PC=0xFF0628 帯
 *   CP-P-D: BusErrFlag/BusErrHandling 直接 polling (D-1 trace ring / D-2 per-frame
 *           sticky / D-3 BusErrHandling sticky-edge ring for DMAC clear-race)
 *   CP-P-E: HYPOTHESIS-P backup (A7 sampling / 0x7A region write watch /
 *           IOCS B_READ ledger)
 * 観測窓: arm from frame 5 (P82XP_ARM_FRAME), verdict one-shot at frame>=96。
 * Plan: /tmp/mx68k_P82-X-P_plan.md §3-§6. */
/* P82-X-Q: dormant — Plan §1.1 / §5. CP-P-A through CP-P-E are kept in code
 * (revival path) but mechanically skipped to remove cumulative probe
 * perturbation while the BPC-safe accessor proves itself. */
#define P82XP_ENABLE      0
#define P82XP_PROBE_TOKEN "P82XP-PROBE-P1"
#define P82XP_ARM_FRAME   5u
#if P82XP_ENABLE
void p82xp_emit_verdict(void);
void p82xp_tick(void);          /* per-frame: CP-P-D D-2 sticky edge sample */
#endif

/* P82-X-R (Round 5 post-Codex) — 真の panic 原因 並列観測プローブ群
 * (CP-R-1〜8、Type-A read-only). Provenance: P82XR-PROBE-R1。
 * 実装は Bridge/m68000_bridge.c 内 #if P82XR_ENABLE ブロック。
 * EmulatorBridge.c の frame loop から per-frame tick と frame≥96 で
 * verdict emit を呼ぶための extern 宣言 (P82XR_ENABLE 0 でも no-op stub
 * が定義され orphan-free). reset_all は TU-local static 維持。 */
void p82xr_emit_verdict(uint32_t frame);
void p82xr_tick(void);

/* ---- P82-X-T diagnostic probe family (TYPE A — read-only, pure addition) ----
 * BUS-ERR-FRAME-SNAP + IRQ-LOSS-GATE 同時観測 fresh family (CP-T-1〜T-6).
 * 既存 P82-X-S Outcome C で確定した「3 patches は真因ではない」を baseline
 * として、faulting access の正体 (Spec Path B / bus-err 14-byte frame) と
 * 上流 IRQ 配送の silent loss を 1 サイクルで同時観測する。
 *  CP-T-1 BUS-ERR-FRAME-SNAP  — 0xFF05E4 entry (frame>=80 AND D7==0) で
 *                               A7+0..17 (bsr return 4 + bus-err frame 14)
 *                               を 1 行 snap
 *  CP-T-2 DMA-CCR-GATE        — window 80-95、DMA[0] CSR.COC|BTC または
 *                               MTC→0 edge で (CCR/CSR/MTC/NIV/EIV/IRQH[3]) を
 *                               最大 8 行記録
 *  CP-T-3 IOC-INTSTAT-GATE    — window 80-95、IOC_IntStat 直読みで bit7
 *                               立ち上がり edge を最大 8 行記録 (IRQH[1] 同伴)
 *  CP-T-4 PC-TAIL-TRAIL       — distinct PC を 8 段 ring に保持、PC ==
 *                               0xFF0626 または 0xFF05E4 初回で 1 行 8 cell dump
 *  CP-T-5 FDC-STATUS-TRAJECTORY — window 80-95、1 frame 1 行で
 *                                 (FDC_Read(0xE94001), IOC_IntStat, IRQH[1/3], PC)
 *  CP-T-6 DMA-NIV-EIV-CHECK   — stage A (frame=5) + stage B (frame=90 or
 *                               0xFF062A) の 2 行で NIV/EIV/CCR delta 観測
 * 全 probe は read-only、emit token は P82XT-PROBE-T1〜-T6。総 emit 40 行/run 以下。
 * Plan: /tmp/mx68k_P82-X-T_plan.md §3-§5. */
/* P82-X-V (H5-C confirmation cycle): re-enable CP-T-1 macro flip — the
 * highest-yield zero-LOC observation. CP-T-2..T-6 also reactivate but are
 * baseline overlap with prior P82-X-T verdicts (no new information; Build &
 * Test Agent annotates the re-emit accordingly). Plan §3.1 / §4. */
#define P82XT_ENABLE                      0
void p82xt_tick(void);                /* per-frame: CP-T-2/T-3/T-5/T-6 */

/* ---- P82-X-U diagnostic probe family (TYPE A — read-only, pure addition) ----
 * D-class INVESTIGATE FIRST cycle (Plan: /tmp/mx68k_P82-X-U_plan.md §3-§5).
 * Six TYPE-A read-only probes pinpointing the IPLROM hybrid completion-wait
 * break point, decision-rule output is hypothesis H1〜H5 from §6 outcome matrix.
 *   CP-U-1 FDC-MSR-AND-BUFREADY-TRACE — per-frame sampler (window 80-95, 15 cap)
 *                                       MSR-derived CB/DIO/RQM + FDC_IsDataReady()
 *                                       + IOC_IntStat + IRQH_IRQ[1/3] + PC
 *   CP-U-2 FDC-SETINT-CALLPATH-MARKER — per-chunk IOC_IntStat bit7 rising-edge
 *                                       (window 5-95, 16 cap). bit7 rise =
 *                                       FDC_SetInt invocation marker (Spec §Q3).
 *   CP-U-3 IPLROM-PC-WINDOW-CLASSIFY  — per-frame PC band classify
 *                                       (window 60-95, 15 cap). H4 pin-point.
 *   CP-U-4 IRQH-IRQ-WRITE-TRACE       — per-chunk IRQH_IRQ[1] transition log
 *                                       (window 5-95, 8 cap). H3 race detect.
 *   CP-U-5 $C90-RESULT-BUFFER-PEEK    — one-shot at frame=90; dump drv 0/1
 *                                       (16 bytes via p47_read_long_le).
 *                                       H2 IRQ handler drain evidence.
 *   CP-U-6 $E9C001-WRITE-TRACE        — extends existing $E9C001 write hook
 *                                       in m68000_bridge.c (8 cap). H1 vs H3
 *                                       supporting evidence — bit2 set timing.
 * 全 probe は read-only、emit token は P82XU-PROBE-U1〜-U6。総 emit ≤ 64 行/run。
 * P82XU_ENABLE 0 で全 #if ブロックが消滅し pre-P82-X-U とバイト等価に戻る。 */
/* P82-X-V: P82-X-U observation cycle has concluded (Test#145 GO, FDC
 * pipeline end-to-end confirmed). Disable to reduce probe noise during
 * the P82-X-V H5-C confirmation cycle. */
#define P82XU_ENABLE                      0
void p82xu_tick(void);                /* per-frame: CP-U-1/U-3/U-5 driver */

/* ---- P82-X-V diagnostic probe family (TYPE A — read-only, pure addition) ----
 * H5-C PRIMARY confirmation cycle (Plan: /tmp/mx68k_P82-X-V_plan.md §3-§5).
 * Four new TYPE-A read-only probes + CP-T-1 re-enable (macro flip, 0 LOC).
 * Confirms the H5-C hypothesis: vector $6C (level-3 autovector) is left at
 * the IPL-init default thunk ($00FF05E4), so any stray IRQ/exception passes
 * through Lff05e4 → Lff05e8 magic-walk prologue → fails compare → Lff0622
 * (clr.w d7 / trap #14) → $ff062a HALT trampoline (the P67 fingerprint).
 *   CP-T-1 BUS-ERR-FRAME-SNAP    — re-enabled via P82XT_ENABLE=1; observes
 *                                   the 14-byte exception frame at $ff05e4
 *                                   entry (frame>=80 AND D7==0).
 *   CP-V-2 VEC-6C-STATE-MONITOR  — frame=5 (post-init) + frame=90 (panic-
 *                                   near) snap of vector slots at byte
 *                                   offsets $08 / $0C / $60 / $6C.
 *   CP-V-3 FF05E8-ENTRY-D7-SNAP  — first PC ∈ [$ff05e8..$ff0626] band,
 *                                   one-shot, frame>=80; snaps D7 (lower
 *                                   byte = vector_number / 4 after lsr.w).
 *   CP-V-4 FF0622-ENTRY-PROBE    — first PC ∈ [$ff0622..$ff0628] band,
 *                                   one-shot, frame>=80; snaps D7/SR/A7/A6
 *                                   + 8-slot distinct-PC trail dump.
 *   CP-V-5 IRQH-ALL-LEVELS-SAMPLER — frame 80..95 window, 1 row/frame
 *                                    (cap 16); IRQH[1..7] + IRQLine + IPL +
 *                                    MFP IPRA/B + DMA0 CSR/CCR snapshot.
 * 全 probe は read-only、emit token は P82XV-PROBE-V2 〜 -V5 + P82XV-PROBE-V4-TRAIL。
 * 総 emit ≤ 24 行/run (V2: 2 + V3: 1 + V4: 2 + V5: 16 + 1 buffer)。
 * P82XV_ENABLE 0 で全 #if ブロックが消滅し pre-P82-X-V とバイト等価に戻る。 */
#define P82XV_ENABLE                      0
#define P82XV_V2_FRAME_POSTINIT           5u    /* CP-V-2 snap #1: post-init */
#define P82XV_V2_FRAME_PANIC              90u   /* CP-V-2 snap #2: panic-near */
#define P82XV_WIN_LO                      80    /* CP-V-3/V-4/V-5 arm gate lower bound */
#define P82XV_WIN_HI                      95    /* CP-V-5 window upper bound (inclusive) */
#define P82XV_V3_PC_LO                    0x00FF05E8u /* magic-walk band lower (M-1 範囲化) */
#define P82XV_V3_PC_HI                    0x00FF0626u /* magic-walk band upper (inclusive) */
#define P82XV_V4_PC_LO                    0x00FF0622u /* clr.w d7 / swap.w d7 / Lff0626 band */
#define P82XV_V4_PC_HI                    0x00FF0628u /* band upper (inclusive) */
#define MX68K_VEC_BUSERR_ADDR             0x00000008u /* vector  2: bus error */
#define MX68K_VEC_ADDRERR_ADDR            0x0000000Cu /* vector  3: address error */
#define MX68K_VEC_SPURIOUS_ADDR           0x00000060u /* vector 24: spurious interrupt */
#define MX68K_VEC_6C_ADDR                 0x0000006Cu /* vector 27: level-3 autovector */
#define P82XV_PC_TRAIL_DEPTH              8u    /* CP-V-4 distinct-PC ring depth */
void p82xv_tick(void);                /* per-frame: CP-V-2 / CP-V-5 driver */
void p82xv_on_chunk_pc(uint32_t pc);  /* per-chunk PC: CP-V-3 / CP-V-4 */

/* P87-A: $FF0628 panic への到達が (i) CPU 例外 vector fetch 経由か (ii) 通常
 *  control-flow 経由かを判別する診断プローブ。既存 CP-R-1 (p82xr_on_vec_fetch /
 *  [P82XR-VERDICT]) を一切改変せず、独立フック p87a_on_vec_fetch を vec-fetch
 *  呼出点 (:10993) に追加し、3 ゲート (verdict_done/armed/$FF05E4 filter) を素通しで
 *  handler∈$FE0000-$FFFFFF の vector fetch を全件記録する。通常 ring 128 slot +
 *  discriminator (§1.5 D1∧(D2∨D3)) 合致「真候補」を circular latch 8 件に別保存。
 *  dump 発火は per-chunk PC hook (gated pc_now_h ∈ {$FF0632,$FF063C}、P87-A 専用
 *  one-shot s_p87a_dumped、:14186 付近) に置く (panic terminus は低位 vector を
 *  読まないため vec-fetch フック内 trigger は不発)。stacked 読みが sentinel でも
 *  discard せず記録。全 snap は MX68KQ_GUEST_PC() gated。純粋追加・read-only 診断。
 *  P87A_ENABLE 0 で全 #if ブロックが消滅し pre-P87-A とバイト等価に戻る。 */
#define P87A_ENABLE   0

/* P87-C (本 cycle 非実装・将来用 guard): handler-band 精密 D7 snap (CP-V-3 相当)。
 *  PREMISE が (i) 例外経路と確証され per-vector トランポリン前提が裏付くまで P88
 *  以降に deferral (PREMISE 未確定下では誤誘導)。実装する場合は本体コードを
 *  #if P87C_ENABLE で囲み OFF で codegen 同一を保証する (chunk gate の runtime
 *  if 不可)。本 cycle では本体コードを書かない (フラグのみ予約)。 */
#define P87C_ENABLE   0

/* P88-A0ELATCH: IOCS work 変数 $0A0E への word write を D7 主判別で latch する
 *  観測専用 probe。$FF0628 リテラル TRAP#14 panic へ合流する path(b)$FF1E46
 *  (D7=$01FF) / path(c)$FF1E5A (D7=$0100|func#) は bra $FF0626 直前に必ず
 *  $0A0E に #$FFFF を write する。既存 P66 Probe D が PC-window 分類で null
 *  (PC drift 疑い) だったため D7 で経路判別し、直前 $FF1E2A write の val
 *  (=func#) で culprit IOCS を得る。trace_Memory_WriteW 内 P66 直後に独立
 *  latch、dump は panic-terminus one-shot (s_p88_dumped)。全 snap は
 *  MX68KQ_GUEST_PC() gated。純粋追加・read-only — P66 を改変しない。
 *  P88_ENABLE 0 で全 #if ブロックが消滅し pre-P88 とバイト等価に戻る。 */
#define P88_ENABLE    0

/* P89: $FF0628 panic predecessor を supervisor-stack ウォーク (f) +
 *  L6 割込ベクタ ground-truth 直読 (g) で捕捉する観測専用 probe。gated-PC は
 *  panic band で $FF1760 に固着し命令間を判別しない (P70-TRACE 2048/2048 実測) ため、
 *  predecessor 機構の特定は gated-PC でなくスタック上の ground-truth (例外フレーム
 *  保存 PC/SR + BSR/JSR 戻りアドレス連鎖) と割込ベクタ直読で行う。dump は既存 P88
 *  dump 直後の panic-terminus one-shot (s_p89_dumped)。hot-path callback には一切
 *  挿入しない (P67 regression surface 最小)。純粋追加・read-only — 既存出力を
 *  改変しない。P89_ENABLE 0 で全 #if ブロックが消滅し pre-P89 とバイト等価に戻る。 */
#define P89_ENABLE    0

/* P90-PREDFRAME: $FF0628 panic の到達経路 (1=$FF05E4 例外 / 2=$FF1E40・$FF1E52 IOCS
 *  異常退出 / 3=$FF0600 エラー分類 fall-through) を ground-truth で判別する観測専用
 *  probe。経路1・2 は $FF0622 clr.w d7 / $FF0624 swap d7 をバイパスして $FF0626 へ
 *  分岐するため d7≠0 を保持し、経路3 のみ d7=0 になる (P67 実測 d7=0 と整合)。probe は
 *  (1) panic-entry d7 の 3-way 機械分類、(2) pre-panic SP (= a7+6, 実測 SP0=$1FFC)
 *  からの bounded スタックウォーク [SP0, $2000) を領域分類 ($2000 以降は boot-sector
 *  ロードデータで predecessor 判定除外) し、bsr/jsr opcode 検証を通った boot-decision
 *  predecessor の戻りアドレスを回収する。dump は既存 P89 dump 直後の panic-terminus
 *  one-shot (s_p90_dumped)。hot-path callback には一切挿入しない (P67 regression
 *  surface ゼロ)。read-only — guest メモリに書かず Core 関数を呼ばない。純粋追加で
 *  既存出力 (P88/P89/P82XG/P47D/P87-B) を1バイトも改変しない。P90_ENABLE 0 で全 #if
 *  ブロックが消滅し pre-P90 とバイト等価に戻る。 */
#define P90_ENABLE    0

/* P91-VECNUM: 元例外ベクタ番号 n を CPU 例外ディスパッチ (vec[n] フェッチ) 時点で
 *  ground-truth 直接捕捉する観測専用 probe。vec[n] の hi-word read (ReadW 経由、P67 が
 *  vec$0B8 で実証・Code-Review が c68k READ_LONG_F=Read_Word 経由を Core 直読で確認) で
 *  handler lo24==$FF05E4 (= default ハンドラ行き = 致命) のとき n=addr/4 と a7 上の
 *  group-1/2 例外フレーム (6byte SR+PC) を small ring に latch (最新上書き、panic 直前の
 *  最後の該当 fetch=culprit を保持)。決着軸は「panic は真の CPU 例外 (Codex) か制御フロー
 *  終端 (P87/P90) か」: ring 非空=例外発火 (n が例外源を一意確定)、ring 空=P91 ring 自身を
 *  主証拠に CP-T-1 (s_p82xt_t1_done) cross-check で経路漏れ/真に例外なしを機械判定。c68k は
 *  group-0 (bus/addr error=vec2/3) を一切ディスパッチしないため bus/addr は赤鯡・group-0
 *  解読は防御的 unreachable。dump は既存 P90 dump 直後の panic-terminus one-shot
 *  ($FF0632/$FF063C、s_p91_dumped)。callback は latch のみ・dump しない (P88 M2-loc 教訓)。
 *  P87-A/P87-B/CP-T-1/CP-R-6/P67 を一切改変せず独立 ring/判定を並走。read-only・guest
 *  メモリ非書込・Core 関数 (Set/Exec) 非呼出。純粋追加で既存出力を1バイトも改変しない。
 *  P91_ENABLE 0 で全 #if ブロックが消滅し pre-P91 とバイト等価に戻る。 */
#define P91_ENABLE    0

/* P92-EXCSIG: 例外 dispatch シグネチャ probe — 「panic は真の CPU 例外経由か /
 *  control-flow 終端か」を frame-push×vec-fetch 相関で確実裁定 (P91 宙吊りを解消)。
 *  主 (a): WriteW で supervisor-stack frame-push (PC long + SR word の 3 連続降順
 *  word write、seq>=3、ゲート addr∈[0x1EF0,0x2010)) を latch → ReadW で
 *  STEP1 positive control (addr==$B8 = TRAP#14 vector を lo24 filter の外で検出、
 *  handler lo24==$FF0632 assert、faulting PC==$FF062A を P67 cross-check) +
 *  STEP2 上流例外 (lo24==$FF05E4 の vec fetch、addr==$00 除外、frame-push と
 *  freshness 相関 write_ctr delta<=2) を弁別。傍証 (b): addr∈[0x1EF0,0x2010) かつ
 *  全 $xxFF05E6 longword read = handler $FF05E8 実行の直接証拠 + 元ベクタ番号 nn。
 *  (c): 既存 P47-D PC ring を terminus で read-only 再走査し computed jmp(a1)
 *  predecessor ($FF036E/$FF042A) を探索。VERDICT 評価順序
 *  EXC-DISPATCH-CONFIRMED > HANDLER-VIA-CONTROL-FLOW > NO-EXC-CONTROL-FLOW >
 *  INCONCLUSIVE。観測専用 (read-only)・emulation 不変・新規 hot-path callback ゼロ
 *  (既存 WriteW/ReadW に address-gated latch を pure addition)。既存
 *  P87/P88/P82XG/P47D/P89/P90/P91/CP-R/CP-T/P67 を1バイトも改変しない。
 *  P92_ENABLE 0 で全 #if ブロックが消滅し pre-P92 とバイト等価に戻る。 */
#define P92_ENABLE    0

/* P93-CFENTRY: panic 域 ($FF05E4-$FF0628) への control-flow 到達経路を ground-truth
 *  で確定する観測専用 probe。fork 決着対象: 仮説A (例外→$FF05E4 default handler) vs
 *  仮説B (IOCS 未実装 func# スタブ $FF1E4E/$FF1E62 の bra.w $FF0626) vs 第3経路
 *  (computed jmp / 直接到達)。判別子 G1-G6:
 *   G1 (corroborative): $FF05E8 の (a7)+ pop ($xxFF05E6 longword、hi-word first) を
 *      窓撤廃 (addr<0x10000) で生 latch。発火=handler 実行、nn=元ベクタ番号 (top byte)。
 *   G2 (validated primary): vec longword fetch (addr<0x400, aligned, lo24==$00FF05E4)
 *      を全ベクタクラス網羅 latch。$B8 positive control 検証済 (c68k vec fetch は ReadW
 *      経由) → 発火=例外 dispatch 確証、不発=「$FF05E4 行き例外なし」を信頼可。
 *   G3 (形状): $FF0632 entry one-shot で a6/a7 pristine latch。a6==a7+6 で $FF0626
 *      経由 / a6!=a7+6 で $FF0628 直接到達 (第3経路 D) を判別。
 *   G4 (master): panic-entry d7 (P90 entry_d7 を read-only 流用、未取得時 live 直読+
 *      clobber 注記)。d7=$01xx→仮説B、d7=0→仮説A or 第3経路 C/D。
 *   G5 (仮説B 弁別): $A0E word write ring + $FF1E32 push 検出 (lo-word $1E32 先・
 *      hi-word $xxFF 後の降順)。terminus で $A0E 直読が ground-truth。
 *   G6 (補助): handler 復帰パス ($FF05FC-$FF0620) read を live C68k_Get_PC gate で
 *      latch。観測あれば bus/addr error dispatch を示唆 (非 load-bearing)。
 *  VERDICT 7 分類 (評価順): EXC-TO-DEFAULT-HANDLER > EXC-VECFETCH-NO-HANDLERPOP >
 *   HANDLER-RAN-NO-VECFETCH > DIRECT-JUMP-TO-$FF0628 > IOCS-STUB-CONTROL-FLOW >
 *   DIRECT-CF-TO-$FF0626-D7ZERO > INCONCLUSIVE。観測専用 (read-only)・emulation 不変・
 *   新規 hot-path callback ゼロ (既存 ReadW/WriteW に address-gated latch を pure
 *   addition)。既存 P87/P88/P82XG/P47D/P89/P90/P91/P92/CP-R/CP-T/P67 を1バイトも
 *   改変しない。P93_ENABLE 0 で pre-P93 とバイト等価に戻る。 */
#define P93_ENABLE    0

/* P94-E4PTR: $nnFF05E4 ポインタ load の source slot を ground-truth で pinpoint。
 * trace_Memory_ReadW に value/region-gated latch を pure addition し、indirect
 * jmp に食わせた slot アドレス・nn (高位 byte=slot 番号)・region・live PC を捕捉。
 * 観測専用 (read-only)・emulation 不変・新規 hot-path callback ゼロ。既存
 * P87/P88/P82XG/P47D/P89/P90/P91/P92/P93/CP-R/CP-T/P67 を1バイトも改変しない。
 * P94_ENABLE 0 で pre-P94 とバイト等価に戻る。 */
#define P94_ENABLE    0

/* P95-ARRIVAL: $FF05E4 到達経路を bsr-push 直接検出で裁定 (ROUTE-HANDLER vs
 * ROUTE-EXPLICIT-BRA)。trace_Memory_WriteW に lo-first + adjacency gate の
 * pure-addition フックを置き、$FF05E4 の bsr.b が push する $00FF05E6 を捕捉、
 * その瞬間の A0-A7/D0-D7/SR を live snapshot して着地先 An を同定する。
 * 観測専用 (read-only)・emulation 不変・新規 hot-path callback ゼロ。既存
 * P87/P88/P82XG/P47D/P89/P90/P91/P92/P93/P94/CP-R/CP-T/P67 を1バイトも改変しない。
 * P95_ENABLE 0 で pre-P95 とバイト等価に戻る。 */
#define P95_ENABLE    0

/* P96-G1SNAP 観測専用。G1 handler-pop ($00FF05E6 pop) 認識時に A0-A7/D0-D7/SR
 * を live snapshot し、pre-pop A7 の $0 近接性・どの An が $00FF05E4 か・a7_live
 * 直上の supervisor stack frame coherence を測って $FF05E4 到達機序 (rts-from-~0
 * / movea($0) 間接 jump / default-vector 例外) を実測裁定する。新規 hot-path
 * callback ゼロ (既存 p93_on_readw 内の helper 呼出 + 既存 chunk hook の emit のみ)。
 * P93/P94/P47D/P90 state は read-only 流用。P96_ENABLE 0 で pre-P96 とバイト等価に戻る。 */
#define P96_ENABLE    0

/* P97-A7TRACE: A7 (SSP) を ≈$0 へ corrupt した機序 (sudden-load vs push-runaway
 * vs misaligned) を実測裁定する観測専用 probe。全 WriteW + ReadW で live A7
 * (C68k_Get_AReg) を sample し、signed delta 列・min A7 イベント・$400 crossing・
 * monotonic_run・$D4E4 (SCSI SSP-restore cell) read watch を記録、panic-terminus
 * ($FF0632/$FF063C) で one-shot dump。新規 hot-path callback ゼロ (既存
 * trace_Memory_ReadW/WriteW 内の cheap helper 呼出 + 既存 chunk hook の emit のみ)。
 * 分類基盤は min A7 イベントの delta/monotonic_run。s_p47d_pc_ring / P96 state は
 * read-only 流用。P97_ENABLE 0 で pre-P97 とバイト等価に戻る。 */
#define P97_ENABLE    0

/* P98-A7SRC: A7 を $0 へ LOAD した命令の SOURCE を実測裁定する観測専用 probe。
 * P97 が A7=$0 を bad LOAD (単一ステップ降下) と確定したのを受け、両 cell
 * ($D4DE / $D4E4 = SCSI SSP-restore 候補) を post-value ReadW で value-watch し、
 * crossing 直前 access の全レジスタ snapshot (memcpy、observation-only) で
 * register-source (movea.l An,A7) も露出、$D4E4 save-witness + $D4DE setup-write
 * witness で SCSI dispatch 進入有無を弁別。新規 hot-path callback ゼロ (既存
 * trace_Memory_ReadW/WriteW 内の cheap helper + 既存 chunk hook の emit のみ)。
 * P97/P95/P47D state は read-only 流用。P98_ENABLE 0 で pre-P98 とバイト等価。 */
#define P98_ENABLE    0

/* P99-STACKWRITE: supervisor-stack $1FF6 の $0 longword の出所を実測する観測専用 probe。
 * P98 が live-A7 で A7=$0 crossing を実測 (popped slot $1FF6 = $0、verdict γ) したのを受け、
 * (a) in-window write [$1FF4,$1FFC) を val 付きで ring latch ($1FF6 longword の writer 同定)、
 * (b) 自前 per-access A7 sampler + crossing latch (H1/H3 detector)、
 * (c) in-window READ の last-wins latch (H2 rte/PC=$0 discriminator、corroboration 限定)。
 * 重い処理 (longword 再構成・分類・VERDICT) は panic-terminus one-shot emit に集約。
 * P98/P97/P95/P47D state は read-only 流用。唯一の plumbing 変更は WriteW callsite で val を
 * probe フックに渡すこと (エミュレートされる write 自体は不変)。observation-only ゆえ
 * (read-only C68k_Get_AReg / memcpy のみ、guest write なし) cycle-accurate 不変。
 * P99_ENABLE 0 で pre-P99 とバイト等価。 */
#define P99_ENABLE    0

/* P100-STACK1FF8: $1FF8 の $0 longword write source + A7=$0 を起こす真の pop の同定。
 * P99 が $1FF8 を $0-write 唯一 slot と確定。P100 は writer の register 候補集合 + RAM/ROM
 * region (BasePC + Fetch-walk oracle、chunk-stale pred_pc 非依存) と真の pop source-read slot
 * (3-bucket + prior-access で P96 transfer-read と分離) を観測。全 pure addition、observation
 * -only (read-only C68k_Get_AReg / C68K.BasePC|D|A|Fetch / memcpy / p47_read_long_le のみ、guest
 * write なし) ゆえ cycle-accurate 不変。WriteW callsite の新規 plumbing なし (P99 の val 再利用)。
 * P98/P99/P97/P95/P47D state は read-only 流用。P100_ENABLE 0 で pre-P100 とバイト等価。 */
#define P100_ENABLE   0

/* P101-STACK1FFA: $1FFA pop の真の pop 命令同定 + top-of-stack longword 全 layout 裁定。
 * P98/P99/P100 は単一 prior-access slot で $1FF6/$1FF8/$1FFA を順に同定したが、これは
 * p47_read_long_le の word@addr=MS half / word@addr+2=LS half ゆえ一つの longword の半語を
 * +2 drift で誤同定した可能性 (half-word aliasing)。P101 は単一 slot を捨て (a) raw
 * read-sequence ring (全 ReadW の seq/addr/val/A7/BasePC/pred_pc、crossing 時 freeze) で
 * crossing-adjacent (addr,addr+2) ペアから真の longword BASE を pre-label なしで復元、
 * (b) widened window [$1FE0,$2000) (8 longwords/16 words) の per-WORD layout table を
 * as-rebuilt(再読み) と as-written(writer ring 再構成) の両方で出力、(c) writer-witness
 * ring (WR_RING=24) で $0 longword を構成する全 $0-write を region 付きで可視化、(d) 各 read
 * の BasePC+bank を read 系列全体で latch (bank-$FE fork arbiter)。value は split-hook
 * (pre-value push + post-value backfill) で真 bus 値。VERDICT は直交2ファミリー
 * IDENTITY{V1/V2/V3} + REGION{V4/V5/V6/V7} + 共有 escape{V8/UNCLASSIFIED}。
 * 全 pure addition、observation-only (read-only C68k_Get_AReg / C68K.BasePC|D|A|Fetch /
 * memcpy / p47_read_long_le のみ、guest write なし) ゆえ cycle-accurate 不変。
 * P98/P99/P100/P97/P95/P47D state は read-only 流用。P101_ENABLE 0 で pre-P101 とバイト等価。 */
#define P101_ENABLE   0

/* P102-REGFLIP: bank-$FE A7->$0 flip の機序を register-snapshot ring + A7-source-read 捕捉で
 * 同定する観測専用 probe。各 memory access で C68K.D[0..7]/A[0..7] を memcpy + a7/access_addr/
 * access_value/is_write/BasePC/pred_pc を ring(深さ8)に push。P101 と完全一致する crossing
 * predicate (seen_healthy && a7<0x400 && delta<0) で A7->$0 flip を latch、flip-snap と直前
 * access の prior-snap を freeze。A7-source read を half-word 2分割対応で再合成 (movea.l $40(a0),a7
 * は hi@a0+$40 -> lo@a0+$42 の 2 ReadW、decisive longword は architectural address で結合)。
 * 全 pure addition、observation-only (read-only C68k_Get_AReg / C68K.BasePC|D|A|Fetch / memcpy /
 * p47_read_long_le のみ、guest write なし) ゆえ cycle-accurate 不変。P98/P99/P100/P101/P47D
 * state は read-only 流用。P102_ENABLE 0 で pre-P102 とバイト等価。 */
#define P102_ENABLE   0

/* P103-A7CHANGE: ungated A7-change transition probe. P101 反証「$0 を stack から pop」、
 * P102 は gated (a7<$400) crossing で SITE-C/AB を測ったが、Codex P103 診断: BasePC oracle は
 * 最後の SET_PC translation base (実行中 PC でない) を測るゆえ bank fork は corroboration のみ。
 * P103 は (1) UNGATED に毎 access で live A7 を prev と比較し、変化した access を transition
 * ring に記録 (full register snapshot 付き) して $1FF8->...->$0 の全 A7 trajectory を可視化、
 * (2) new_a7==$0 を専用 latch、prior/pre-prior/flip snapshot + 「A7-write の source read (prior=lo
 * /pre-prior=hi) が値 $0 の memory READ だったか」(frozen-snapshot source-$0-read discriminator) を
 * 記録し register->SP movea (memory-invisible) と memory-sourced A7-load を弁別、(3) pre-flip
 * snapshot の zero-register 列挙 (D0 強調、source reg 候補)、
 * (4) flip 時 BasePC region を last-branch-target として (実行 PC でない明示ラベル付き) 報告、
 * (5) stack image が pre-/post-panic かを報告 ([$1FF8]=$00FF062A ordering artifact 検証)。
 * 全 pure addition、observation-only (read-only C68k_Get_AReg / C68K.BasePC|D|A|Fetch / memcpy /
 * p47_read_long_le のみ、guest write なし) ゆえ cycle-accurate 不変。P103_ENABLE 0 で
 * pre-P103 とバイト等価。 */
#define P103_ENABLE   0

/* P104-CLEANDISC: clean A7=$0 source discriminator (memory-sourced vs register-to-SP).
 * P103 で残った核心: A7 が $1FF8->$0 に flip した source が memory-sourced (movea.l (ea),a7
 * など、値の data-read を伴い longword==新 A7) か register-to-SP (movea.l Dn,a7 with Dn=$0、
 * memory-invisible、値の data-read なし) か未決着。P103 VERDICT は ordering check の
 * first-match で short-circuit し source 弁別を出力しなかった。P104 は (I) STACK-ORDERING
 * (post-panic 判定、corroboration) と A7-SOURCE (memory vs register、主目的) を常に独立計算
 * する 2 フィールドに decouple、(II) flip 直前に split pre/post hook で latch した half-word
 * pair を addr ベースで longword 再構成し reconstructed==A7-after($0) を MEMORY-SOURCED の
 * 必要条件とする A7==value gate (zero-reg corroboration と両証拠を常に併記、競合は AMBIGUOUS
 * に倒す)、(III) s_p104_boot_gen (reset-clear で increment-only bump、memset 対象外) +
 * latch-on-first-arm/freeze で single clean boot (reset#2 = post-mount) のみ観測。zero-register
 * は prior と flip の両時点で列挙併記 (D0 強調 = movea.l d0,a7 family 最有力)。全 pure addition、
 * observation-only (read-only C68k_Get_AReg / C68K.BasePC|D|A|Fetch / memcpy / p47_read_long_le
 * のみ、guest write なし) ゆえ cycle-accurate 不変。P104_ENABLE 0 で pre-P104 とバイト等価。 */
#define P104_ENABLE   0

/* P105-DNPIN: A7=$0 を作る register-to-SP movea の source register を opword で exact 同定する
 * 観測専用 probe (主目的) + D0=$0 源流の手掛かり取得 (副目的、falsifiable)。boot-stall 機序は
 * P104 で「A7=$0 source は register-to-SP movea 系 (memory-invisible)」と確定したが、どの register
 * が source かは未同定 (register-source movea は memory callback を出さない)。P105 は flip-latch 時に
 * prior transition snapshot を anchor として opword scan で 0x2E40..0x2E4F (movea.l Dn/An,a7) を列挙し、
 * src=opword&7 を frozen prior snapshot の zero-register と cross-check、後続 0x4E75 (rts) との構造整合で
 * source register を一意化する。anchor-consistency gate (pred_pc が prior.region band 内のときのみ scan)
 * + live C68k_Get_PC を第3 corroboration anchor。源流 latch は $9DF/$9E0/$ED0008-B/$ED000C/$ED0010 の値と
 * error-path 兆候 (D0=$0 を真因と bake-in せず error-path 進入 vs seed 差を 2 解釈併記)。全 pure addition、
 * observation-only (read-only C68k_Get_AReg/Get_DReg/Get_PC / C68K.BasePC|D|A|Fetch / s_ipl_fetch / MEM /
 * memcpy / p47_read_long_le のみ、guest write なし) ゆえ cycle-accurate 不変。P105_ENABLE 0 で pre-P105
 * とバイト等価。s_p105_boot_gen は reset-clear で increment-only bump (memset 対象外、P104 M3 踏襲)。 */
#define P105_ENABLE   0

/* P106-SINGLESTEP-ANCHOR: A7=$1FF8 -> $0 へ flip させる register-to-SP movea (movea.l Dn/An,a7) の
 * source register を exact 同定する。P101-P105 の BasePC anchor (last-branch-target、現命令 PC でない)
 * は 4 度 SCAN-NO-HIT。P106 は手段を変える: m68000_execute の while loop で live A7==$1FF8 を検出した
 * chunk だけ exec chunk を 1 命令に縮め (gated single-step)、各 step の call 直前に entry_pc=
 * MX68KQ_GUEST_PC() を latch。1 命令 chunk では全境界=命令境界ゆえ entry_pc は exact (BasePC anchor
 * バグ解消)。A7 が $0 へ flip した step の entry_pc = movea の PC。その opword を region-dispatch idiom
 * (IPL ROM=s_ipl_fetch[off^1] / RAM=MEM BE-direct、P105 reader 再利用) で読み 0x2E40|Dn / 0x2E48|An を
 * decode して source register を exact 同定 (A2/A3 を bake-in しない、Dn/An 全 16 到達可)。a7==0 単独で
 * SOURCE 判定せず必ず opword decode (非 0x2E4x は SOURCE-IS-OTHER、short-circuit 禁止)。
 *
 * ★摂動 (本サイクル最重要前提): P106 は厳密には observation-only でない。chunk を 4096->1 に縮めると
 * 割込 (CHECK_INT) サンプリング頻度が変わる (c68k は割込を C68k_Exec entry / chunk-exit でのみ check)
 * = 唯一の摂動源 (single-step 実行そのものでなく)。EmulatorBridge.h:704-716 (P82-X-Y-fix1) に既知の
 * 発火先例あり (chunk 16 -> CHECK_INT 頻度 ~256倍 -> IRQ timing 変化 -> cmp/beq 反転 -> canonical
 * $FF062A path 消失、Test#149 NO-GO)。よって VERDICT=PERTURBED は LIKELY な転帰。受入 arbiter は P67
 * byte-identical 実測 — 崩れたら結果無効。gating で blast radius を A7==$1FF8 の chunk のみに最小化
 * (post-flip panic path は 4096 chunk のまま)。実 flip (a7_after==0) 捕捉時のみ done を立て 4096 恒久
 * 復帰、cap=64 / A7 が $0 以外へ離脱 (non-flip exit) では done を立てず re-arm (次の A7==$1FF8 で再 step、
 * wrong-window 恒久 miss 防止)。
 *
 * 全コード P106_ENABLE で gating。P106_ENABLE 0 で single-step を一切行わず byte-equivalent。reuse は
 * READ-ONLY (C68k_Get_PC/AReg/DReg、C68K.D|A 直読 memcpy、P105 region reader / opword idiom)。P106-own
 * state 新設、既存 [P87]-[P105] / CP probes / P67 不改変 (pure addition)。s_p106_boot_gen は reset-clear で
 * increment-only bump (memset 対象外、P104 M3 / P105 踏襲)。 */
#define P106_ENABLE   0   /* P107/P109/P110 と排他: 両者が同 chunk を c=1 にしようと競合するため single-step サイクルでは 0。コードは残置 (pure addition、P106_ENABLE 0 で byte-equivalent) */

/* P107-ARBITER: A7=$1FF8 域 -> $0 へ flip する機序を movea (単発 register-to-SP) と exception-storm
 * (-6 ずつネストフレーム push の単調降下=SSP underflow) で実測裁定する。P104 は movea を、Spec は storm を
 * 主張し対立 — どちらも assume せず frame-late + bare 低 A7 gated single-step (1命令 chunk) で flip を
 * instruction-boundary で直接捕捉し、step 列の A7 delta 系列 + entry_pc 系列 + opword 系列で裁定する。
 *
 * P106 との違い: (1) arm 述語を A7==$1FF8 単一値でなく bare 低 A7 (a7!=0 && a7<=$1FF8) に拡張 — movea
 * ($1FF8 到達) も storm ($1FF6 から $1FF8 未経由で降下) も機序非依存に bracket。(2) done は真の flip
 * (a7_after==0) 捕捉時のみ — cap=512 / gate 域離脱 (a7>$1FF8) は re-arm (次に gate 成立で再 step) で
 * P106 の flip-occurrence exhaustion を解消。(3) per-step ring 深さ = cap (512) で storm 降下の onset
 * handler PC を silently 失わない (head+tail preserve + truncation log)。(4) flip-latch を機序非依存に:
 * a7_before が $1..$1FF8 域 かつ a7_after==0 の step を flip として latch (movea なら 1 step 大 delta、
 * storm なら最終 -6 step)。
 *
 * ★摂動: P106 同様 chunk を 4096->1 に縮めると CHECK_INT サンプリング頻度が変わる = 唯一の摂動源。
 * frame-late + bare 低 A7 + cap で blast radius を panic 近傍に限定。受入 arbiter は P67 byte-identical
 * (Build&Test 判定)。崩れたら VERDICT=PERTURBED で結果無効。★storm は IRQ-cadence 駆動ゆえ single-step が
 * 真 storm を隠しうる — PERTURBED/NO-FLIP を storm 反証と読んではならない。imminence (prev=$1FF6 &&
 * live=$1FF8) は arm に使わず dump 内 log-only hint のみ。
 *
 * range gate (A7<$2000 全域) は採らない (boot 全域 single-step = Test#149 型摂動 LIKELY)。flip_pc / 各
 * step の region は addr ベース判定 (BasePC oracle でない)。全コード P107_ENABLE で gating。P107_ENABLE 0
 * で single-step を一切行わず byte-equivalent。reuse は READ-ONLY (C68k_Get_AReg/DReg、C68K.D|A 直読
 * memcpy、p105_read_opword [P105_ENABLE=1])。movea decode は P107-own p107_decode_movea (p106 版は
 * P106_ENABLE=0 で compile out)。P107-own state 新設、既存 [P87]-[P106] /
 * CP probes / P67 不改変。s_p107_boot_gen は reset-clear で increment-only bump (memset 対象外)。 */
#define P107_ENABLE     0   /* P109/P110 と排他: single-step は同時 1 機構のみ active。P110 サイクルでは 0。コードは残置 (pure addition、P107_ENABLE 0 で byte-equivalent) */
#define P107_FRAME_ARM  80    /* boot 前半除外: flip は panic frame≈91 近傍。実測 flip frame の直前数フレーム (Build&Test が確認) */
#define P107_STEP_CAP   512   /* single-step 安全弁 = ring 深さ: movea (1 step) も storm (数百 step 降下) も 1 window で覆う中庸値 */

/* ====================================================================
 * P108-WRITEW-EXC-PUSH: WriteW で例外フレーム push を観測し A7 -> $0 flip が
 * exception-storm (frame-push 連鎖) か単発 movea (frame-push 不在) かを裁定する。
 *
 * 設計 (全 observation-only=byte-equivalent、single-step 不使用 = P107 の摂動源なし):
 *  (A) WriteW callback で live A7 = C68k_Get_AReg(&C68K,7) を読み、A7 が低位
 *      (a7 != 0 && a7 <= $1FF8) の時の WriteW を P108-own ring に
 *      {seq, addr, value(word), live_a7, frame} で記録。head+tail preserve
 *      (HEAD=descent onset、TAIL=$0 直前 terminal frames)。truncation も記録。
 *      timing 前提: c68k PUSH_32_F は A[7] -= 4 を Write_Word の前に実行ゆえ
 *      callback の live A7 は減算済で write addr に追従する。
 *  (B) frame-push 連鎖検出: 裁定は frame-push 有無でなく「$0 まで pop による A7
 *      戻りなく単調降下する frame-push 連鎖」(MINOR-1)。storm は例外 frame
 *      (3-word 群) の反復として数える (MINOR-2) — P92 frame-arm の連発を集計し
 *      生 word の単純降順 seq に依存しない。
 *  (C) IRQ-ACK ring cross-check: 既存 CP-R-2 IRQ ring を read-only 参照し
 *      反復 vector を storm 発火源候補として dump。
 *  (D) single clean boot 隔離: s_p108_boot_gen increment-only (memset 除外)。
 *
 * 全コード P108_ENABLE で gating。P108_ENABLE 0 で WriteW 観測を一切行わず
 * byte-equivalent。reuse は READ-ONLY (C68k_Get_AReg、CP-R-2 IRQ ring、
 * s_p92_frame_armed/token、g_mx68k_frame_num)。P108-own state 新設、既存
 * [P87]-[P107] / CP probes / P67 不改変。
 * ==================================================================== */
#define P108_ENABLE     0   /* P109/P110/P111 と排他: single-step を lean に走らせるため WriteW observer を off。コードは残置 (pure addition、P108_ENABLE 0 で byte-equivalent) */
#define P108_RING_DEPTH 512   /* 低 A7 WriteW ring 深さ (head+tail preserve、storm 降下を terminal まで保全) */
#define P108_A7_LOW_HI  0x001FF8u  /* 低 A7 gate 上限 (SSP top)。a7 != 0 && a7 <= この値 で記録 */

/* ====================================================================
 * P109-FLIP-DISCRIMINATOR: A7=$1FF8 域 -> $0 へ flip する機序を、frame-late +
 * bare 低 A7 gated single-step (P107 機構の self-contained clone) に「per-step
 * 降下 WriteW 発火カウント」を足して artifact なしで裁定する。
 *
 * 設計: P107 は flip-step opword を decode したが、c=1 step の chunk 境界で
 * CHECK_INT/例外 entry が A7 を変えるため opword latch が誤 anchor になりうる
 * (P107 flip opword=0x4E73=RTE と decode されたが RTE は A7+6 ゆえ $1FF6->$0 と
 * 物理矛盾 = AMBIGUOUS)。P109 は flip step 中の「降下 WriteW 発火」を数え、
 * write-presence (物理的に一義) を一次裁定者にする: 例外フレーム push は A[7]-=N
 * の後に Write_Word を出すため降下 write が WriteW callback で見える。movea/pop は
 * SSP に何も書かない (movea=値ロードのみ、RTE=read only)。
 *  → flip step 中に低帯域 write が発火 = EXCEPTION-PUSH-DRIVEN、
 *    発火なし + opword=movea = MOVEA-REGISTER-CONFIRMED、
 *    発火なし + opword=RTE で delta 不整合 = ANCHOR-MISMATCH (over-claim 防止)。
 *
 * ★self-contained clone: P107/P106/P108 の static/関数を一切参照しない (それらは
 * ENABLE=0 でコンパイル除外されるため参照=undeclared ビルド破壊、p106_decode_movea
 * 前例)。P109-own: s_p109_stepping / s_p109_done / s_p109_step_count /
 * s_p109_step_wdesc / s_p109_step_wdesc_band / p109_ring[512] / p109_ring_push /
 * s_p109_flip_* / p109_decode_movea / p109_emit_dump / p109_on_writew /
 * s_p109_frozen / s_p109_boot_gen、独自の arm / before-latch / after-latch /
 * WriteW hook / dump-trigger、全て #if P109_ENABLE。
 *
 * 全 observation-only=動作変更ゼロ (single-step は P106/P107 で非摂動実証済、
 * P67 byte-identical)。WriteW hook は single-step 中のみカウント (stepping 外は
 * 即 return)、stepping 自体は P107 と同条件・同手法。reuse は READ-ONLY
 * (C68k_Get_AReg/DReg、C68K.D|A 直読 memcpy、p105_read_opword [P105_ENABLE=1])。
 * 全コード P109_ENABLE で gating。P109_ENABLE 0 で byte-equivalent。
 * s_p109_boot_gen は reset-clear で increment-only bump (memset 対象外)。
 * ==================================================================== */
#define P109_ENABLE     0   /* P110/P111 と排他: single-step は同時 1 機構のみ active。P111 サイクルでは 0。コードは残置 (pure addition、P109_ENABLE 0 で byte-equivalent) */
#define P109_RING_DEPTH 512   /* per-step ring 深さ (head+tail preserve、storm 降下 onset 保全) */
#define P109_A7_LOW_HI  0x001FF8u  /* 低 A7 gate 上限 (SSP top)。a7 != 0 && a7 <= この値 で arm / 降下 write 判定 */
#define P109_FRAME_ARM  80    /* boot 前半除外: flip は panic frame≈91 近傍 (P107 同値) */
#define P109_STEP_CAP   P109_RING_DEPTH  /* single-step 安全弁 = ring 深さ (storm 降下を 1 window で覆う) */

/* ====================================================================
 * P110-USP-BANK-SWITCH: P109 の flip-instant single-step 機構を P110-own に
 * self-contained clone し、flip step (a7 $1FF6 -> $0) について SR/USP/SSP と
 * popped SR/PC を latch して「USP-bank-switch 機序」(RTE が S=0 の SR を pop し
 * active A7 = USP に切替わる) が実 boot で発火するかを empirical 裁定する。
 *
 * 機序 (c68k OP_0x4E73 で確定): RTE は SR pop -> flag_S 更新 -> PC pop ->
 *   if (!flag_S) { A[7] = USP; USP = old_A[7]; }。pop した SR が S=0 なら
 *   active A7 が USP に切替わり、USP=$0 なら A7=$0。
 *
 * ★全 symbol P110-own (P109/P108/P107/P106 を一切参照しない、ENABLE=0 で
 * compile out -> 参照=undeclared ビルド破壊、p106_decode_movea 前例)。opword
 * read も P110-own (p110_read_opword、p105_read_opword [P105_ENABLE gated] に
 * 依存しない)。reuse は READ-ONLY getter のみ (C68k_Get_AReg/USP/SR、C68K.D|A
 * 直読 memcpy、p47_read_long_le、s_ipl_fetch[] / MEM[] addr-based read)。
 * 全 observation-only=動作変更ゼロ (single-step は P106/P107/P109 で非摂動実証済、
 * P67 byte-identical)。全コード P110_ENABLE で gating、P110_ENABLE 0 で
 * byte-equivalent。s_p110_boot_gen は reset-clear で increment-only bump
 * (memset 対象外)。
 * ==================================================================== */
#define P110_ENABLE     0   /* P111 と排他: single-step は同時 1 機構のみ active。P111 サイクルでは 0。コードは残置 (pure addition、P110_ENABLE 0 で byte-equivalent) */
#define P110_RING_DEPTH 512   /* per-step ring 深さ (head+tail preserve、storm 降下 onset 保全) */
#define P110_A7_LOW_HI  0x001FF8u  /* 低 A7 gate 上限 (SSP top)。a7 != 0 && a7 <= この値 で arm / 降下 write 判定 */
#define P110_FRAME_ARM  80    /* boot 前半除外: flip は panic frame≈91 近傍 (P107/P109 同値) */
#define P110_STEP_CAP   P110_RING_DEPTH  /* single-step 安全弁 = ring 深さ */
#define P110_SR_S       0x2000u  /* SR の supervisor (S) ビット (= C68K_SR_S、P110-own 名で self-contained) */

/* ====================================================================
 * P111-FRAME-ORIGIN: P110 の flip-instant single-step ENVELOPE 全体 + P108 の
 * 低 A7 WriteW ring を P111-own に self-contained clone し、2 つの相補的観測で
 * 「boot program が user mode (S=0) に落ちる瞬間」を pin する observation-only probe。
 *
 * 目的A (writer-PC WriteW probe): SR=$0000 frame を $1FF6 に push した命令の PC、
 *   PC=$212c を $1FF8/$1FFA に push した命令の PC を捕捉。single-step entry の現命令
 *   PC (bare BasePC でない) を writer-PC とし、ROM ($FFxxxx) vs loaded-boot ($2xxx) で
 *   band 分類。低 A7 帯 [$1FF0,$2000) の WriteW を全件 latch (value-gate は TAG のみ)。
 *   was_single_stepping flag で stale entry_pc を UNRELIABLE 弁別。
 * 目的B (S-bit 1->0 遷移 watch): single-step entry で C68k_Get_SR の S ビット (bit13) を
 *   sample し 1->0 遷移点の causing 命令 (1 つ前 step) の PC/opword を latch。
 *   ★opword 分類: RTE-pop (0x4E73) = terminal flip = root-cause 候補から除外、
 *   privileged-write (move/andi/eori #,sr) = ORIGINATING upstream 原因として分離 latch。
 *
 * ★全 symbol P111-own (P110/P109/P108/P107/P106/p106_decode_movea/p105_read_opword を
 * 一切参照しない、ENABLE=0 で compile out -> 参照=undeclared/重複 ビルド破壊、前例多数)。
 * opword read も P111-own (p111_read_opword)。reuse は READ-ONLY getter のみ
 * (C68k_Get_AReg/USP/SR、C68K.D|A 直読 memcpy、p47_read_long_le、s_ipl_fetch[]/MEM[]
 * addr-based read)。全 observation-only=動作変更ゼロ (single-step は P106/P107/P109/P110
 * で非摂動実証済、P67 byte-identical)。全コード P111_ENABLE で gating、P111_ENABLE 0 で
 * byte-equivalent。s_p111_boot_gen は reset-clear で increment-only bump (memset 対象外)。
 * ==================================================================== */
#define P111_ENABLE     0   /* P112 と排他: P111 single-step gate と P112 per-access sample の共存で P67 attribution を汚さないため 0。コードは残置 (pure addition、P111_ENABLE 0 で byte-equivalent) */
#define P111_RING_DEPTH 512   /* per-step ring 深さ (head+tail preserve、storm 降下 onset 保全) */
#define P111_A7_LOW_HI  0x001FF8u  /* 低 A7 gate 上限 (SSP top)。a7 != 0 && a7 <= この値 で arm */
#define P111_FRAME_ARM  80    /* boot 前半除外: flip は panic frame≈91 近傍 (P107/P109/P110 同値) */
#define P111_STEP_CAP   P111_RING_DEPTH  /* single-step 安全弁 = ring 深さ */
#define P111_SR_S       0x2000u  /* SR の supervisor (S) ビット (= C68K_SR_S、P111-own 名で self-contained) */
#define P111_W_RING     512   /* 低 A7 WriteW writer-PC ring 深さ (head+tail preserve) */
#define P111_SCLEAR_RING 64   /* S-bit 1->0 遷移 latch ring 深さ */
#define P111_BAND_LO    0x001FF0u  /* writer-PC ring 採取 addr 帯 下限 (含む) */
#define P111_BAND_HI    0x002000u  /* writer-PC ring 採取 addr 帯 上限 (排他) */
#define P111_PC_HI_ADDR 0x001FF8u  /* PC longword HI half slot (= [$1FF8]) */
#define P111_PC_LO_ADDR 0x001FFAu  /* PC longword LO half slot (= [$1FFA]) */
#define P111_PC_TAG_VAL 0x0000212Cu /* pc_tag 期待 longword (recon($1FF8,$1FFA)) */
#define P111_SR_TAG_ADDR 0x001FF6u  /* sr_tag slot (SR push 先) */

/* ====================================================================
 * P112-SWATCH: boot 全体で S ビット (SR bit13) が最初に 1->0 になる瞬間
 * (CPU が最初に user mode に入る点) を非摂動 per-access で捕捉し、その
 * 機序 (RTE/例外 return か move/andi/eori #,SR か) を SR-source (stack-read
 * 有無) で判別する observation-only probe。
 *
 * 設計: single-step を使わない (lesson ㉓: chunk 拡大は IRQ cadence を変え
 * canonical path 消失)。ReadW/WriteW per-access callback 上で S ビットを軽量
 * sample (lesson ⑬: observation-only なら毎 access sample でも P67 byte-identical、
 * p97_on_access 先例)。1->0/0->1 遷移を whole-boot ring に記録し、最初の 1->0 を
 * 別 latch に one-shot 固定 (head ring が押し出されても保全)。
 *
 * 機序判別 (★A7 ジャンプは判別子に使えない=全機序共通 bank swap、corroboration のみ):
 *   直近 ReadW 履歴 ring を照合し「val16 の bit13=0 (S=0 SR) ∧ addr==read 時点の
 *   live_a7 (= SSP top で起きた read)」の stack-SR-read があれば RTE-LIKE、
 *   無ければ MOVE-TO-SR。
 *
 * ★全 symbol P112-own。p97_/p108_/p109_/p110_/p111_ を一切参照しない
 * (ENABLE 状態に関わらず undeclared/重複 ビルド破壊回避、p106_decode_movea 前例)。
 * reuse は READ-ONLY getter のみ (C68k_Get_SR/USP/MSP/AReg)。全 observation-only=
 * 動作変更ゼロ (per-access sample は p97_on_access で P67 byte-identical 実証済)。
 * 全コード P112_ENABLE で gating、P112_ENABLE 0 で byte-equivalent。
 * s_p112_boot_gen は reset-clear で increment-only bump (memset 対象外)。
 * ==================================================================== */
#define P112_ENABLE     0
#define P112_SR_S       0x2000u  /* SR の supervisor (S) ビット (bit13)、P112-own 定数 */
#define P112_TRANS_RING 64       /* S 遷移 ring 深さ (head+tail preserve、最早期 1->0 保全) */
#define P112_READW_HIST 4        /* 直近 ReadW 履歴 ring 深さ (SR-source 判別用) */
#define P112_A7_LOW_HI  0x001FF8u /* A7=$0 flip freeze 判定用 (a7_before!=0 && a7_after==0) */

/* ====================================================================
 * P113-FB (WriteW frame-build trace): boot 全体で低 A7 帯 [$1FF0,$2000) への
 * WriteW を非摂動 per-access で全件 latch し、S=0 frame の build が deliberate
 * launch (IPL の意図的 user-mode 起動、move.w #0,-(sp)+pea+rte) か corruption
 * (孤立 $0000 / misaligned SP) かを frame-shape で実測裁定する observation-only
 * probe。判別子は $0000 SR write が decrement 順連続 frame の一部で隣接 PC.l が
 * valid IPL-ROM PC か (deliberate) / 孤立・misaligned (corruption)。
 *
 * ★single-step 不使用 (P111 が同 gate で S-clear=0、chunk 境界例外は per-step で
 * bracket 不可、lesson ㉕)。per-access WriteW (frame-build 捕捉) + chunk 境界 read
 * (post-RTE landing PC) のみ — 両方 P67 byte-identical 実証済の非摂動手段。
 *
 * ★全 symbol P113-own。p97_/p108_-p112_ を一切参照しない (undeclared/重複ビルド
 * 破壊回避、p106_decode_movea 前例)。reuse は READ-ONLY getter のみ
 * (C68k_Get_SR/PC/USP/AReg、p47_read_long_le)。全 observation-only=動作変更ゼロ。
 * 全コード P113_ENABLE で gating、P113_ENABLE 0 で byte-equivalent。
 * s_p113_boot_gen は reset-clear で increment-only bump (memset 対象外)。
 * ==================================================================== */
#define P113_ENABLE     0
#define P113_SR_S       0x2000u  /* SR の supervisor (S) ビット (bit13)、P113-own 定数 */
#define P113_W_RING     64       /* 低 A7 帯 WriteW ring 深さ (head+tail preserve) */
#define P113_WIN_LO     0x001FF0u /* frame-build 観測帯 下限 (含む、RTE が pop する SSP frame 領域) */
#define P113_WIN_HI     0x002000u /* frame-build 観測帯 上限 (排他) */

/* ====================================================================
 * P114-NS (SSP-top named-slot trace): boot 全体で SR bit13 が最初に 1->0 に
 * なる瞬間 (RTE が S=0 SR を pop した瞬間) を非摂動 per-access SR sample で捕捉し、
 * pop された $0000 SR が (a) MALFORMED-WRITE (どこかが SR slot に $0000 を書いた)
 * か (b) ZERO-INIT-POP (一度も書かれず RAM zero-init の $0000 をそのまま pop) かを
 * truncate 不可能な named-slot + written-flag で affirmative に裁定する
 * observation-only probe。P113 の固定深 ring 切り捨て欠陥 (追跡 write を落とす) を
 * 追跡対象 2 アドレス ($001FF6 primary / $001FF8 secondary) に限定して解消。
 *
 * ★single-step 不使用。per-access SR sample (ReadW + WriteW 両 callback が同一
 * s_p114_sr_prev を共有し p114_sample_s を両方から呼ぶ — P112 の per-access 機構を
 * 正確に clone) で S 1->0 を検出、その瞬間に named-slot snapshot を freeze。
 * named-slot WriteW は per-access で追跡 (truncate なし)。両方 P67 byte-identical
 * 実証済の非摂動手段。
 *
 * ★全 symbol P114-own。p97_/p108_-p113_ を一切参照しない (undeclared/重複ビルド
 * 破壊回避、p106_decode_movea 前例)。reuse は READ-ONLY getter のみ
 * (C68k_Get_SR/PC/USP/AReg)。全 observation-only=動作変更ゼロ。
 * 全コード P114_ENABLE で gating、P114_ENABLE 0 で byte-equivalent。
 * s_p114_boot_gen は reset-clear で increment-only bump (memset 対象外)。
 * ==================================================================== */
#define P114_ENABLE     0
#define P114_SR_S       0x2000u  /* SR の supervisor (S) ビット (bit13)、P114-own 定数 */
#define P114_SLOT0_ADDR 0x001FF6u /* named-slot idx0 (RTE pop SR primary, c68k OP_0x4E73) */
#define P114_SLOT1_ADDR 0x001FF8u /* named-slot idx1 (secondary, a7 ambiguity 保険) */

/* ====================================================================
 * P119-A7W (a7-watch localize probe): boot 全体で SSP 帯 ($1FF6/$1FF8) への
 * memory access を非摂動 access-keyed で観測し、2-byte SSP offset がどこで・
 * 割込配送と相関して出るか、frame-push 回数 vs RTE-pop 回数の count 非対称が
 * あるかを実測する observation-only probe。P118 plan §5.1 の確定仕様。
 *
 * 統一仮説 (配送タイミング依存の c68k 例外 frame 非対称) と (ii) 多重/nested
 * 配送 (count 非対称) を判別する。第一判定軸 = frame-push 回数 vs RTE-pop 回数の
 * count 照合 (MINOR-1)。
 *
 * ★keying は memory-access ADDRESS 基準 (MAJOR-2 / P118 §5.3) — runtime 命令 PC
 * 捕捉は不能 (P115/教訓㊸) ゆえ PC==$FF1E3E のような runtime PC 比較 latch は
 * 実装しない。代わりに既知 SSP アドレスへの access を hook し、その access addr
 * = そのときの a7 を latch する access-keyed 方式に徹する。
 *
 * 3 観測点 (すべて access addr 基準、すべて既存 trace_Memory_ReadW/WriteW callback
 * 経由 — c68k WRITE_LONG_F は Write_Word x2 に分解されるため例外 frame の PC long も
 * WriteW 2 件として届く、新規 plumbing なし):
 *   (1) SSP slot ($1FF6/$1FF8) への READ access を hook (ReadW) — dispatcher の
 *       movem.l (a7)+,a0 pop と RTE の frame pop が読む SSP slot read。addr=$1FF6 なら
 *       balanced / $1FF8 なら offset。RTE-pop 回数を count、offset 出現 frame/seq を latch。
 *   (2) SSP slot ($1FF6=SR/$1FF8=PC) への WRITE access を hook (WriteW) — TRAP#15 entry の
 *       frame-push write addr を latch → frame 起点 a7=$1FF6 を access ベースで確認。
 *   (3) 例外 frame-push を WRITE access で観測 (WriteW) — SSP 観測帯 [$1FF0,$2000) への
 *       decrement-順 write を frame-push とみなし count、その frame/seq/a7 を latch。
 *       割込配送 (CHECK_INT) push と TRAP entry push の両方を捕捉する。
 *
 * ★全 symbol P119-own。p97_/p108_-p117_ を一切参照しない (undeclared/重複ビルド
 * 破壊回避、p106_decode_movea 前例)。reuse は READ-ONLY getter のみ
 * (C68k_Get_SR/PC/USP/AReg)。全 observation-only=動作変更ゼロ。CPU/guest メモリ
 * 状態を一切書かない。cycle/timing/chunk size/timer feed を一切変えない。
 * 全コード P119_A7_WATCH で gating、flag=0 で byte-equivalent (P67 byte-identical)。
 * s_p119_boot_gen は reset-clear で increment-only bump (memset 対象外)。
 * ==================================================================== */
#ifndef P119_A7_WATCH
#define P119_A7_WATCH   0   /* 0 = control (no-op、P67 byte-identical), 1 = a7-watch 観測有効 */
#endif
#define P119_SR_S       0x2000u  /* SR の supervisor (S) ビット (bit13)、P119-own 定数 */
#define P119_SLOT0_ADDR 0x001FF6u /* SSP slot idx0 (balanced pop = SR slot、$1FF6) */
#define P119_SLOT1_ADDR 0x001FF8u /* SSP slot idx1 (offset pop = PC-hi slot、$1FF8) */
#define P119_WIN_LO     0x001FE0u /* 例外 frame-push 観測帯 下限 (含む、SSP frame 領域) */
#define P119_WIN_HI     0x002000u /* 例外 frame-push 観測帯 上限 (排他) */
#define P119_PUSH_RING  48        /* frame-push event ring 深さ (head+tail preserve) */
#define P119_POP_RING   48        /* RTE-pop (SSP slot read) event ring 深さ */

/* P120-FM: a7-watch refine — 第一判定軸 (exc-frame push 数 vs RTE-pop 数) の like-for-like
 * 測定。6-byte group-2 例外 frame の 3-word シーケンス (降順 push / 昇順 pop、run_len==3
 * ちょうど、a7 ∓6) だけを count する。P119 とは独立した別ブロック (p119_ 状態は不参照)。
 * flag=0 で全ブロックがプリプロセッサ除去 → P67 byte-identical (P119 と同じ機構)。 */
#ifndef P120_FRAME_MATCH
#define P120_FRAME_MATCH       0   /* 0 = control (no-op, P67 byte-identical), 1 = frame-match 観測有効 */
#endif
#define P120_WIN_LO            0x001FE0u  /* in-band 下限 (含む、SSP frame 帯、nested 余裕で P119 と同帯) */
#define P120_WIN_HI            0x002000u  /* in-band 上限 (排他) */
#define P120_SLOT_SR_OK        0x001FF6u  /* balanced SR slot (正常 RTE の SR pop addr) */
#define P120_SLOT_SR_OFFSET    0x001FF8u  /* offset slot (PC-hi word を SR として誤読する addr) */
#define P120_CAUSAL_FRAME_GATE 80u        /* init noise 除外 gate (TRAP#15 entry frame≈88 より前で十分小) */
#define P120_PUSH_RING         32         /* exc-push event ring 深さ */
#define P120_POP_RING          32         /* RTE-pop event ring 深さ */

/* ====================================================================
 * P122: S-clear (S 1->0) supply-source discriminator probe (observation-only)。
 * panic 直前に bad SR=$0000(S=0) を生む S-bit clear の供給源が、RTE 系 (stack pop で
 * 新 SR を取る) か 非 RTE-SR-write 系 (MOVE/ANDI/EORI/ORI-to-SR が immediate/EA から取る)
 * かを、stack pop 署名の有無 (3-word 昇順 run + run 内 live S 1->0 straddle-S) で中立判別する。
 *
 * 判別主軸 = 観測点1 (p122_on_readw): 毎 access で C68k_Get_SR の live S を sample し
 * S 1->0 flip した access を S-FLIP として per-access に pin (chunk-size 非依存)。その access が
 * straddle-S 昇順 run の read#2 位置なら [A] RTE 系、stack-read run を伴わなければ [B] 非 RTE。
 * 観測点3 (chunk sample) は USP=$0 実証 / landing PC の確証補助に降格 (judge anchor ではない)。
 *
 * 全 symbol P122-own。reuse は READ-ONLY getter (C68k_Get_SR/PC/AReg/USP) + host-side
 * ring/latch のみ。CPU/guest メモリ書込ゼロ・cycle/timing/chunk size/timer feed 一切不変。
 * flag=0 で完全 no-op (P67 byte-identical)。P119/P120 は変更しない (別 #if guard で並走)。
 * ==================================================================== */
#ifndef P122_SRCLEAR_PROBE
#define P122_SRCLEAR_PROBE     0   /* 0 = control (no-op, P67 byte-identical), 1 = S-clear 判別 観測有効 */
#endif
#define P122_STACK_LO          0x00001F00u  /* band "STK" 主帯 下限 (含む) */
#define P122_STACK_HI          0x00002000u  /* band "STK" 主帯 上限 (排他) */
#define P122_USP_LO            0x00000000u  /* band "USP" 下限 (含む、MINOR-3: vector table と重複、straddle-S で除外) */
#define P122_USP_HI            0x00000100u  /* band "USP" 上限 (排他) */
#define P122_RING              32           /* pop/push event ring 深さ */
#define P122_CAUSAL_FRAME_GATE 64u          /* MINOR-4: pop/push ring 格納条件のみに適用 (sflip/confirm latch には掛けない) */

/* ====================================================================
 * P124: `move.w (a7)+,sr` bad SR(S=0) pop の dual-endpoint pop/push 捕捉 probe。
 * pop {a7_at_pop, popped word} と push {a7_at_push, pushed SR} の両端点 + 区間
 * in-band WriteW を frame90 窓で raw 捕捉し、a7-keyed LIFO ペアリングで
 *   (a) 上流 push が既に S=0 / (b) a7 オフセットで非 SR 語 pop / (clobber) 区間 write が slot 上書き
 * を Orchestrator が事後算術判定できる data を残す純観測 probe。VERDICT は断定しない。
 * 全 symbol P124-own (s_p124_* / p124_*)、READ-ONLY getter のみ再利用、P122 並走不変。
 * flag=0 で完全 no-op (P67 byte-identical)。
 * ==================================================================== */
#ifndef P124_SRPAIR_PROBE
#define P124_SRPAIR_PROBE      0   /* 0 = control (no-op, P67 byte-identical), 1 = pop/push 両端点観測有効 */
#endif
#define P124_STACK_LO          0x00001F00u  /* STK band 下限 (含む)。注目 slot $1FF6/$1FF8 は STK 内 */
#define P124_STACK_HI          0x00002000u  /* STK band 上限 (排他) */
#define P124_RING              32           /* pop/push/write event ring 深さ (head+tail で frame90 窓保全) */
#define P124_FRAME_GATE        64u          /* ring 格納条件のみに適用 (sample/latch には掛けない) */

/* ====================================================================
 * P126: exception-push 経路 instrument probe (observation-only)。
 * frame90 窓 (89..91) の c68k 全例外 entry を vector fetch (ReadW callback の
 * addr&3==0 && addr<0x400) で時系列 log し、(i) 最初に pushed-SR が S=0 で entry
 * した例外 (ii) 最初の期待外 vec# 例外 (iii) handler PC が $0/garbage の例外を
 * raw に surface して trigger 例外を pin する。push-signature (直前 3-word 降順
 * SSP push) を WriteW tap で検出し entry_confirmed として真の vector fetch を確認。
 * a7-validity gate + sentinel filter (CP-R-1 移植) で garbage 由来の S=0 誤 latch
 * を防止。ring-append は全件無条件 (drop gate なし)、latch 段のみ
 * entry_confirmed && a7_valid && !sentinel_hit で gate。VERDICT は断定しない。
 * 全 symbol P126-own (s_p126_* / p126_*)、READ-ONLY getter のみ再利用
 * (C68k_Get_SR/PC/AReg/USP/MSP/DReg, p47_read_long_le)。P122/P124 並走不変。
 * flag=0 で完全 no-op (空 inline スタブ、seq guard 含む P67 byte-identical)。
 * ==================================================================== */
#ifndef P126_ENABLE
#define P126_ENABLE            0   /* 0 = control (no-op, P67 byte-identical), 1 = exc-push instrument 観測有効 */
#endif
#define P126_RING              256          /* 全件記録方針 (drop gate なし) ゆえ余裕を持たせる ring 深さ */

/* ====================================================================
 * P128: $934/$938 mouse-callback ポインタ runtime 検証 probe (observation-only)。
 * SCC マウスハンドラ $FF181C が dispatch する guest longword ポインタ
 * $00000934 (期待 $00FF4546) / $00000938 (期待 $00FFA2DE) が MX68K boot で
 * 正しく init され panic 直前までその値を保つかを検証。WriteW tap で
 * $934/$936/$938/$93A への word write を全件記録 + panic-terminus で live
 * longword を read-only に再構成。flag=0 で完全 no-op (空 inline スタブ、
 * P67 byte-identical)。観測のみ・guest 書込ゼロ・timing 不変。
 * ==================================================================== */
#ifndef P128_ENABLE
#define P128_ENABLE            0   /* 0 = control (no-op, P67 byte-identical), 1 = callback-ptr 監視 */
#endif
#define P128_RING              64          /* $934/$938 への write は稀ゆえ 64 で十分 */

/* ====================================================================
 * P130: 11 個の move.w (a7)+,sr ($46DF) サイトのうち frame90 窓で S を 1->0 に
 * 落とす pop を直接捕捉する観測 probe (observation-only)。pop の stack 読みは
 * ReadW POST-value callback に到達し、読まれた語が新 SR そのもの。callback 時点で
 * SET_SR 前ゆえ S-before が読め、popped_word の bit13 が S-after 候補。S-clear pop
 * = (s_before==1 and s_after_candidate==0)。さらに次の in-band access で S が実際に
 * 0 へ flip したかを s_confirmed_after に記録し、通常 data 読みと真の SR-pop を
 * 三角測量で識別 (P124 はここで失敗)。site 解決と Case A/B/C 分類は捕捉 a7/
 * popped_word から Orchestrator が OFFLINE で実施。VERDICT は断定しない。
 * 全 symbol P130-own、READ-ONLY getter のみ再利用、P122/P124/P126/P128 並走不変。
 * flag=0 で完全 no-op (空 inline スタブ、P67 byte-identical)。guest 書込ゼロ。
 * ==================================================================== */
#ifndef P130_ENABLE
#define P130_ENABLE            0   /* 0 = control (no-op, P67 byte-identical), 1 = S-clear pop 捕捉有効 */
#endif
#define P130_RING              128         /* pop(ReadW) 全件記録 ring 深さ (power-of-two、& (RING-1) mask) */
#define P130_WRING             256         /* push(WriteW) 捕捉 ring 深さ — matching push は窓前方に起きうるので深め */
#define P130_SR_MASK           0x271Fu     /* C68k_Get_SR が T(15) を合成しない分を除く SR マスク (P124 :12213 と同値) */

/* ====================================================================
 * P132: $1FF0..$1FFA (top-of-stack frame region) 専用 write-ring。frame90 で
 * a7=$1FF6 (実 SR slot) から 0x0000 を SR にロードし S を 1->0 に落とすことが
 * P131 で確定。本 probe は「誰が $1FF6 を 0x0000 にしたか (書込側)」を read-PC
 * 不明と独立に追う。狭帯域 (6 byte) ゆえ ring 128 + overflow flag で全 boot を
 * 非 evict 記録。WriteW (word/long) と WriteB (move.b) の両 tap を捕捉し、
 * $1FF6 SR-slot は ring overflow に依らない専用 latch で要点を保持。分岐 A/B/C の
 * 判定は OFFLINE (probe は raw + ヒントのみ・断定しない)。全 symbol P132-own、
 * READ-ONLY getter のみ、guest 書込ゼロ。flag=0 で完全 no-op (P67 byte-identical)。
 * ==================================================================== */
#ifndef P132_ENABLE
#define P132_ENABLE            0   /* 0 = control (no-op, P67 byte-identical), 1 = $1FF6 書込側捕捉有効 */
#endif
#define P132_RING              128         /* frame-region write-ring 深さ */
#define P132_LO                0x1FF0u     /* 帯域下限 (inclusive) */
#define P132_HI                0x1FFAu     /* 帯域上限 (inclusive、PC-low slot $1FFA を含む) */

/* ====================================================================
 * P133: H1/H2 discriminator。P132 で「$1FF6 は全 boot で word-0x0000 を持った
 * ことがない」が確定 (非evict latch=権威)。だが P131 は $1FF6 から 0x0000 を
 * 読み SR→0x0000。本 probe は H1 [S-clear は直接 SR 書込命令で stack-pop 誤帰属]
 * か H2 [byte 書込で $1FF6/$1FF7 をゼロ化 (word-latch 盲点)] を経験的に決着する。
 * 主判別 = $1FF6/$1FF7 への byte-zero 書込を非evict latch で捕捉 (H2 直接検証)。
 * 補強 = confirmed-S-clear の arm-time スナップショット (実 a7 で live mem 読取・
 * D0-D7・SR-before)。a7!=0x1FF6 なら H3 (wrong-address) も識別。判定は OFFLINE
 * (probe は raw + ヒントのみ・断定しない)。全 symbol P133-own、READ-ONLY getter +
 * p47_read_long_le のみ、guest 書込ゼロ。flag=0 で完全 no-op (P67 byte-identical)。
 * ==================================================================== */
#ifndef P133_ENABLE
#define P133_ENABLE            0   /* 0 = control (no-op, P67 byte-identical), 1 = H1/H2 discriminator 有効 */
#endif

/* ====================================================================
 * P134: $1FF6 zeroing source — per-frame $1FF6 poll + DMAC MAR/BAR 直読.
 * P133 で確定: S-clear は $1FF6 から 0x0000 を pop する genuine stack-pop。
 * CPU word/byte 書込フックは $1FF6 ゼロ化を一切観測せず。★核心: DMA データ転送
 * (DMA_Exec → dma_writemem24 → wm_main/wm16_main) は RAM 配列直書込で
 * trace_Memory_WriteW/WriteB を迂回する。有力候補は FDD セクタ読込 DMA の MAR が
 * $1FF6 近傍を指し SR slot を 0x0000 上書き。P134 は (1) $1FF6 ゼロ化 frame を
 * per-frame poll で bracket し (2) 各 ch の DMA[ch].MAR/.BAR 直読でスタック帯を
 * 指す ch を検出 (frame poll) + (3) CCR-start (STR=1) 時の programmed MAR を
 * frame 粒度非依存で latch する。観測専用・read-only・flag=0 で byte-identical。
 * ==================================================================== */
#ifndef P134_ENABLE
#define P134_ENABLE            0   /* 0 = control (no-op, P67 byte-identical), 1 = $1FF6 ゼロ化源 poll 有効 */
#endif
#ifndef P134_FRAMES
#define P134_FRAMES            96  /* per-frame 配列 (boot 全域) */
#endif
#ifndef P134_STK_LO
#define P134_STK_LO            0x1F00u  /* スタック帯 lo ($1FF6 近傍・offline 再評価可) */
#endif
#ifndef P134_STK_HI
#define P134_STK_HI            0x2000u  /* スタック帯 hi (exclusive) */
#endif
#ifndef P134_STARTRING
#define P134_STARTRING         64  /* CCR-start latch ring 容量 */
#endif

/* ====================================================================
 * P135: $1FF6=0x0000 起源の 3-way 弁別プローブ (第3クラス / H-W / H-R)。
 * 観測のみ・既存ロジック不変・Bridge のみ。
 *   Part 1 (EmulatorBridge.c): host-path runtime latch (init clear / IPL shadow が
 *     boot 後の frame で走るか動的確認)。
 *   Part 2 (m68000_bridge.c): confirmed S-clear pop の実 addr / 返値 / 同一物理セル値捕捉。
 *   Part 2b: pop 直後 access で RTE 署名 (PC-pop) を同定。
 *   Part 3: frame 86-91 窓の $1FF4..$1FF8 write-hook 通過 audit。
 * ==================================================================== */
#ifndef P135_ENABLE
#define P135_ENABLE            0   /* 0 = control (no-op, P67 byte-identical), 1 = 3-way 弁別有効 */
#endif
#ifndef P135_WIN_LO
#define P135_WIN_LO            86  /* Part3 write-window 下限 (inclusive) */
#endif
#ifndef P135_WIN_HI
#define P135_WIN_HI            91  /* Part3 write-window 上限 (inclusive) */
#endif
#ifndef P135_WADDR_LO
#define P135_WADDR_LO          0x1FF4u  /* Part3 監視 addr 下限 (inclusive) */
#endif
#ifndef P135_WADDR_HI
#define P135_WADDR_HI          0x1FF8u  /* Part3 監視 addr 上限 (exclusive) */
#endif
#ifndef P135_WRING
#define P135_WRING             16  /* Part3 write-audit ring 容量 (非evict) */
#endif
#ifndef P135_HOSTLAT
#define P135_HOSTLAT           8   /* Part1 host-path latch 容量 (非evict) */
#endif
#if P135_ENABLE
/* Part1 host-path runtime latch のレコード型 (EmulatorBridge.c 定義・
 *   m68000_bridge.c の p135_dump から extern 参照)。両 TU で layout 一致のため
 *   typedef をヘッダに置く。which_path: 0=A(init clear), 1=B-memset(vec-skip 0 clear),
 *   2=B-shadow(IPL→MEM byte-swap loop)。 */
typedef struct {
    unsigned char  set;        /* このスロットが書込済みか */
    unsigned char  which_path; /* 0/1/2 */
    int            frame;      /* g_mx68k_frame_num at execution */
    unsigned short mem_before; /* MEM[0x1FF6] LE16 (path 実行直前) */
    unsigned short mem_after;  /* MEM[0x1FF6] LE16 (path 実行直後) */
} p135_hostlat_t;
#endif

/* ====================================================================
 * P136: frame89 ゼロ化の実行コンテキスト特定 (step-granularity transition bracket)。
 * frame [88,89] で MEM[0x1FF6] を per-line / per-frame の各実行ステップ直後に
 * 1 read で poll し、0x2000→0x0000 遷移が起きた step (= 直前ステップが犯人) を
 * 単一 latch する。観測のみ・c=1 強制なし=timing 中立。p136_poll は
 * m68000_bridge.c に定義 (latch statics も同 TU)・EmulatorBridge.c は
 * 下記 extern 宣言で呼ぶのみ。
 * ==================================================================== */
#ifndef P136_ENABLE
#define P136_ENABLE            0   /* 0 = control (no-op, P67 byte-identical), 1 = step-bracket 有効 */
#endif
#if P136_ENABLE
/* step-id 列挙 — poll 呼出順と一致 (offline 可読性のため固定順)。
 *   PRE_CPU=前 line 末〜この line CPU 実行前、CPU_EXEC=CPU 実行区間、以降は
 *   per-line の各 host サブステップ直後、LINE_TAIL=line 末、FDD_SETFDINT/
 *   FRAME_TAIL=line loop 外 (per-frame)。 */
enum {
    P136_PRE_CPU = 0,
    P136_CPU_EXEC,
    P136_MFP_TIMER,
    P136_RTC_TIMER,
    P136_MFP_TIMERA,
    P136_DMA0,
    P136_DMA1,
    P136_DMA2,
    P136_OPM_ADPCM,
    P136_KEYBOARD_INT,
    P136_SCC_INTCHECK,
    P136_LINE_TAIL,
    P136_FDD_SETFDINT,
    P136_FRAME_TAIL,
    P136_STEP_COUNT
};
/* m68000_bridge.c 定義の poll 関数 (external linkage)。EmulatorBridge.c から呼ぶ。
 *   step_id = enum 上記、line = H-line index (per-frame poll は sentinel -1)。 */
void p136_poll(unsigned char step_id, int line);
#endif

#ifndef P142D_SRAM_INIT
#define P142D_SRAM_INIT 1
#endif

/* ====================================================================
 * P117: frame-loop cadence A/B diagnostic — executed-vs-requested cycle feed.
 * MFP_Timer/RTC_Timer に供給する cycle 数を、現行の requested cycle (sc) から
 * 実行 cycle (ex = m68000_execute の戻り値) に切替えた時、frame≈90 の S=0 pop
 * (panic 経路) が消えるか + must-stay-green 7/7 が維持されるかを A/B で観測する。
 * MPX68K (A> boot 成功 reference) は usedclk = 実行 cycle を timer に供給するが、
 * MX68K は raw sc = requested cycle を供給する divergence の確認。
 *
 * ★default OFF (0) = control = 現行と byte 等価 (sc を供給)。
 * flag=1 = experiment (ex を供給) — 実験は -DP117_EXEC_CYCLE_FEED=1 build で観測。
 * 一変数原則: MFP_Timer/RTC への cycle feed (sc to ex) のみ。ICount-carry /
 * do-while 構造復元 / DMA_Exec 粒度 / OPM・ADPCM feed は触らない (P118 以降)。
 * 全コード P117_EXEC_CYCLE_FEED で gating、flag=0 で byte-equivalent (pure addition)。
 * ==================================================================== */
#ifndef P117_EXEC_CYCLE_FEED
#define P117_EXEC_CYCLE_FEED 0   /* 0 = control (feed sc; 7/7 baseline, byte-identical), 1 = experiment (feed ex) */
#endif

#define P82XB_WIN_LO     0x001FB0u   /* C2 MEM[] スナップショット窓 下限 */
#define P82XB_WIN_HI     0x001FF0u   /* C2 MEM[] スナップショット窓 上限(排他)*/
#define P82XB_FAULT_WORD 0x001FCCu   /* faulting word ベースアドレス */
#define P82XB_C1_LO      0x001FC0u   /* C1b コンテキスト窓 下限 */
#define P82XB_C1_HI      0x001FE0u   /* C1b コンテキスト窓 上限 (排他) */
#define P82XB_FW_RING    16          /* C1a faulting-word 専用リング段数 */
#define P82XB_WR_RING    96          /* C1b コンテキストリング段数 */
#define P82XB_FAULT_LO   0x001F00u   /* C2 トリガ: fault_pc 近傍下限 */
#define P82XB_FAULT_HI   0x002000u   /* C2 トリガ: fault_pc 近傍上限 */

/* P57-A session-end summary (called from mx68k_shutdown and mx68k_atexit_summary;
 * P52 非依存 — Code Major-4 反映). Emits [P57A-SUMMARY] + [P57A-SUMMARY-DECISION]. */
void m68000_p57a_dump_summary(void);

/* P56 helper — snapshot taker (called from mx68k_run_frame each frame; idempotent).
 * Implemented in m68000_bridge.c to keep snapshot table file-scope static.
 * /tmp/mx68k_P56_plan.md Edit F. */
void m68000_p56_take_snapshot(int frame_num);

/* ====================================================================
 * P214 診断 probe — 描画欠陥の class 判別のための測定のみ（修正なし）。read-only:
 * 描画結果を 1 画素も変えず、エミュレーション状態にも触れない。
 *   R1 (EmulatorBridge.c) = 表示行ごとの映像状態ハッシュ差分 = class 1 の主測定
 *   R2 (m68000_bridge.c)  = CPU 映像レジスタ書込の分類 = 帰属の裏取り
 *   S  (EmulatorBridge.c) = 静的署名 / K = キー make/break 先頭 60 件
 * 出力は 60 フレームに 1 行（20 秒走行で ~20 行）+ K 最大 60 行。
 * p214_r2_format_and_reset は m68000_bridge.c に定義 (累積 static も同 TU)・
 * EmulatorBridge.c は下記宣言で呼ぶのみ。
 *
 * ★休止中 (dormant)。既測定分の結果は .mx68k_cycles/P214_measured_*.md と
 * 同 P214_conclusion.md にある。残りのゲームを計測するときは下の
 * P214_ENABLE を 1 に戻すだけでよい（コードは全て残してある）。
 * 既定を 0 にしてあるのは R1 が表示行ごとに ~1KB をハッシュするためで、
 * 既知の性能課題（MPX68K に無い処理落ち）を悪化させないため。
 * ==================================================================== */
#ifndef P214_ENABLE
#define P214_ENABLE            1   /* P277: D-20実測のため一時的に再有効化(0=probe休止, 1=計測有効)。
                                      ★P515: D-39ヌル実験のため休止。超過フレーム窓でのログ密度急増
                                      (52-86行/フレーム)の主要因 P335/P338/P367/P350/P315 を含む
                                      23件のカスケードを道連れ休止し、Max frame time を再計測する。
                                      D-20 は P284 で解決済み。
                                      ★★P533: D-40 hands-on計測run専用に一時的に再有効化(このカスケード
                                      配下の P533_ENABLE を有効化するため)。計測完了後は0へ戻すこと
                                      ——D-39(ログストーム、P529で解決)の再発を避けるため常用しない。 */
#endif
#if P214_ENABLE
void p214_r2_format_and_reset(char *buf, int len);   /* len は int — bridging header に <stddef.h> を持ち込まない */
#endif

/* ====================================================================
 * P290 診断 probe — D-4(グラディウスII 遅延+点滅)調査。フレーム単位で
 * Sprite_Regs の変化有無・アクティブスプライト数を毎フレーム記録する
 * (読み取りのみ・エミュレーション状態は一切変更しない)。
 * P214_ENABLE に完全依存(Sprite_Regsハッシュの計算元がP214 R1のため)。
 * 使用後(本サイクル完了後)は 0 に戻す — P214/P218/P220と同じ運用。
 * ==================================================================== */
#if P214_ENABLE
#ifndef P290_ENABLE
#define P290_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P290_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

/* ====================================================================
 * P292 診断 probe — D-4(グラディウスII 遅延+点滅)調査。既存のP214-R2
 * (BGR領域書込みの表示期間中カウント)を毎フレーム出力に密度化する
 * (読み取りのみ・新たな計測ロジックは追加せず、既存r2bufの文字列パースのみ)。
 * P214_ENABLE に完全依存。使用後(本サイクル完了後)は 0 に戻す。
 * ==================================================================== */
#if P214_ENABLE
#ifndef P292_ENABLE
#define P292_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P292_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

/* ====================================================================
 * P293 診断 probe — D-4(グラディウスII 遅延+点滅)調査。表示期間中に
 * Sprite_Regs(0xEB0000-0xEB03FF、Core Sprite_DrawLineMcrが実際に参照する
 * 128エントリ×8バイトの範囲)へ書き込まれたスロット番号の範囲(最小/最大)と
 * 件数を毎フレーム記録する(読み取りのみ・エミュレーション状態は一切
 * 変更しない)。P214_ENABLE に完全依存。使用後は 0 に戻す。
 * ==================================================================== */
#if P214_ENABLE
#ifndef P293_ENABLE
#define P293_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P293_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif
#if P293_ENABLE
void p293_peek_and_reset(int *out_min_slot, int *out_max_slot, unsigned *out_n);
#endif

/* ====================================================================
 * P299 診断 probe — D-4(グラディウスII 遅延+点滅)調査。Sprite_Regs
 * (0xEB0000-0xEB03FF)書換えバースト発生時、書込みが起きた瞬間の生ラスタ行
 * (x68k_vline)の最小/最大を毎フレーム記録し、可視表示域(CRTC_VSTART〜
 * CRTC_VEND)内/外の件数を分けて数える(読み取りのみ・エミュレーション
 * 状態は一切変更しない)。P214_ENABLE に完全依存。使用後は 0 に戻す。
 * ==================================================================== */
#if P214_ENABLE
#ifndef P299_ENABLE
#define P299_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P299_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif
#if P299_ENABLE
void p299_peek_and_reset(int *out_vl_min, int *out_vl_max,
                          unsigned *out_n_in_disp, unsigned *out_n_out_disp);
#endif

#if P214_ENABLE
#ifndef P300_ENABLE
#define P300_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P300_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif
#if P300_ENABLE
void p300_peek_and_reset(uint16_t *out_before_posx, uint16_t *out_before_posy,
                          int *out_have_before);
#endif

#if P214_ENABLE
#ifndef P315_ENABLE
#define P315_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P315_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P318_ENABLE
#define P318_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P318_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P321_ENABLE
#define P321_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P321_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P335_ENABLE
#define P335_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P335_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P337_ENABLE
#define P337_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P337_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P338_ENABLE
#define P338_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif
#else
#define P338_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P350_ENABLE
#define P350_ENABLE            1   /* OverTake調査: BGスクロールレジスタ書込み+消費ペアトレース用、一時的に有効化 */
#endif
#else
#define P350_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P351_ENABLE
#define P351_ENABLE            1   /* OverTake調査: BG0行別代表色スキャン用、一時的に有効化 */
#endif
#else
#define P351_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P367_ENABLE
#define P367_ENABLE            1   /* OverTake調査: $EB0000-0811書込み経路(CPU/DMA)トレース用、一時的に有効化 */
#endif
#else
#define P367_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P378_ENABLE
#define P378_ENABLE            1   /* OverTake調査: ラスタ割込みACK実行トレース+ゲート値ウォッチ用、一時的に有効化 */
#endif
#else
#define P378_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P384_ENABLE
#define P384_ENABLE            1   /* OverTake調査: ラスタ割込み要求↔応答対応トレース用、一時的に有効化 */
#endif
#else
#define P384_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P391_ENABLE
#define P391_ENABLE            1   /* OverTake調査: 空の黒帯・合成経路診断用、一時的に有効化 */
#endif
#else
#define P391_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P404_ENABLE
#define P404_ENABLE            1   /* D-23調査: MFP Timer C分母プローブ用、一時的に有効化 */
#endif
#else
#define P404_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

/* P169: テキストVRAM word 書込みミラー([P169-TVWW])のゲート。従来はマクロ
 * ゲート自体が無く常時実行だった (P403 で新規導入)。一時的な観測用プローブで
 * あり恒久的な構造不変条件の監視ではないため、P378/P384/P391 と同じ多数派
 * パターン(P214_ENABLE 配下)に連動させる — 将来 D-4 調査を凍結する際に他の
 * 一時プローブと一緒に自然に休止されるほうが一貫性がある。
 * ★本サイクル(P403)では既定値 1 のまま = 挙動変更なし。ゲート化のみ。 */
#if P214_ENABLE
#ifndef P169_ENABLE
#define P169_ENABLE            1   /* 0=probe休止(pre-P169とバイト等価), 1=計測有効 */
#endif
#else
#define P169_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

/* P385: 走査線チャンク不変条件(n==0 が発生しない)の常設監視。
 * これは一時的な現象観測ではなく構造的不変条件のトリップワイヤなので、
 * 他の多くのプローブと違い P214_ENABLE(D-4表示診断)には連動させない —
 * D-4 凍結時にこの監視まで一緒に消えるのを避けるため。 */
#ifndef P385_ENABLE
#define P385_ENABLE            1
#endif

/* P657: フロントポーチ実行契機のコンパイル時ゲート。
 * ★1 = 通常(Core パッチ適用済が前提)。0 にしてよいのは Core パッチ未適用状態で
 *   「Bridge 側改修だけでは症状が動かない」ことを実証する検証手順(テスト計画 Step B)
 *   のときのみ。Core パッチ適用済で 0 にするとラスタコピーが一切実行されなくなる。
 *   コミットされる値は必ず 1 でなければならない。 */
#ifndef P657_FRONTPORCH_ENABLE
#define P657_FRONTPORCH_ENABLE 1
#endif

/* P657: [P657-RCFIRE] 常設カウンタのゲート。0 でブロックごと消滅し byte-equivalent。 */
#ifndef P657_PROBE_ENABLE
#define P657_PROBE_ENABLE      1
#endif

#if P657_PROBE_ENABLE
/* P657 測定2: GPIP 差し替えフックのヒット件数と、その分母(MFP窓への全読み取り数)。
 * 定義は Bridge/m68000_bridge.c(別翻訳単位)側。ログ出力のみの計装でも
 * 同一TU内へ定義を足すとメモリレイアウトが動きうるため、
 * memory `feedback_instrumentation_layout_adjacency_corruption.md` の指針に従い
 * 「定義は別TU + extern」で参照する。 */
extern unsigned long long g_p657_gpip_hits;
extern unsigned long long g_p657_mfp_reads;
#endif

/* P670: [P670-RCDROP] 累積カウンタのゲート。0 でブロックごと消滅し byte-equivalent。 */
#ifndef P670_PROBE_ENABLE
#define P670_PROBE_ENABLE      1
#endif

#if P670_PROBE_ENABLE
/* P670 (D-69): ラスタコピー「取りこぼし」仮説の直接観測用の累積カウンタ。
 * rc_exec_cum = 実際に転送が起きた回数(分子)、r22_writes_cum = ゲストが R22 に
 * src/dst 対を発行した回数(分母)。どちらもアプリ起動から一度もリセットしない。
 * 定義は Bridge/m68000_bridge.c(別翻訳単位)側 — g_p657_* と同じ理由
 * (memory `feedback_instrumentation_layout_adjacency_corruption.md`)。 */
extern unsigned long long g_p670_rc_exec_cum;
extern unsigned long long g_p670_r22_writes_cum;
#endif

#if P214_ENABLE
#ifndef P303_ENABLE
#define P303_ENABLE            0   /* D-4凍結中のため休止(0=probe休止, 1=計測有効) */
#endif
#else
#define P303_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P305_ENABLE
#define P305_ENABLE            0   /* D-4凍結中のため休止(0=probe休止, 1=計測有効) */
#endif
#else
#define P305_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

#if P214_ENABLE
#ifndef P312_ENABLE
#define P312_ENABLE            0   /* D-4凍結中のため休止(0=probe休止, 1=計測有効) */
#endif
#else
#define P312_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

/* ====================================================================
 * P291 診断 probe — D-4(グラディウスII 遅延+点滅)調査。毎フレーム、
 * 表示走査線ごとにY方向で重なるスプライト数を計測し、フレーム内最大値と
 * 複数閾値(16/24/32)超過走査線数を記録する(読み取りのみ・エミュレーション
 * 状態は一切変更しない)。可視判定式はCore/px68k/x68k/bg.cのSprite_DrawLineMcr
 * を参照して独立に再実装(Core無変更)。P214_ENABLEには依存しない独立フラグ
 * (Sprite_Regs/BG_VLINE/disp_hのみに依存)。使用後(本サイクル完了後)は 0 に戻す。
 * ==================================================================== */
#ifndef P291_ENABLE
#define P291_ENABLE            1   /* D-4実測のため一時的に有効化(0=probe休止, 1=計測有効) */
#endif

#ifndef P295_ENABLE
#define P295_ENABLE            0   /* D-4: ラスタ(CRTC IntLine)割込み発火計測(0=休止, 1=有効)。
                                      ★P515: P214_ENABLE=0 との組合せでビルドを通すため同時休止が必須
                                      (s_p295_raster_irq_fires は #if P214_ENABLE 内で宣言される一方、
                                      使用箇所は #if P295_ENABLE 内にのみ在るため、P214 単体の休止では
                                      未定義参照になる。P515 Code Review が実機 clang で確認済み)。
                                      D-4 は解決済み。 */
#endif

/* ====================================================================
 * P217 診断 probe — 合成ループで「BG フラグが立っているが色は 0」の画素を数える
 * だけ（測定のみ・描画は 1 画素も変えない）。P214 とは別マクロにしてある:
 * P214 の R1 は表示行ごとに ~1KB をハッシュする重い probe で、既知の性能課題を
 * 悪化させるため休止中。P217 はフレーム毎の整数カウンタのみ。
 * 出力は 60 フレームに 1 行 + パレット/BG_CHRSIZE の一度きりのダンプ。
 * ==================================================================== */
#ifndef P217_PROBE
#define P217_PROBE             0   /* 0 = probe 休止 (P216 と byte-identical), 1 = 計測有効 */
#endif

/* ====================================================================
 * P281 — BG0/BG1手動表示トグル機構の有効フラグ。元はD-20切り分け用の
 * 0.5秒自動巡回アブレーションだったが、P353でモニタUI(BGMonitorView)
 * からの明示的なON/OFF操作(mx68k_set_bg_layer_visible)に置き換えられた。
 * BG_Regs[9] を Bridge 側で描画直前に一時退避→書換え→直後に復元するだけで、
 * Core は無改変。デフォルト両方表示 — 通常動作は byte-identical。
 * ==================================================================== */
#ifndef P281_ENABLE
#define P281_ENABLE            1   /* BGレイヤー表示制御機構(P353)の有効フラグ */
#endif

#ifndef P282_ENABLE
#define P282_ENABLE            1   /* OverTake調査のため一時再有効化。0 = probe 休止 (byte-identical), 1 = 計測有効 */
#endif

#ifndef P283_ENABLE
#define P283_ENABLE            0   /* 0 = probe 休止 (byte-identical), 1 = 計測有効 */
#endif

/* ====================================================================
 * P218 診断 probe — 固着ループが「実際にどのゲストアドレスを読んでいるか」を
 * 記録するだけ（測定のみ・ゲスト状態は 1 バイトも変えない）。
 * これは H1（CRTC 高速クリア）を反証しうる probe である: payload は probe が
 * 選ばない値（= 実効アドレス）なので、H1 に同意することしかできない probe では
 * ない。判定基準は .mx68k_cycles/P218_plan.md §4.4 に事前登録済み。
 * サイクル完了後は 0 に戻して休止させる（P214/P217 と同じ運用）。
 * ==================================================================== */
#ifndef P218_PROBE
#define P218_PROBE             0   /* 1 = 計測有効（本サイクル）, 0 = 休止 */
#endif
#if P218_PROBE
/* 黒画面の問い（plan §5）を実測に預ける一度きりのスキャン。tag は "G"/"S"。 */
void p218_gvram_census_once(const char *tag);
#endif

/* ====================================================================
 * P220 — メモリ SIZE 設定をゲスト SRAM $ED0008 に届けたことを実測する probe。
 * reset_hard 末尾の write が guest runtime まで生存するか（IPL 上書きの有無）を
 * 裁定する。判定基準は .mx68k_cycles/P220_plan.md §4 に事前登録済み。
 * commit 時は 0（休止）。テスト走行時のみ 1 に上げて計測し、merge 前に 0 へ戻す
 * （P214/P217/P218 と同じ往復運用）。 */
#ifndef P220_PROBE
#define P220_PROBE             0   /* 1 = 計測有効（本サイクル）, 0 = 休止 */
#endif

/* P221b: falsifiable probe — reset_hard で machine/clock/導出 XVIMode/$E8E00B
 * 実読値を 1 行ログ。GO 条件: 既定(SCSI/16MHz)=XVIMode 1 & $E8E00B=0xFE、
 * SASI/10MHz=XVIMode 0 & $E8E00B=0xFF。commit 時は 0（休止）。テスト走行時のみ
 * 1 に上げて計測し merge 前に 0 へ戻す（P214/P217/P218 と同じ往復運用）。
 * m68000_bridge.c の P221_PROBE とは別名で衝突回避。 */
#ifndef P221B_PROBE
#define P221B_PROBE            0   /* 測定フェーズ: 1（Build & Test が commit 前に 0 化） */
#endif

/* ====================================================================
 * P413 — D-25(SI 幻ボード再出現)の真因特定用の読み取り専用プローブ
 * [P413-RANGEWATCH] のゲート。幻ボード検出アドレス窓(AWESOME-X $EC0000
 * 帯・G-RAM bank $EE0000 帯・POLYPHON $EFF800/$EFF880・RS-232C $EAFC00)
 * への読み書きを、成功/失敗(BusErrFlag の値)に関わらず無条件に記録する。
 * 「0 件」の意味を「SI が一度もアクセスしなかった」の 1 通りに確定させる
 * ため、他プローブと共有しない専用カウンタを持つ(P411 で起きた「ログ上限
 * 到達で消えた」という紛れを構造的に排除する)。
 * P214_ENABLE(D-4 表示診断)には連動させない — D-25 調査専用の一時
 * プローブであり、既存の P214 運用に巻き込まれないよう独立させる
 * (調査終了後は本フラグ単独で 0 に戻して休止できる)。
 * 読み取り専用: guest メモリ・CPU 状態・BusErrFlag を一切変更しない。
 * ==================================================================== */
#ifndef P413_ENABLE
#define P413_ENABLE            1   /* D-25(幻ボード)真因特定用、調査終了後に休止 */
#endif

/* ====================================================================
 * P419 — D-26(SI が $E9E200 帯を「未知のボード」として検出し続ける)の
 * 修正と、その検証プローブのゲート。
 *
 *  P419_ENABLE:        読み取り専用プローブ [P419-FPUWATCH] のゲート。
 *                      $E9E000-$E9FFFF への read を成功/失敗に関わらず
 *                      無条件に記録する(条件付きフィルタを持たないため
 *                      「0 件」の意味が「起動シーケンス中この帯域を一度も
 *                      読んでいない」の 1 通りに確定する)。P413 と同じ
 *                      設計原則。guest メモリ・CPU 状態・BusErrFlag を
 *                      一切変更しない。
 *  P419_BUSERR_ENABLE: $E9E000-$E9FFFF(純正 FPU ボード CZ-6BP1/CZ-6BP1A
 *                      の CIR 窓)を無条件バスエラー化する挙動変更のゲート。
 *                      Core 側は当該帯域を rm_nop(0 を返すのみで
 *                      BusErrFlag を立てない)として扱うため、P209 の
 *                      BusErrFlag&1 分岐は永久に偽 — P221d(Mercury 空窓)と
 *                      同型の無条件合成のみが有効。
 * ==================================================================== */
#ifndef P419_ENABLE
#define P419_ENABLE            1   /* D-26調査用[P419-FPUWATCH]プローブ */
#endif
#ifndef P419_BUSERR_ENABLE
#define P419_BUSERR_ENABLE     1   /* D-26修正: $E9E000-$E9FFFF無条件バスエラー化 */
#endif

/* ====================================================================
 * P424 — D-23(ロード→タイトル画面遷移の遅延)調査用、メモリアクセス
 * フックの呼出し回数を「分子/分母のペア」で毎フレーム記録する読み取り
 * 専用プローブ [P424-HOTPATH] のゲート。
 *
 *  trace_Memory_ReadB/ReadW/WriteB/WriteW の呼出し回数(= フレーム総
 *  メモリアクセス数 acc)を最上位の分母として併記するため、個別フックが
 *  無罪だった場合(acc 自体が読み込み中に跳ねない = フック税仮説の族
 *  丸ごとが的外れ)も一意に判定できる。同様に pc=walks/calls・
 *  p214=hits/calls も分子/分母のペアで出力し、「呼ばれていない」と
 *  「呼ばれたが素通り」を区別可能にする(P413/P419 と同じ設計原則)。
 *  guest メモリ・CPU 状態・BusErrFlag を一切変更しない。
 * ==================================================================== */
#ifndef P424_ENABLE
#define P424_ENABLE            1   /* D-23調査用[P424-HOTPATH]プローブ */
#endif

/* ====================================================================
 * P430 — D-5(MMDSP終了後にBG0のゴミが実画面に残る)調査用、読み取り
 * 専用プローブ [P430-BGIDX0] のゲート。
 *
 *  仮説: XM6 は BG パレットコードの下位4bit が 0 の画素へ REND_COLOR0
 *  (render.cpp:1358-1361)を立て、合成時(rend_asm.asm:6257-6261)に
 *  無条件で透明として扱う。MX68K の合成判定(has_bg = Text_TrFlag&2 &&
 *  tcol)には相当する判定が無い。この差が MMDSP のゴミの原因かは未実測。
 *
 *  検出方式は「既存データ同士の値比較」——Core の内部計算(タイル反転
 *  4分岐など)を一切再現しないため、複製ロジック自体のバグという解釈が
 *  原理的に存在しない。BG_DrawLine(0,1) 直前に TextPal32[N*0x10]
 *  (N=1..15 = 各パレットブロックの index0)を退避し、直後に
 *  BG_LineBuf32 の表示範囲を走査して一致画素を数える。分子(marker_hit)
 *  だけでなく分母2種(scanned / bg_drawn)と経路フラグ(bg_above_text /
 *  bg0_on / sp_on)を同一行に併記するので、marker_hit=0 の解釈が
 *  (a)仮説誤り (b)別経路 (c)BG0無効 (d)sp_on=0 に一意化される。
 *  guest メモリ・既存バッファを一切変更しない(読み取りのみ)。
 * ==================================================================== */
#if P214_ENABLE
#ifndef P430_ENABLE
#define P430_ENABLE            1   /* D-5調査用[P430-BGIDX0]プローブ(D-5はP436で別原因により解決済み・
                                      本プローブの前提仮説はP432で反証済み。再開時はP214_ENABLE=1で復活) */
#endif
#else
#define P430_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

/* ====================================================================
 *  P529: [P256-DIAG](SCSI SPC 読み書き診断)を P214 カスケードへ編入
 *  P527 のアイドル実測で、最大スパイク(59.4ms)直前の 1 秒間に本プローブが
 *  単一フレーム内で 4989 回発火していた。1 行あたり無バッファ stderr への
 *  vfprintf + ファイルへの vfprintf + fflush() を mutex 下で実行するコスト
 *  ((59.406−平常7.9)ms÷4989行 = 10.3µs/行)がフレーム時間超過の主因と
 *  推定される。本プローブの調査対象だった READ(10) 経路の競合は P264 で
 *  解決済みで、コード内コメント自身が「一時計装・削除予定」と明記している。
 *  ★ゲート範囲は debug_log() 呼出しのみ。s_p256_trace_count の
 *  インクリメント・上限判定 (P256_TRACE_MAX) と StatBar_HDD(1) は
 *  P256_ENABLE の値に関わらず従来どおり実行される(挙動不変)。
 * ==================================================================== */
#if P214_ENABLE
#ifndef P256_ENABLE
#define P256_ENABLE            1   /* P256調査用[P256-DIAG]プローブ(SCSI SPC
                                      読み書き診断)。対象のREAD(10)経路競合は
                                      P264で解決済み・本プローブは役目を終えた
                                      一時計装。再開時はP214_ENABLE=1で復活 */
#endif
#else
#define P256_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

/* ====================================================================
 *  P533: D-40診断プローブ [P533-XSNAP]
 *  でたな!!ツインビーの画面遷移時の (1)左端の縦帯ゴミ (2)KONAMIロゴ演出の
 *  配色崩壊 について、H1(Pal_TrackContrast() の±1追従)・H2(GVRAM_FastClear
 *  の非ラップ書込み)・H3/Spec仮説(CRTCレジスタ即時反映=フレーム内での
 *  ジオメトリ/パレット変化)のいずれが実際の遷移フレームで発火しているかを
 *  1行で同時記録する。mx68k_render_begin()/mx68k_render_end() の両方で
 *  同じ項目を採取し、必ず両方(_b/_e)を出力するため「変化なし」と
 *  「プローブ未到達」を標本自身で区別できる。トリガ条件なし・毎フレーム
 *  1行。既存グローバルの読み取りと debug_log() のみで、エミュレーション
 *  状態への書込みは一切ない(read-only計装)。
 * ==================================================================== */
#if P214_ENABLE
#ifndef P533_ENABLE
#define P533_ENABLE            1   /* D-40調査用[P533-XSNAP]プローブ。
                                      再開時はP214_ENABLE=1で復活 */
#endif
#else
#define P533_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

/* ====================================================================
 *  P570: D-51診断プローブ [P570-GEOM]
 *  Phalanx オープニング演出のスケーリング異常(症状1=横伸び/症状2=縦伸び)
 *  について、仮説 H-1「MXは CRTC の表示ウィンドウ寸法(TextDotX×TextDotY)を
 *  そのまま公開フレームバッファ寸法として publish し、ホスト側が固定4:3へ
 *  フルストレッチするため、ゲストが表示窓だけを狭めた場合に実機では出るはずの
 *  ボーダーの代わりに拡大が起きる」を実測で判定する。
 *  判定の要は「TextDotX/TextDotY の非標準値が『画面モード切替(R20変化)』
 *  なのか『表示ウィンドウ縮小(R02/R03・R06/R07 のみ変化)』なのか」であり、
 *  そのため派生値(TextDotX/TextDotY)だけでなく計算元の生値
 *  (CRTC_HSTART/HEND/VSTART/VEND・CRTC_Regs[0x28]/[0x29]・CRTC_VStep)を
 *  同一行へ併記する —— 標本自身から事後判定できるようにするため。
 *  mx68k_render_begin()/mx68k_render_end() の両方で同じ項目を採取し必ず
 *  両方(_b/_e)を出力するので、「変化なし」と「プローブ未到達」を標本自身で
 *  区別できる。加えて rows_drawn/rows_total(=disp_h)という分母つきの対を
 *  持つため、「描画行ゼロ」と「プローブが走らなかった」も区別できる。
 *  トリガ条件なし・毎フレーム1行・フレーム番号による間引きなし。
 *  ★P595 (D-55): 行末の出力を P571 のレターボックス専用フィールド
 *  (outw/outh/offx/offy/lb)から連続スケール式の
 *  hscale/vscale/offxf/offyf/geo_mode(%.4f)へ差し替えた。分岐条件そのものの
 *  生値 r00_b/r04_b(フレーム頭の実測 R00/R04)も同一行へ併記するので、
 *  読者は geo_mode の分岐と派生値を手計算で再現・反証できる。
 *  既存グローバルの読み取りと debug_log() のみで、エミュレーション状態への
 *  書込みは一切ない(read-only計装)。
 *  ★P214_ENABLE への従属は意図的に付けない: D-51 は D-20 系とは独立の調査で
 *  あり、P214 の休止に巻き込まれて本プローブが黙って消えると自己反証可能性
 *  (0行の解釈が一意であること)が損なわれるため。
 * ==================================================================== */
#ifndef P570_ENABLE
#define P570_ENABLE            1   /* D-51調査用[P570-GEOM]プローブ。既定で有効 */
#endif

/* ====================================================================
 *  P534: D-40症状1 診断プローブ [P534-GVWCOL]
 *  でたな!!ツインビーの画面左端16ドットのゴミ帯について、「MPX68Kが行う
 *  掃除バースト(GVRAM列496-511への5フレーム・12288バイト書込み)をMXも
 *  受け取り、同じ列へ適用しているか」という残る1問だけを [runtime log]
 *  階へ引き上げる。Core/px68k/x68k/gvram.c の GVRAM_Write() を
 *  Bridge/p534_gvwcol_hook.h の force-include で改名し、Bridge側の同名
 *  ラッパで列別に計数する(判定は gvram.c:146-211 の逐語転記)。
 *  出力は mx68k_render_end() で毎フレーム1行・トリガ条件なし・フレーム
 *  番号による間引きなし(P533の[P533-XSNAP]が症状1の瞬間を取り逃した
 *  直接原因が間引き/条件ゲートだったため)。分母を2段(wr256/calls)持ち、
 *  ゲートが依存する生値(r28=CRTC_Regs[0x28])も併記するので、0行の解釈が
 *  一意になる。前置12フィールドはMPX68K [R1-GVWCOL] と同名・同順・同書式で
 *  行単位 diff できる。カウンタ加算とdebug_log()のみで、エミュレーション
 *  状態への書込みは一切ない(read-only計装)。
 * ==================================================================== */
#if P214_ENABLE
#ifndef P534_ENABLE
#define P534_ENABLE            1   /* D-40調査用[P534-GVWCOL]プローブ。
                                      再開時はP214_ENABLE=1で復活 */
#endif
#else
#define P534_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

/* ====================================================================
 *  P435: D-5検証プローブ [P435-SPGATE]
 *  MPX68K(px68k上流)の sprite/BG 描画ゲートは3条件
 *  ((VCReg2[1]&0x40) && (BG_Regs[8]&2) && !(BG_Regs[0x11]&2))、
 *  MX68K の sp_on は1条件のみ。3条件版で判定した場合の可否
 *  (mpx_would_draw)と MX68K の実際の可否(mx_draws)を同一行に併記し、
 *  食い違う区間を直接特定する。各ゲートの個別評価結果と
 *  生レジスタ値(BG8/BG9/BG10/BG11)も併記するので、両者一致の場合でも
 *  (a)ゲート無関係 (b)未定義h_res状態が発生しなかった、を後から
 *  区別できる。読み取りとログ出力のみ(挙動変更ゼロ)。
 * ==================================================================== */
#ifndef P435_ENABLE
#define P435_ENABLE            1   /* D-5検証用[P435-SPGATE]プローブ */
#endif

#ifndef P543_ENABLE
/* D-36着手条件検証用[P543-DISPWIN]プローブ(全表示期間のDISP乖離計測)。
 * ★P544で 0 に変更(コードは将来の再有効化のため残置)。理由: 本プローブの
 * 計測条件は `sp_on && !disp_ok`(上流なら描画しない・MXは描画する走査線)
 * だが、P544で sp_on 自身に DISP 項(BG_Regs[8]&0x02)を追加したため、
 * この条件はP544以降**恒真的に偽**となり、構造上いかなる乖離も検出できない。
 * 1 のまま残すと出力ゼロが「計測継続中・乖離なし」に見えてしまい、
 * 検出不能な状態を「クリーン」と誤読させる(自己反証可能性を失った
 * プローブになる)。再有効化する場合は sp_on から DISP 項を外した独立式で
 * 計測し直すこと。 */
#define P543_ENABLE            0
#endif

/* ====================================================================
 *  P436: D-5 書込み経路検証プローブ [P436-REGWRITE]
 *  BG_Regs[0x0f]-[0x11] ($EB080F-$EB0811) への書込みを、CPU経由
 *  (m68000_bridge.c の trace_Memory_WriteB/W)と DMA経由
 *  (EmulatorBridge.c の p367_dma_write_check)の両方から、既存プローブの
 *  300フレームサンプリング窓を介さず無条件(件数上限のみ)でログする。
 *  MMDSP 終了後に BG_Regs[0x11] が未定義値のまま固定される現象が、
 *  正当なゲスト書込みによるものか、MX68K 側の書込み経路の異常かを
 *  弁別する。読み取りとログ出力のみ(挙動変更ゼロ)。
 * ==================================================================== */
/*  CPU側ヘルパは m68000_bridge.c の #if P214_ENABLE 区画内に置かれるため、
 *  P367/P351 と同じく P214_ENABLE=0 のときは強制休止する(呼出しだけ残って
 *  未定義関数参照になるのを防ぐ)。P214_ENABLE=1 の現状では常に 1。 */
#if P214_ENABLE
#ifndef P436_ENABLE
#define P436_ENABLE            1   /* D-5書込み経路検証用[P436-REGWRITE]プローブ */
#endif
#else
#define P436_ENABLE            0   /* P214_ENABLE=0のときは強制的に休止 */
#endif

/* ====================================================================
 *  D-16 修正: プリンタ未接続 ($E9C001 / $E9C000 bit5 = READY) の是正
 *  D16_PRN_DISCONNECT_ENABLE: Bridge の CPU 可視読み取り経路
 *  (m68000_bridge.c の trace_Memory_ReadB / trace_Memory_ReadW) において
 *  $E9C001(byte 読み)および $E9C000(word 読みの下位バイト)の bit5
 *  (0x20 = プリンタ READY) を 0 にマスクするゲート。
 *
 *  理由: Core/px68k/x68k/ioc.c の IOC_Read() は、この bit5 を「読み取る
 *  たびにトグルする」副作用を持つ。これは本プロジェクトの Core fork に
 *  固有の実装で、XM6・px68k 本家・px68k-libretro の 3 参照実装との直接
 *  比較でいずれにも存在しないことを確認済み(3 系統とも、プリンタ未接続
 *  時は bit5 恒久 0 が正しい挙動)。またゲスト IPLROM の逆アセンブルに
 *  より、IOCS $3D _PRNSNS ($FF8506) が
 *      move.b $00E9C001,d0 / and.l #$20,d0 / rts
 *  としてまさにこの bit5 をそのまま返すことを確認済み — ゲストユーティ
 *  リティ si が「printer : online」と誤表示する直接の原因である。
 *
 *  Core の内部状態(IOC_IntStat のトグル)は一切変更しない。CPU へ返す
 *  値だけを補正する(Core File Modification Policy 準拠)。
 * ==================================================================== */
#ifndef D16_PRN_DISCONNECT_ENABLE
#define D16_PRN_DISCONNECT_ENABLE 1  /* D-16修正: $E9C001/$E9C000 bit5(プリンタREADY)を0にマスク */
#endif

/* ====================================================================
 *  P492: D-43(PCM8PP常駐時のフリーズ)調査用プローブ [P492-IOHIST]
 *  フリーズ中にゲスト側ソフトがどの I/O デバイス窓を読み続けているかを、
 *  既知のデバイス窓ごとの read 回数ヒストグラムとして直接実測する。
 *  窓境界は Core/px68k/x68k/mem_wrap.c:58-96 の MemReadTable[]
 *  (8KB=0x2000 刻み)から逆算(Fix Plan の記号表参照)。
 *
 *  自己反証可能性: 全バケットに加えて分母(I/O 空間外の read 数・総 read 数・
 *  フレーム番号)を同一行に併記するため、「全バケット 0」が
 *  (a)プローブが動いていない のか (b)本当にどの I/O 窓も見ていない のかを
 *  一意に判別できる。guest メモリ・CPU 状態・読み取り値を一切変更しない。
 *  一時診断用のため、真因判明後のサイクルで休止させる。
 * ==================================================================== */
#ifndef P492_ENABLE
#define P492_ENABLE            1   /* D-43調査用[P492-IOHIST]プローブ */
#endif
#if P492_ENABLE
/* m68000_bridge.c 定義。mx68k_run_frame() のフレーム末尾から毎フレーム呼ぶ
 * (出力自体は 300 フレームごと)。フレーム番号は引数で渡さず、関数内部で
 * g_mx68k_frame_num を直接参照する。 */
void p492_io_histogram_dump(void);
#endif

/* ====================================================================
 *  P602 (D-57): 同人版ソーサリアンが MC68040 搭載と誤検出される件の
 *  実行時診断プローブ群 [P602-WORKAREA] / [P602-EXCHIST] /
 *  [P602-BRFF] / [P602-IOHIST]。
 *
 *  4 プローブすべて観測専用 —— guest メモリ・c68k レジスタ・関数の戻り値・
 *  制御フロー・タイミングを一切変更しない(debug_log 出力と file-static
 *  カウンタへの加算のみ)。実装は Bridge/m68000_bridge.c(全プローブ本体)と
 *  Bridge/EmulatorBridge.c(起動直後スナップショットと毎フレーム呼び出しの
 *  2 箇所の呼出しのみ)。Core/ は無変更。
 *
 *  Fix Plan: .mx68k_cycles/P602_plan.md(記号表・自己反証可能性の根拠)。
 *  一時診断用。真因確定後のサイクルで他の役目を終えたプローブ同様に
 *  P214 カスケードへ編入して休止させる予定。
 * ==================================================================== */
#ifndef P602_ENABLE
#define P602_ENABLE            1   /* D-57調査用プローブ(0=休止, 1=計測有効) */
#endif
#if P602_ENABLE
/* m68000_bridge.c 定義。mx68k_reset_hard() の末尾(全 HW init 完了後・CPU 実行
 * 開始前)から 1 回だけ呼ぶ。IOCS ワークエリア $0CBC-$0CBF の初期値を記録し、
 * 同時に P602 の全カウンタを 0 に戻して「この run の観測開始点」を明示する。 */
void p602_workarea_snapshot(const char* tag);
/* m68000_bridge.c 定義。mx68k_run_frame() のフレーム末尾から毎フレーム呼ぶ
 * (出力自体は 300 フレームごと。P492 と同じ周期・同じ位置)。フレーム番号は
 * 引数で渡さず、関数内部で g_mx68k_frame_num を直接参照する。 */
void p602_periodic_dump(void);
#endif

// ---- Debug Logging ----
extern void debug_log(const char* fmt, ...);
void mx68k_log(const char* msg);
void mx68k_dump_framebuffer(void);
void mx68k_set_trace_enabled(bool enabled);

// ---- Error Codes ----
#define MX68K_OK                  0
#define MX68K_ERR_BIOS_NOT_FOUND -1
#define MX68K_ERR_BIOS_INVALID   -2
#define MX68K_ERR_INIT_FAILED    -3
#define MX68K_ERR_NO_MEMORY      -4
