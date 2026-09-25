//---------------------------------------------------------------------------
//
//	MX68K — 内蔵SCSI(MB89352 SPC)ランタイムブリッジ  (P251 Stage 2c)
//
//	MX68K はオープンソースのシャープ X68000 エミュレータ px68k の macOS 移植版。
//	ここでいう「SASI」/「SCSI」/「SPC」は、X68000 ハードウェアが使う
//	1980年代のディスクインタフェース規格名/チップ名であり、これは通常の
//	エミュレータ開発である。
//
//	この翻訳単位は、XM6(scsi_spc.cpp)から移植した唯一の静的 SCSI(MB89352)
//	インスタンスを所有し、C ブリッジ(scsi_in_bridge.c)から駆動できるよう
//	小さな extern "C" インタフェースを公開する。また scsi_compat_shim.h で
//	宣言された Memory / SRAM 互換スタブの実体も
//	提供する。
//
//	適用範囲の注記(P251): 割込みは無効のまま(shim 内の CPU::Interrupt は no-op)
//	で、ディスクイメージも接続しない(scsihd[] のパスは空なので
//	scsi.disk[*] == nullptr)。配線するのはレジスタ/フェーズの状態機械
//	のみである。
//
//	リポジトリ直下の NOTICE-THIRD-PARTY.md を参照。
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
//	px68k コア / EmulatorBridge.c 側の C シンボル。C++ リンカが名前修飾
//	なしで解決できるよう、ここで extern "C" として宣言する。
//---------------------------------------------------------------------------
extern "C" {
	// px68k の SRAM(Core/px68k/x68k/sram.c)— 実際のバッテリバックアップメモリ。
	uint8_t SRAM_Read(uint32_t adr);
	void    SRAM_Write(uint32_t adr, uint8_t data);
	// px68k のシステムポート書込(Memory_WriteB == cpu_writemem24)。SRAM
	// アクセス許可ゲート($E8E00D)で前後を挟む用途にのみ使う。px68k 自身の
	// SRAM_SetSASIDrive / SRAM_SetSCSIMode と同じ定石。
	void    cpu_writemem24(uint32_t adr, uint32_t data);
	// EmulatorBridge.c が保持する内蔵SCSI IPL ROM バッファ(P247)。
	extern uint8_t s_scsi_in_rom[];
	// 最初の実 SRAM 書込の前に 1 度だけ取る sram.dat のバックアップ。
	void    mx68k_backup_sram_before_scsi_wiring(void);
	void    debug_log(const char* fmt, ...);
	// EmulatorBridge.c が保持する内蔵SCSI HD ディスクイメージのパス(P253)。
	const char* mx68k_get_scsi_in_disk_path(int id);
	// P269: ステータスバーの HDD busy パルス(status_bridge.c)。DREG データ
	// 転送時に、P256 診断トレースの上限とは無関係に発火する。
	void    StatBar_HDD(int32_t sw);
	// P450: 「メモリスイッチ自動更新」設定値(EmulatorBridge.c)。SetMemSw の
	// 第2層ゲートで、書込のその瞬間の設定を読むために使う。
	bool    mx68k_get_memsw_auto_update(void);
}

//---------------------------------------------------------------------------
//	mx68k_reset_hard() から受け渡される SCSI 構成モード。導出元のグローバル
//	(g_machine_type, g_scsi_ext_board_installed, s_scsi_ext_rom_loaded)は
//	EmulatorBridge.c 内で内部リンケージを持ち extern できないため、直接共有
//	するのではなく、導出済みの値を C 境界越しに渡す(scsi_in_bridge.c の
//	install 引数と同じ前例)。
//
//	P510: 以前は scsi_real_set_machine_type(int) で、SASI と内蔵SCSI しか
//	区別せず、しかも scsi_in_bridge.c のゲート成立分岐の中からしか呼ばれ
//	なかった — そのため SASI 機では前回リセット時の値が残ったままになっていた
//	(P447/D-31 の一方向書込欠陥と同じ形)。現在は mx68k_reset_hard() 内の
//	唯一の呼出し箇所から、ハードリセットごとに無条件でちょうど 1 回、3 つの
//	P510_SCSI_MODE_* 値のいずれかに設定される。
//	既定値 INT は P510 以前の初期値(4 == SCSI)を維持するためのもの。
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
//	Memory / SRAM 互換スタブの実体(宣言は scsi_compat_shim.h)。
//---------------------------------------------------------------------------
Memory::memtype Memory::GetMemType() const {
	// SCSI::Reset() はこれらをそれぞれ scsi.type 0 / 1 / 2 へ対応付ける
	// (scsi_spc.cpp:183-201)。
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
	// SCSI::Reset() が書き込むメモリスイッチのバイト列(ゲスト $ED006F/70/71)
	// をそのまま忠実に再現し、各書込を $E8E00D のアクセス許可トグルで前後から
	// 挟む — px68k の SRAM_SetSASIDrive と同じ 1 回書込の定石。
	// あえて SRAM_SetSCSIMode(2) は経由しない: あのヘルパーは ROM 起動ハンドル
	// $ED000C-0F まで書き換えてしまうため。
	//
	// P508 での更新: $ED000C 自体の書換禁止(P251 計画 §9、D-18: 起動ハンドル
	// を未マップの SCSI IPL へ向けると起動が壊れる)は前提を失った — D-18 は
	// P247-P267 で解決済みで、P508 は現在 sasi_bridge.c
	// (sasi_bridge_apply_rom_boot_handle)から意図的に $ED000C-0F を書き込み、
	// ROM 起動ハンドルが配線済みの SCSI 構成に追従するようにしている。残して
	// いるのは禁止ではなく設計規則のほう: SRAM_SetSCSIMode() は決して経由しない。
	// 単一のモード引数しか持たないため、引数の取り違えで起動ハンドルを誤って
	// 書き換えうるからだ — 取り違えようのない引数を持つ専用関数を使う
	// (P450/P456 の定石)。
	// Reset() が $ED000C の書込を要求することは無いので、構造上この経路は
	// 依然としてそこに触れられない。
	//
	// P450 第2層ゲート: 「メモリスイッチ自動更新」が OFF なら SRAM に一切
	// 触れない。第1層(SCSI::Init() の scsi.memsw)は Init 時点の値を 1 度だけ
	// キャッシュするため、その鮮度は「毎ハードリセットで Init が再実行される」
	// という P450 (E) の変更に依存する。この層は書込のその瞬間に設定を読むので、
	// (E) が将来変更・revert されても OFF の約束が破れない。異なる失敗モードを
	// 塞ぐ 2 枚であり、同一原因の二重ガードではない。
	if (!mx68k_get_memsw_auto_update()) return;
	cpu_writemem24(0x00e8e00du, 0x31);                 // SRAM アクセスを許可
	SRAM_Write(0x00ed0000u + offset, (uint8_t)data);
	cpu_writemem24(0x00e8e00du, 0x55);                 // SRAM アクセスを禁止
}

//---------------------------------------------------------------------------
//	静的 SCSI(MB89352)インスタンス + extern "C" ブリッジインタフェース。
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

// 内蔵SCSI SPC を(1 度だけ)構築する。SRAM 書込が起こりうるより *前* に、
// sram.dat のバックアップを 1 度だけ取る(SCSI::Reset() はメモリスイッチを書き込む)。
extern "C" void scsi_real_install_construct(void) {
	if (s_scsi_instance) return;
	mx68k_backup_sram_before_scsi_wiring();
	s_scsi_instance = new SCSI(&s_vm);
	s_scsi_instance->Init();
	// P274: HD イメージのパス(ID0-6)を Reset() より *前* に取り込む。これで
	// 下の Reset() が呼び出す Construct() が空でない scsihd[id] を参照し、
	// 実ディスクを開ける(scsi.disk[id] != nullptr)。
	// P510: 取得元は構成モードによって異なる — 外付けの CZ-6BS1 は
	// イメージを内蔵機とは別の配列 Config.SCSIEXHDImage[] に
	// 保持している。
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
	// Reset() はメモリスイッチを書き込んだ直後に内部で Construct() を呼ぶので、
	// private な Construct() をこちらから呼ぶことはしない(そもそも
	// 呼べない)。
	s_scsi_instance->Reset();       // Construct() + メモリスイッチ書込はここで 1 回だけ行われる
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

// レジスタ読み書き。オフセットは scsi_in_bridge.c の scsi_in_dispatch_read/write
// で既に 0x00-0x1f へ正規化されている(ゲスト $E96020-3F から 0x20 を引いた値)
// ので、ReadByte/WriteByte はレジスタデコードに到達し、ROM 分岐
// (addr >= 0x20)や外部ウィンドウのバスエラーガードには決して入らない。
// P256 Stage 2g / P257 Stage 2i: DATA-IN フェーズの一時的なレジスタトレース。
// 診断専用 — 読み書きの結果や SCSI 状態は一切変更しない。
// Stage 2g の上限 500 行は TUR/REQUEST SENSE/READ CAPACITY まではカバーしたが、
// READ(10) の一括セクタ転送はセットアップ途中で打ち切られた。Stage 2i は上限を
// 3000 に上げ、その転送の DREG/TC/Transfer/TransComplete ハンドシェイクを捕捉した。
// Stage 2j(P258)はさらに 20000 へ上げる: P257 の 3000 行トレースでは、上限
// 3000 行に達した時点で ~13KB の DREG 転送バーストがまだ単調に進行中だった
// (TC はカウントダウン中で、停止していない)— この大きな上限は、そのバーストを
// 実際の完了まで捕捉するか、あるいは本当に完了しないことを確かめることを
// 狙ったもの。
// Stage 2l(P260)ではバイト単位の DREG ログを完全にやめる。P259 で、~13KB の
// READ(10) が完了した直後に ROM が TC=0x09c403(~624KB)の新たな転送を開始する
// ことが判明した — バイト単位の DREG ログではこれに追いつけない(P256-P259 で
// 上限を 500→3000→20000→22500 と上げ続けたのは勝ち目のない競争だった)。
// 代わりに、DREG(reg 0x15)へのアクセスは粗いチェックポイントでのみ記録する:
// 完了時(tc==0)または 1024 バイトごと(tc&0x3ff==0)。DREG 以外のレジスタ
// アクセス(SCMD/PSNS/SSTS/INTS/TEMP/TCx/PCTL 等)は引き続き無条件で記録する
// — これらは低頻度で、重要なイベント(SELECT、コマンドのセットアップ、
// フェーズ遷移、完了ハンドシェイク)の目印になる。これにより debug.log の I/O
// が減るので、上限は 5000 まで下げ戻す。
// READ(10) の一括転送経路が解明されたら削除する(CLAUDE.md
// ルール 3)。
static int s_p256_trace_count = 0;
#define P256_TRACE_MAX 5000

static inline bool p256_should_log_dreg(uint32_t tc) {
	// 粗いチェックポイント: DREG は 1 バイトごとではなく、完了時(tc==0)または
	// 1024 バイトごとにのみ記録する — バイト単位のログでは数百 KB 規模の転送
	// に追いつけない(P259 で ~624KB の転送が見つかった)。
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

// P253: 指定 ID に内蔵SCSI HD イメージのパスを設定する。C ブリッジから委譲
// される。実際のディスク Open は(Reset() 経由の)Construct() 内で行われる。
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

// P252 Stage 2c-2: レベル 1 割込み ACK の委譲。Bridge のマルチプレクサ
// (m68000_bridge.c::mx68k_diag_irqh_callback)は、割込み元が内蔵SCSI の
// ときにこれを呼ぶ。これにより SCSI::IntAck() が自前の ACK 後処理
// (scsi.vector の後始末など)を行える。上の scsi_real_read / scsi_real_write
// と同じ extern "C" ラッパーのパターン。
extern "C" void scsi_real_int_ack(int level) {
	if (!s_scsi_instance) return;
	s_scsi_instance->IntAck(level);
}
