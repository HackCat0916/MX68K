//---------------------------------------------------------------------------
//
//	MX68K — Internal SCSI (MB89352 SPC) runtime bridge  (P251 Stage 2c)
//
//	MX68K is a macOS port of the open-source Sharp X68000 emulator px68k.
//	"SASI" / "SCSI" / "SPC" here are the 1980s disk-interface standard /
//	chip names used by the X68000 hardware — this is ordinary emulator
//	development.
//
//	This translation unit owns the single static SCSI (MB89352) instance
//	ported from XM6 (scsi_spc.cpp) and exposes a small extern "C" surface so
//	the C bridge (scsi_in_bridge.c) can drive it. It also provides the real
//	backing for the Memory / SRAM compatibility stubs declared in
//	scsi_compat_shim.h.
//
//	Scope note (P251): interrupts stay disabled (CPU::Interrupt is a no-op
//	in the shim) and no disk image is attached (scsihd[] paths are empty, so
//	scsi.disk[*] == nullptr). Only the register / phase state machine is
//	wired.
//
//	See NOTICE-THIRD-PARTY.md at the repository root.
//
//---------------------------------------------------------------------------

#include "scsi_spc.h"
#include "scsi_compat_shim.h"

#include <cstdio>	// P576: [P253] ログの ID0-6 固定フォーマット組み立て(snprintf)
#include <cstring>	// P692: mx68k_scsi_live_path() の strlcpy

// P510: P510_SCSI_MODE_* / mx68k_get_scsi_ext_disk_path() の単一の定義元。
// EmulatorBridge.h は純 C ヘッダ(extern "C" ガードを持たない)なので、
// この翻訳単位からは extern "C" で包んで取り込む。定数を C++ 側で複写しない
// ことが目的 — 複写すると値のずれが静かに入り込む余地が生まれる。
extern "C" {
#include "EmulatorBridge.h"
}

//---------------------------------------------------------------------------
//	C symbols from the px68k core / EmulatorBridge.c. Declared extern "C"
//	here so the C++ linker resolves them without name mangling.
//---------------------------------------------------------------------------
extern "C" {
	// px68k SRAM (Core/px68k/x68k/sram.c) — real battery-backed memory.
	uint8_t SRAM_Read(uint32_t adr);
	void    SRAM_Write(uint32_t adr, uint8_t data);
	// px68k system-port write (Memory_WriteB == cpu_writemem24). Used only to
	// bracket the SRAM access-enable gate ($E8E00D), the same idiom px68k's
	// own SRAM_SetSASIDrive / SRAM_SetSCSIMode use.
	void    cpu_writemem24(uint32_t adr, uint32_t data);
	// Internal SCSI IPL ROM buffer held in EmulatorBridge.c (P247).
	extern uint8_t s_scsi_in_rom[];
	// One-time sram.dat backup taken before the first real SRAM write.
	void    mx68k_backup_sram_before_scsi_wiring(void);
	void    debug_log(const char* fmt, ...);
	// Internal SCSI HD disk-image path held in EmulatorBridge.c (P253).
	const char* mx68k_get_scsi_in_disk_path(int id);
	// P269: status-bar HDD busy pulse (status_bridge.c). Fired on DREG data
	// transfer independently of the P256 diagnostic trace cap.
	void    StatBar_HDD(int32_t sw);
	// P450: 「メモリスイッチ自動更新」設定値(EmulatorBridge.c)。SetMemSw の
	// 第2層ゲートで、書込のその瞬間の設定を読むために使う。
	bool    mx68k_get_memsw_auto_update(void);
}

//---------------------------------------------------------------------------
//	SCSI configuration mode threaded in from mx68k_reset_hard(). The globals it
//	is derived from (g_machine_type, g_scsi_ext_board_installed,
//	s_scsi_ext_rom_loaded) have internal linkage in EmulatorBridge.c and cannot
//	be extern'd, so the derived value is passed across the C boundary rather
//	than shared directly (same precedent as scsi_in_bridge.c's install argument).
//
//	P510: was scsi_real_set_machine_type(int), which only ever distinguished
//	SASI from internal SCSI and was called from inside scsi_in_bridge.c's
//	gate-satisfied branch only — on a SASI machine the previous reset's value
//	stayed behind (the P447/D-31 one-way-write defect shape). It is now set
//	unconditionally, exactly once per hard reset, to one of the three
//	P510_SCSI_MODE_* values, from the single call site in mx68k_reset_hard().
//	Default INT preserves the pre-P510 initial value (was 4 == SCSI).
//---------------------------------------------------------------------------
static int s_scsi_mode = P510_SCSI_MODE_INT;

extern "C" void scsi_real_set_scsi_mode(int mode) {
	s_scsi_mode = mode;
}

extern "C" int scsi_real_get_scsi_mode(void) {
	return s_scsi_mode;
}

// P687 (D-29 C): ログ表示用のモード名。scsi_spc.cpp の [P687] scsi Open failed
// が内蔵/外付けを事後判別するために使う。★変換をここに置くのは、
// P510_SCSI_MODE_* の定数値を scsi_spc.cpp 側へ複写させないため
// (定義元は EmulatorBridge.h の 1 箇所だけに保つ)。呼び出し側は
// この名前と scsi_real_get_scsi_mode() の生値を必ず両方出力すること。
extern "C" const char* scsi_real_get_scsi_mode_name(void) {
	switch (s_scsi_mode) {
		case P510_SCSI_MODE_SASI: return "sasi(unexpected)";
		case P510_SCSI_MODE_EXT:  return "external";
		case P510_SCSI_MODE_INT:  return "internal";
		default:                  return "unknown";
	}
}

//---------------------------------------------------------------------------
//	Memory / SRAM compatibility-stub backing (declared in scsi_compat_shim.h).
//---------------------------------------------------------------------------
Memory::memtype Memory::GetMemType() const {
	// SCSI::Reset() maps these to scsi.type 0 / 1 / 2 respectively
	// (scsi_spc.cpp:183-201).
	switch (s_scsi_mode) {
		case P510_SCSI_MODE_SASI: return Memory::SASI;
		case P510_SCSI_MODE_EXT:  return Memory::SCSIExt;
		default:                  return Memory::SCSIInt;
	}
}

const BYTE* Memory::GetSCSI() const {
	return s_scsi_in_rom;
}

DWORD SRAM::GetMemSw(DWORD offset) const {
	return SRAM_Read(0x00ed0000u + offset);
}

void SRAM::SetMemSw(DWORD offset, DWORD data) {
	// Faithfully reproduce the exact memory-switch bytes SCSI::Reset() writes
	// (guest $ED006F/70/71), each bracketed by the $E8E00D access-enable
	// toggle — the same single-write idiom as px68k's SRAM_SetSASIDrive.
	// Deliberately NOT routed through SRAM_SetSCSIMode(2): that helper also
	// rewrites the ROM boot handle $ED000C-0F.
	//
	// P508 update: the ban on rewriting $ED000C itself (P251 plan §9, D-18:
	// pointing the boot handle at an unmapped SCSI IPL breaks boot) has lost
	// its premise — D-18 was resolved in P247-P267, and P508 now writes
	// $ED000C-0F deliberately from sasi_bridge.c
	// (sasi_bridge_apply_rom_boot_handle) so the ROM boot handle tracks the
	// wired SCSI configuration. What is retained is the design rule rather
	// than the ban: never go through SRAM_SetSCSIMode(), whose single mode
	// argument lets an argument mix-up rewrite the boot handle by accident —
	// use a dedicated function with unconfusable arguments (P450/P456 idiom).
	// Reset() never asks us to write $ED000C, so by construction this path
	// still cannot touch it.
	//
	// P450 第2層ゲート: 「メモリスイッチ自動更新」が OFF なら SRAM に一切
	// 触れない。第1層(SCSI::Init() の scsi.memsw)は Init 時点の値を 1 度だけ
	// キャッシュするため、その鮮度は「毎ハードリセットで Init が再実行される」
	// という P450 (E) の変更に依存する。この層は書込のその瞬間に設定を読むので、
	// (E) が将来変更・revert されても OFF の約束が破れない。異なる失敗モードを
	// 塞ぐ 2 枚であり、同一原因の二重ガードではない。
	if (!mx68k_get_memsw_auto_update()) return;
	cpu_writemem24(0x00e8e00du, 0x31);                 // allow SRAM access
	SRAM_Write(0x00ed0000u + offset, (uint8_t)data);
	cpu_writemem24(0x00e8e00du, 0x55);                 // block SRAM access
}

//---------------------------------------------------------------------------
//	Static SCSI (MB89352) instance + extern "C" bridge surface.
//---------------------------------------------------------------------------
static VM    s_vm;
static SCSI* s_scsi_instance = nullptr;

// P264: Event 遅延発火状態の唯一の実体定義（ODR: 1 TU のみ）。
bool    Event::s_defer_next_fire = false;
Device* Event::s_pending_dev     = nullptr;
Event*  Event::s_pending_ev      = nullptr;

// P264 レビュー4.1対策: 未ドレイン pending を新選択が supersede した際の診断ログ。
// 本 ROM の線形フローでは発火しないはずのパス — 出力されたら stale-pending 前提が崩れた印。
extern "C" void mx68k_scsi_defer_stale(void) {
	debug_log("[P264] WARN: stale deferred select-timeout superseded by a new selection "
	          "(discarded, not replayed)\n");
}

// Construct (once) the internal SCSI SPC. A one-time sram.dat backup is taken
// BEFORE any SRAM write can occur (SCSI::Reset() writes the memory switches).
extern "C" void scsi_real_install_construct(void) {
	if (s_scsi_instance) return;
	mx68k_backup_sram_before_scsi_wiring();
	s_scsi_instance = new SCSI(&s_vm);
	s_scsi_instance->Init();
	// P274: pull in the HD image paths (ID0-6) BEFORE Reset(), so the
	// Construct() that Reset() invokes below sees non-empty scsihd[id] and
	// opens real disks (scsi.disk[id] != nullptr).
	// P510: the source depends on the configuration mode — the external
	// CZ-6BS1 keeps its images in Config.SCSIEXHDImage[], a separate array
	// from the internal machine's.
	// P576: 各IDについて「パスが設定されていたか」の生値を控えておく。
	// [P253] ログで open(派生値) の隣にこの生値を併記することで、
	// 「パスは設定されていたのに Open されなかった」(=D-52 の症状そのもの)を
	// ログ1行から自己反証可能な形で読み取れるようにする。
	int p576_path_set[7] = {0, 0, 0, 0, 0, 0, 0};
	for (int id = 0; id < 7; id++) {
		const char* p = (s_scsi_mode == P510_SCSI_MODE_EXT)
		              ? mx68k_get_scsi_ext_disk_path(id)
		              : mx68k_get_scsi_in_disk_path(id);
		if (p && p[0]) {
			p576_path_set[id] = 1;
			s_scsi_instance->SetDiskPath(id, p);
		}
	}
	// P668: MO ディスクパスを Reset()->Construct() の前に流し込む。
	// ★HD と違い ID 引数を取らない(MO は ID5 固定の単一スロット)。
	// 内蔵/外付けのどちらのモードでも同じ MO パスを使う — MO は
	// SPC(MB89352)配下のデバイスであり、s_scsi_mode は
	// 「HD イメージ配列をどちらから読むか」の分岐にすぎないため。
	const char* p668_mo = mx68k_get_mo_path();
	int p668_mo_path_set = (p668_mo && p668_mo[0]) ? 1 : 0;
	s_scsi_instance->SetMOPath(p668_mo ? p668_mo : "");
	// P676: CD-ROM イメージパスも同様に Construct() の前に流し込む(ID6 固定)。
	// 内蔵/外付けのどちらでも同じ CD パスを使う理由は MO と同じ。
	const char* p676_cd = mx68k_get_cd_path();
	int p676_cd_path_set = (p676_cd && p676_cd[0]) ? 1 : 0;
	s_scsi_instance->SetCDPath(p676_cd ? p676_cd : "");
	// Reset() internally calls Construct() right after writing the memory
	// switches, so we do not — and cannot — call the private Construct()
	// ourselves.
	s_scsi_instance->Reset();       // Construct() + memory-switch write happen here, once
	SCSI::scsi_t st;
	s_scsi_instance->GetSCSI(&st);
	// P576: ID0-6 の全7件を毎回・固定フォーマットで出力する。
	// 旧来は disk[0] 単独報告だったため「ID5 が未接続」なのか「ログがそもそも
	// ID5 を報告しない設計」なのかを区別できなかった(自己反証不能)。
	// 拡張後は「シグナル無し」の解釈が「この行自体が出力されなかった」の
	// 1通りに限定される。open は派生値、path はその判定の元になった生値。
	// 既存フィールド(mode/type/disk[0])は過去ログとの互換性のため保持する。
	char p576_ids[192];
	int  p576_pos = 0;
	for (int id = 0; id < 7 && p576_pos < (int)sizeof(p576_ids) - 1; id++) {
		int n = snprintf(p576_ids + p576_pos, sizeof(p576_ids) - (size_t)p576_pos,
		                 " id%d_open=%d id%d_path=%d",
		                 id, st.disk[id] ? 1 : 0, id, p576_path_set[id]);
		if (n <= 0) break;
		p576_pos += n;
	}
	p576_ids[sizeof(p576_ids) - 1] = '\0';
	debug_log("[P253] scsi_real_install_construct: SCSI SPC constructed, mode=%d type=%d disk[0]=%s hdmax=%d%s mo_path=%d cd_path=%d\n",
	          s_scsi_mode, (int)st.type, st.disk[0] ? "OPEN" : "not attached",
	          (int)SCSI::HDMax, p576_ids, p668_mo_path_set, p676_cd_path_set);
}

// P510: SPC の SCTL レジスタ生値。[P510-SCTL] の分母行が「SCTL の bit0 が
// 一度も立たなかった」と「SPC がそもそも構築されていない」を区別できるよう、
// 未構築時は 8 bit レジスタでは到達しえない 0xFFFFFFFF を返す(派生判定は
// 呼び出し側では行わず、生値をそのままログへ出す)。
extern "C" uint32_t scsi_real_get_sctl(void) {
	if (!s_scsi_instance) return 0xFFFFFFFFu;
	SCSI::scsi_t st;
	s_scsi_instance->GetSCSI(&st);
	return (uint32_t)st.sctl;
}

//---------------------------------------------------------------------------
//	P692: Storage Monitor 用のライブ状態アクセサ(読み取り専用、4 本)
//
//	既存の設定側アクセサ(mx68k_get_scsi_in_disk_path / mx68k_get_scsi_ext_disk_path
//	/ mx68k_get_mo_path / mx68k_get_cd_path)は Bridge 側シャドウ(= pending config)
//	であり、実際に走行中の SPC が何を開いているかとはハードリセットを挟んでずれる。
//	この 4 本は走行中インスタンス自身(s_scsi_instance->GetSCSI())だけを単一の
//	真実源として読むため、Core 側 scsi.c の per-block open 経路が生きているか
//	どうかに関わらず正しい。Storage Monitor は両者を必ず並べて表示し、食い違う
//	場合に「⌘R 待ち」を明示する。
//
//	ガード方針は既存 scsi_real_get_sctl() と同じ ——
//	s_scsi_instance 未構築時・ID 範囲外・disk[id]==NULL のいずれでも安全な既定値
//	(mask=0 / kind=-1 / ready=false / path="")を返し、逆参照しない。
//
//	★スレッド安全性(P692 Fix Plan §C): disk[id] は MO(ID5)/CD(ID6)の
//	ライブ Insert/Eject がメインスレッドから delete/new する生ポインタなので、
//	Swift 側の呼び出しは mx68k_run_frame() と同じ emulationLock で囲むこと
//	(EmulatorEngine.fetchMonitorsAndPerfStats() の storageVisible ブロック)。
//---------------------------------------------------------------------------

extern "C" uint32_t mx68k_scsi_live_attached_mask(void) {
	if (!s_scsi_instance) return 0u;
	SCSI::scsi_t st;
	s_scsi_instance->GetSCSI(&st);
	uint32_t mask = 0;
	for (int id = 0; id < 7; id++) {
		if (st.disk[id]) mask |= (uint32_t)(1u << id);
	}
	return mask;
}

extern "C" int mx68k_scsi_live_kind(int id) {
	if (id < 0 || id > 6) return -1;
	if (!s_scsi_instance) return -1;
	SCSI::scsi_t st;
	s_scsi_instance->GetSCSI(&st);
	if (!st.disk[id]) return -1;
	// 記号表(P692 Fix Plan): 'SCHD'/'SCMO'/'SCCD' は Construct() 側の
	// 種別判定(scsi_spc.cpp:3672 / :3763 / :3819)と同一の定数。
	switch (st.disk[id]->GetID()) {
		case MAKEID('S', 'C', 'H', 'D'): return 0;
		case MAKEID('S', 'C', 'M', 'O'): return 1;
		case MAKEID('S', 'C', 'C', 'D'): return 2;
		default:                         return -1;
	}
}

extern "C" bool mx68k_scsi_live_ready(int id) {
	if (id < 0 || id > 6) return false;
	if (!s_scsi_instance) return false;
	SCSI::scsi_t st;
	s_scsi_instance->GetSCSI(&st);
	if (!st.disk[id]) return false;
	return st.disk[id]->IsReady() ? true : false;
}

extern "C" const char* mx68k_scsi_live_path(int id) {
	// ★呼び出しの都度 disk[id]->GetPath(fp)(out-param)でスタックローカルな
	// Filepath を埋め、その中身をこの静的バッファへ複写して返す。バッファは
	// 共有・上書きされるため、呼び出し側は戻り値を次の呼び出しの前に必ず
	// 自前の文字列へ変換すること(Swift 側は String(cString:) 即時変換)。
	// サイズ 520 は Filepath::m_szPath の宣言(scsi_disk.h:67)に合わせている。
	static char buf[520];
	buf[0] = '\0';
	if (id < 0 || id > 6) return buf;
	if (!s_scsi_instance) return buf;
	SCSI::scsi_t st;
	s_scsi_instance->GetSCSI(&st);
	if (!st.disk[id]) return buf;
	Filepath fp;
	st.disk[id]->GetPath(fp);
	const char* p = fp.GetPath();
	if (p) strlcpy(buf, p, sizeof(buf));
	return buf;
}

// P450: [P450-SCSIRECON] ログの自己反証性のため。teardown の直前に呼び、
// 「teardown が走らなかった」が「インスタンスが無かった」のか「そもそも
// 呼ばれなかった」のかをログ行単体で区別できるようにする。
extern "C" bool scsi_real_instance_exists(void) {
	return s_scsi_instance != nullptr;
}

// P275: 電源OFF時に内蔵SCSIインスタンスを解放し、次回電源ONで
// 新しいディスクパスを使って再構築できるようにする。
// ★P450 (D-30) で方針を反転: ハードウェアリセット(⌘R、mx68k_reset_hard)も
// この関数を経由するようになり、ディスク構成が毎リセットで再認識される。
// これは実機/XM6 の挙動(構成の再認識は電源 OFF/ON のみ)からの *意図的な逸脱*
// であり、Docs/05 §5.2「3. ハードリセットで反映」という受入基準と、P447 で
// SASI 側だけが ⌘R 反映になっていた非対称の解消を優先した設計判断である。
// 詳細は EmulatorBridge.c の mx68k_reset_hard() 内 P450 コメントを参照。
extern "C" void scsi_real_install_teardown(void) {
	if (!s_scsi_instance) return;   // 冪等
	s_scsi_instance->Cleanup();     // SCSIHD/Fileio/dcacheを正しく閉じ・書き戻す(必須 — delete単独だとリーク+書き戻し喪失)
	delete s_scsi_instance;
	s_scsi_instance = nullptr;
	Event::ResetDeferState();       // P264由来のstatic pending状態を予防的にクリア
}

// Register read/write. The offset is already normalized to 0x00-0x1f by
// scsi_in_dispatch_read/write in scsi_in_bridge.c (guest $E96020-3F minus
// 0x20), so ReadByte/WriteByte reach the register decode, never the ROM
// branch (addr >= 0x20) or the external-window bus-error guard.
// P256 Stage 2g / P257 Stage 2i: temporary DATA-IN-phase register trace.
// Diagnostic-only — does NOT alter the read/write result or any SCSI state.
// Stage 2g's 500-line cap covered TUR/REQUEST SENSE/READ CAPACITY but cut off
// mid-setup for the READ(10) bulk sector transfer. Stage 2i raised the cap to
// 3000 to capture that transfer's DREG/TC/Transfer/TransComplete handshake.
// Stage 2j (P258) raises it further to 20000: the P257 3000-line trace showed a
// ~13KB DREG transfer burst still monotonically in progress (TC counting down,
// not stuck) when the 3000-line cap was hit — this larger cap aims to capture
// that burst through to actual completion, or to establish that it genuinely
// never completes.
// Stage 2l (P260) switches away from per-byte DREG logging entirely. P259 found
// that immediately after a ~13KB READ(10) completed, the ROM started a new
// transfer with TC=0x09c403 (~624KB) — a byte-by-byte DREG log cannot scale to
// that (raising the cap 500→3000→20000→22500 across P256-P259 was a losing
// race). Instead, DREG (reg 0x15) accesses are now logged only at coarse
// checkpoints: at completion (tc==0) or every 1024 bytes (tc&0x3ff==0). Every
// non-DREG register access (SCMD/PSNS/SSTS/INTS/TEMP/TCx/PCTL etc.) still logs
// unconditionally — those are low-frequency and mark the important events
// (SELECT, command setup, phase transitions, completion handshake). This
// reduces debug.log I/O, so the cap comes back down to 5000.
// To be removed once the READ(10) bulk-transfer path is understood (CLAUDE.md
// rule 3).
static int s_p256_trace_count = 0;
#define P256_TRACE_MAX 5000

static inline bool p256_should_log_dreg(uint32_t tc) {
	// Coarse checkpoint: log DREG only at completion (tc==0) or every 1024
	// bytes, instead of every single byte — a byte-by-byte log cannot scale
	// to transfers in the hundreds-of-KB range (P259 found one at ~624KB).
	return tc == 0 || (tc & 0x3ff) == 0;
}

extern "C" uint8_t scsi_real_read(uint32_t addr) {
	if (!s_scsi_instance) return 0xff;
	Event::DrainPending();                 // P264: 保留中の再アーム選択タイムアウトを発火
	uint8_t val = (uint8_t)s_scsi_instance->ReadByte(addr);
	if ((addr & 0x1f) == 0x15) StatBar_HDD(1);   /* P269: DREG転送 = HDDランプbusy。トレース上限と無関係に常時発火 */
	if (s_p256_trace_count < P256_TRACE_MAX) {
		bool is_dreg = ((addr & 0x1f) == 0x15);
		SCSI::scsi_t st;
		s_scsi_instance->GetSCSI(&st);
		if (!is_dreg || p256_should_log_dreg((uint32_t)st.tc)) {
			/* P529: インクリメントは debug_log() の引数から分離し、
			 * P256_ENABLE の値に関わらず常時実行する(上限判定を維持)。 */
			int p256_n = ++s_p256_trace_count;
#if P256_ENABLE
			debug_log("[P256-DIAG] R reg=0x%02x val=0x%02x phase=%d req=%d ack=%d "
			          "ints=0x%02x scmd=0x%02x tc=0x%06x trans=%d (n=%d)\n",
			          addr, val, (int)st.phase, st.req, st.ack,
			          (unsigned)st.ints, (unsigned)st.scmd, (unsigned)st.tc, st.trans,
			          p256_n);
			if (p256_n == P256_TRACE_MAX) {
				debug_log("[P256-DIAG] trace cap reached (%d) — suppressing further lines\n",
				          P256_TRACE_MAX);
			}
#endif
		}
	}
	return val;
}

extern "C" void scsi_real_write(uint32_t addr, uint8_t data) {
	if (!s_scsi_instance) return;
	s_scsi_instance->WriteByte(addr, data);
	if ((addr & 0x1f) == 0x15) StatBar_HDD(1);   /* P269: DREG転送 = HDDランプbusy。トレース上限と無関係に常時発火 */
	if (s_p256_trace_count < P256_TRACE_MAX) {
		bool is_dreg = ((addr & 0x1f) == 0x15);
		SCSI::scsi_t st;
		s_scsi_instance->GetSCSI(&st);
		if (!is_dreg || p256_should_log_dreg((uint32_t)st.tc)) {
			/* P529: インクリメントは debug_log() の引数から分離し、
			 * P256_ENABLE の値に関わらず常時実行する(上限判定を維持)。 */
			int p256_n = ++s_p256_trace_count;
#if P256_ENABLE
			debug_log("[P256-DIAG] W reg=0x%02x val=0x%02x phase=%d req=%d ack=%d "
			          "ints=0x%02x scmd=0x%02x tc=0x%06x trans=%d (n=%d)\n",
			          addr, data, (int)st.phase, st.req, st.ack,
			          (unsigned)st.ints, (unsigned)st.scmd, (unsigned)st.tc, st.trans,
			          p256_n);
			if (p256_n == P256_TRACE_MAX) {
				debug_log("[P256-DIAG] trace cap reached (%d) — suppressing further lines\n",
				          P256_TRACE_MAX);
			}
#endif
		}
	}
}

// P253: attach an internal-SCSI HD image path to the given ID. Delegated from
// the C bridge; the actual disk Open happens inside Construct() (via Reset()).
extern "C" void scsi_real_set_disk_path(int id, const char* path) {
	if (s_scsi_instance) s_scsi_instance->SetDiskPath(id, path);
}

// P674: MO のライブ挿入。0=即時反映 / -1=SPC未構築 / -2=MOドライブ未装着
// / -3=ゲストがロック中(PREVENT ALLOW MEDIUM REMOVAL) / -4=オープン失敗。
extern "C" int scsi_real_mo_open(const char* path) {
	if (!s_scsi_instance) return -1;
	if (!s_scsi_instance->IsValid(TRUE)) return -2;
	if (s_scsi_instance->IsLocked(TRUE)) return -3;
	Filepath fp;
	fp.SetPath(path ? path : "");
	int r = s_scsi_instance->Open(fp, TRUE) ? 0 : -4;
	debug_log("[P674-MO] open: r=%d ready=%d path=%s\n", r,
	          s_scsi_instance->IsReady(TRUE) ? 1 : 0, path ? path : "");
	return r;
}

// P674: MO のライブイジェクト。0=成功 / -1=SPC未構築 / -2=MOドライブ未装着
// / -3=ゲストがロック中(force==0の場合のみ) / -4=イジェクト後もreadyのまま(異常)。
extern "C" int scsi_real_mo_eject(int force) {
	if (!s_scsi_instance) return -1;
	if (!s_scsi_instance->IsValid(TRUE)) return -2;
	if (!force && s_scsi_instance->IsLocked(TRUE)) return -3;
	s_scsi_instance->Eject(force ? TRUE : FALSE, TRUE);
	int r = s_scsi_instance->IsReady(TRUE) ? -4 : 0;
	debug_log("[P674-MO] eject: force=%d r=%d\n", force, r);
	return r;
}

// P676: CD-ROM のライブ挿入。戻り値の意味論は P674 の MO と同一。
// 0=即時反映 / -1=SPC未構築 / -2=CDドライブ未装着
// / -3=ゲストがロック中(PREVENT ALLOW MEDIUM REMOVAL) / -4=オープン失敗。
// ★第2引数 FALSE = CD(XM6 の mo フラグ意味論: TRUE→MO / FALSE→CD)。
extern "C" int scsi_real_cd_open(const char* path) {
	if (!s_scsi_instance) return -1;
	if (!s_scsi_instance->IsValid(FALSE)) return -2;
	if (s_scsi_instance->IsLocked(FALSE)) return -3;
	Filepath fp;
	fp.SetPath(path ? path : "");
	int r = s_scsi_instance->Open(fp, FALSE) ? 0 : -4;
	debug_log("[P676-CD] open: r=%d ready=%d path=%s\n", r,
	          s_scsi_instance->IsReady(FALSE) ? 1 : 0, path ? path : "");
	return r;
}

// P676: CD-ROM のライブイジェクト。0=成功 / -1=SPC未構築 / -2=CDドライブ未装着
// / -3=ゲストがロック中(force==0の場合のみ) / -4=イジェクト後もreadyのまま(異常)。
extern "C" int scsi_real_cd_eject(int force) {
	if (!s_scsi_instance) return -1;
	if (!s_scsi_instance->IsValid(FALSE)) return -2;
	if (!force && s_scsi_instance->IsLocked(FALSE)) return -3;
	s_scsi_instance->Eject(force ? TRUE : FALSE, FALSE);
	int r = s_scsi_instance->IsReady(FALSE) ? -4 : 0;
	debug_log("[P676-CD] eject: force=%d r=%d\n", force, r);
	return r;
}

// P252 Stage 2c-2: level-1 interrupt-ACK delegation. The Bridge multiplexer
// (m68000_bridge.c::mx68k_diag_irqh_callback) calls this when the internal
// SCSI is the interrupting device, so SCSI::IntAck() performs its own
// post-ACK bookkeeping (scsi.vector cleanup, etc.). Same extern "C" wrapper
// pattern as scsi_real_read / scsi_real_write above.
extern "C" void scsi_real_int_ack(int level) {
	if (!s_scsi_instance) return;
	s_scsi_instance->IntAck(level);
}
