//---------------------------------------------------------------------------
//
//	MX68K — SCSI/SASI ディスク移植用互換シム
//
//	本ヘッダは、XM6 (vm/disk.cpp, vm/disk.h) から MX68K Bridge 層へ移植した
//	ディスクエミュレーションクラスをコンパイルするのに必要な、最小限の
//	Win32 / XM6 環境の型とマクロを提供する。
//
//	MX68K はオープンソースのシャープ X68000 エミュレータ px68k の macOS 移植である。
//	ここでの "SASI" / "SCSI" は X68000 ハードウェアが用いる 1980 年代の
//	ディスクインタフェース規格名であり、通常のエミュレータ開発である。
//
//	移植したディスククラスの出典:
//	  X68000 EMULATOR "XM6"
//	  Copyright (C) 2001-2006 PI.(Twitter: @xm6_original)
//	リポジトリ直下の NOTICE-THIRD-PARTY.md を参照のこと。
//
//---------------------------------------------------------------------------

#if !defined(scsi_compat_shim_h)
#define scsi_compat_shim_h

#include <cstdint>
#include <cassert>
#include <cstddef>

//---------------------------------------------------------------------------
//
//	XM6 ディスクコードが使用する Win32 の基本整数型。
//
//	px68k の win32api/windows.h は BYTE/WORD/DWORD をコメントアウトしたままで、
//	BOOL/FASTCALL/TRUE/FALSE しか提供しない。そのため整数型はここで定義する。
//	同一翻訳単位で px68k のヘッダも(dosio.h 経由で)インクルードされる場合でも、
//	以下の typedef は px68k のもの(BOOL == int)と同一なので再定義は合法であり、
//	マクロは #ifndef でガードしているため衝突しない。
//
//---------------------------------------------------------------------------
typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;

#if !defined(_WIN32)
typedef int BOOL;
#endif

#ifndef FASTCALL
#define FASTCALL
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

//---------------------------------------------------------------------------
//
//	ASSERT / MAKEID — 移植コードが同一に振る舞うよう、XM6 vm/xm6.h から
//	そのまま複製したもの。
//	  xm6.h: #define ASSERT(cond) assert(cond)
//	  xm6.h: #define MAKEID(a, b, c, d) ((DWORD)((a<<24) | (b<<16) | (c<<8) | d))
//
//---------------------------------------------------------------------------
#if !defined(ASSERT)
#define ASSERT(cond) assert(cond)
#endif

#if !defined(MAKEID)
#define MAKEID(a, b, c, d) ((DWORD)((a << 24) | (b << 16) | (c << 8) | d))
#endif

//---------------------------------------------------------------------------
//
//	ASSERT_DIAG / LOG マクロ — XM6 vm/xm6.h + vm/log.h から、NO_LOG / NDEBUG
//	(リリース)形で複製したもの。
//
//	これらを一切使わない vm/disk.cpp と異なり、vm/scsi.cpp はほぼ全メソッドの
//	入口で ASSERT_DIAG() を呼び、多くのレジスタセッタ/フェーズ遷移ルーチンでは
//	#if SCSI_LOG ガードの外で LOG0..LOG4 を呼ぶ。XM6 では NO_LOG / NDEBUG 定義時に
//	これらは ((void)0) へ展開される。マクロ引数(Log::Warning 等)は置換リストに
//	現れないため、プリプロセッサが単に捨て去り、Log クラスは不要となる。
//	これにより移植コードを無改変のままコンパイルできる。
//
//---------------------------------------------------------------------------
#if !defined(ASSERT_DIAG)
#define ASSERT_DIAG() ((void)0)
#endif

#if !defined(LOG0)
#define LOG0(l, s)              ((void)0)
#define LOG1(l, s, a)           ((void)0)
#define LOG2(l, s, a, b)        ((void)0)
#define LOG3(l, s, a, b, c)     ((void)0)
#define LOG4(l, s, a, b, c, d)  ((void)0)
#endif

//---------------------------------------------------------------------------
//
//	前方宣言。
//
//	Device::Callback(Event*) および MemDevice の cpu/scheduler メンバは
//	これらの型をポインタ経由でしか参照しないため、ここでは前方宣言で足りる。
//	完全な定義は後方に続く。
//
//---------------------------------------------------------------------------
class VM;
class Event;
class CPU;
class Scheduler;
class Memory;
class SRAM;
class Config;

//---------------------------------------------------------------------------
//
//	最小限の Device / VM スタブ。
//
//	XM6 の Disk 基底クラスはコントローラポインタ "Device *ctrl" を保持し、
//	SCSIHD::Inquiry は SCSI INQUIRY のリビジョン欄を埋めるために
//	  ctrl->GetVM()->GetVersion(major, minor)
//	経由でエミュレータのバージョンを読む。
//
//	SCSI (MB89352) の移植ではさらに、MemDevice 基底(アドレス窓 + cpu/scheduler
//	の参照解決)と、SPC ステートマシンが行う約 12 種の外部呼び出し
//	(cpu->BusErr/IntCancel/Interrupt, scheduler->Wait/Has/Add/DelEvent,
//	memory->GetMemType/GetSCSI, sram->Get/SetMemSw)を満たす軽量な
//	CPU / Scheduler / Event / Memory / SRAM アダプタが必要となる。これらの
//	アダプタは実際の仮想マシン動作を一切実装せず、移植したプロトコルロジックを
//	コンパイル可能にするためだけに存在する。この翻訳単位のものはまだ何も
//	ランタイムへ配線されていない — SCSI クラスは一度もインスタンス化されない。
//
//---------------------------------------------------------------------------
class VM {
public:
	// 仮のバージョン (major.minor)。実際の値は後の段階で配線する。
	void GetVersion(DWORD& major, DWORD& minor) { major = 1; minor = 0; }
	// 要求されたデバイススタブ(CPU/Scheduler/Memory/SRAM)を返す。定義は
	// 後方のそれらのクラスの後にある。
	void* SearchDevice(DWORD id);
};

class Device {
public:
	// デバイス識別情報 (XM6 device.h)。移植したコンストラクタが設定する。
	struct { DWORD id = 0; const char* desc = nullptr; } dev;

	Device() {}
	virtual ~Device() {}

	void SetVM(VM* vm) { vm_ = vm; }
	VM* GetVM() { static VM s_dummy; return vm_ ? vm_ : &s_dummy; }

	// イベントコールバック(移植した SCSI クラスがオーバーライドする)。
	virtual BOOL Callback(Event* ev) { (void)ev; return TRUE; }

private:
	VM* vm_ = nullptr;
};

//---------------------------------------------------------------------------
//
//	MemDevice — メモリマップドデバイスの基底 (XM6 device.h MemDevice)。
//	[first,last] のゲストアドレス窓を保持し、Init() で VM から
//	CPU / Scheduler スタブを解決する。
//
//---------------------------------------------------------------------------
class MemDevice : public Device {
public:
	struct { DWORD first = 0; DWORD last = 0; } memdev;
	VM* vm = nullptr;
	CPU* cpu = nullptr;
	Scheduler* scheduler = nullptr;

	MemDevice(VM* p) : vm(p) {}
	virtual ~MemDevice() {}

	virtual BOOL Init();
	virtual void Cleanup() {}

	virtual DWORD ReadByte(DWORD addr) { (void)addr; return 0xff; }
	virtual DWORD ReadWord(DWORD addr) { return (ReadByte(addr) << 8) | ReadByte(addr + 1); }
	virtual void WriteByte(DWORD addr, DWORD data) { (void)addr; (void)data; }
	virtual void WriteWord(DWORD addr, DWORD data) { WriteByte(addr, (data >> 8) & 0xff); WriteByte(addr + 1, data & 0xff); }
	virtual DWORD ReadOnly(DWORD addr) const { (void)addr; return 0xff; }
};

//---------------------------------------------------------------------------
//
//	CPU スタブ — 割り込み/バスエラーの窓口 (XM6 cpu.h)。
//
//	P252 Stage 2c-2: Interrupt / IntCancel は Bridge 層のレベル1割り込み
//	マルチプレクサ (m68000_bridge.c) へ配線済み。IntAck は何もしないまま —
//	ACK はこのスタブではなくマルチプレクサ側 (mx68k_diag_irqh_callback) から
//	駆動される。経路全体は既定では不活性である: SCSI クラスが Interrupt(1,...) を
//	呼ぶのはゲストが SPC の有効ビットを立てた後だけであり、配線トグルが
//	オフの間はそれが起こり得ない (P251)。
//
//---------------------------------------------------------------------------
extern "C" void mx68k_scsi_irq_raise(int level, int vector);
extern "C" void mx68k_scsi_irq_cancel(int level);
extern "C" void mx68k_scsi_defer_stale(void);   // P264: 未ドレイン pending を supersede した際の診断

class CPU {
public:
	BOOL Interrupt(int level, int vector) { mx68k_scsi_irq_raise(level, vector); return TRUE; }
	void IntAck(int level) { (void)level; }   /* ACK はマルチプレクサ側から駆動される。空のままにする */
	void IntCancel(int level) { mx68k_scsi_irq_cancel(level); }
	void BusErr(DWORD addr, BOOL read) { (void)addr; (void)read; }
};

//---------------------------------------------------------------------------
//
//	Event スタブ — スケジューライベント記述子 (XM6 event.h)。
//
//---------------------------------------------------------------------------
class Event {
public:
	void SetDevice(Device* p) { device_ = p; }
	Device* GetDevice() const { return device_; }
	void SetDesc(const char* d) { desc_ = d; }
	void SetUser(DWORD data) { user_ = data; }
	DWORD GetUser() const { return user_; }
	void SetTime(DWORD hus) {
		time_ = hus;
		if (hus > 0 && device_) {
			// P264: SetTCL 由来の再アーム選択タイムアウトのみ、この場では発火させず
			// 保留する。ROM は最初のタイムアウトを ack した「後」で INTS をリードするため、
			// 発火は次のレジスタ・リード時(DrainPending)まで遅延させる必要がある。
			// それ以外の全経路(初回選択・成功選択・CD-DA・Reset)は arm されないので即時発火(従来通り)。
			if (s_defer_next_fire) {
				s_defer_next_fire = false;   // ワンショットを消費
				// P264 レビュー4.1対策: 未ドレインの pending が残ったまま新たに arm される
				// (＝より新しい再アームが古いものを追い越す)ケースを構造的に可視化する。
				// 実 HW でも新しい SEL は古い選択タイマを取り消すので、古い pending は破棄が正。
				if (s_pending_dev) { mx68k_scsi_defer_stale(); }
				s_pending_dev = device_;     // 上書き＝古い pending を破棄(replay しない)
				s_pending_ev  = this;
			} else {
				// P264 レビュー4.1対策: 同期発火(初回/成功選択)の直前に未ドレインの pending が
				// 残っていた場合、新たな選択が古い再アーム選択タイムアウトを supersede する。
				// 古い pending の Callback を「後で」replay させると Interrupt(4)/tc デクリメント/
				// MsgOut・Command を無関係な state に対して二重発火してしまう(レビュー指摘の実害)。
				// 実 HW と同じく「新しい選択が古い選択タイマを取り消す」= 破棄 が正しい。
				// 破棄は診断ログで可視化(サイレントに握り潰さない)。本 ROM の線形フローでは発生しないが
				// 構造的ガードとして常時有効。
				if (s_pending_dev) {
					mx68k_scsi_defer_stale();
					s_pending_dev = nullptr;
					s_pending_ev  = nullptr;
				}
				device_->Callback(this);
			}
		}
	}
	DWORD GetTime() const { return time_; }
	DWORD GetRemain() const { return 0; }

	// --- P264 遅延発火制御 ---
	static void ArmDefer()    { s_defer_next_fire = true; }
	static void DisarmDefer() { s_defer_next_fire = false; }   // §3.4 参照(現状到達不能)
	static bool HasPending()  { return s_pending_dev != nullptr; }
	static void DrainPending() {
		if (s_pending_dev && s_pending_ev) {
			Device* d = s_pending_dev; Event* e = s_pending_ev;
			s_pending_dev = nullptr; s_pending_ev = nullptr;  // 再入防止のため先にクリア
			d->Callback(e);
		}
	}
	// P275: teardown 時に static pending 状態を明示クリア(stale ポインタを残さない)。
	static void ResetDeferState() {
		s_defer_next_fire = false;
		s_pending_dev = nullptr;
		s_pending_ev = nullptr;
	}
private:
	Device* device_ = nullptr;
	const char* desc_ = nullptr;
	DWORD user_ = 0;
	DWORD time_ = 0;
	static bool    s_defer_next_fire;   // 定義は scsi_spc_bridge.cpp
	static Device* s_pending_dev;
	static Event*  s_pending_ev;
};

//---------------------------------------------------------------------------
//
//	Scheduler スタブ — イベントキュー (XM6 schedule.h)。
//
//	AddEvent はコールバックを同期的に発火させることで選択フェーズの遅延を
//	近似する (P250 計画 §2-5 参照)。これは単体テスト専用の挙動であり、
//	SCSI クラスを実際に配線する際 (Stage 2c) に実際の CPU サイクル進行と
//	照らして再評価しなければならない。
//
//---------------------------------------------------------------------------
class Scheduler {
public:
	void Wait(DWORD cycle) { (void)cycle; }
	void AddEvent(Event* event) {
		if (event && event->GetDevice()) {
			event->GetDevice()->Callback(event);
		}
	}
	void DelEvent(Event* event) { (void)event; }
	BOOL HasEvent(Event* event) const { (void)event; return FALSE; }
};

//---------------------------------------------------------------------------
//
//	Memory スタブ — X68000 のメモリ構成 (XM6 memory.h)。
//
//	P251 Stage 2c: GetMemType / GetSCSI は実際の Bridge データに裏付けられている。
//	定義は scsi_spc_bridge.cpp にある(機種は scsi_real_set_scsi_mode 経由で
//	渡される (P510)。SCSI IPL ROM = EmulatorBridge.c の s_scsi_in_rom で、
//	P247 で配線)。
//
//---------------------------------------------------------------------------
class Memory {
public:
	enum memtype { None, SASI, SCSIInt, SCSIExt, XVI, Compact, X68030 };
	memtype GetMemType() const;
	const BYTE* GetSCSI() const;
};

//---------------------------------------------------------------------------
//
//	SRAM スタブ — バッテリバックアップされたメモリスイッチ (XM6 sram.h)。
//
//	P251 Stage 2c: GetMemSw / SetMemSw は実際の px68k sram.c に裏付けられている
//	(定義は scsi_spc_bridge.cpp)。SetMemSw はユーザーの sram.dat の
//	メモリスイッチ領域 ($ED0000+offset) へ、前後を括った実際の 1 バイト書き込みを
//	行う。到達するのは内蔵 SCSI の二重ゲート(機種==SCSI かつ SCSIINROM 読込済み)
//	を満たし、かつ sram.dat の一回限りのバックアップを取得済みの場合のみ
//	(Fable5 監査 #2)。ROM 起動ハンドル $ED000C-0F まで書き換えてしまう
//	SRAM_SetSCSIMode(2) は意図的に一切経由しない。
//
//	P508 更新: $ED000C 書き換えの全面禁止は失効した — その前提
//	(D-18「起動ハンドルの先にマップされていない SCSI IPL があると起動が壊れる」)は
//	P247-P267 で解決済みであり、P508 は配線済み SCSI 構成と ROM 起動ハンドルを
//	同期させるため、sasi_bridge.c (sasi_bridge_apply_rom_boot_handle) から
//	意図的に $ED000C-0F を書き込むようになった。残しているのは禁止ではなく
//	*設計* 上のルールである: SRAM_SetSCSIMode() は経由しないこと。その単一の
//	mode 引数では、引数の取り違えによって起動ハンドルを誤って書き換え得るため、
//	引数を取り違えようのない専用関数を使う (P450/P456 と同じ作法)。Reset() が
//	$ED000C への書き込みを求めることはないので、構造上この経路は依然として
//	そこに触れ得ない。
//
//---------------------------------------------------------------------------
class SRAM {
public:
	DWORD GetMemSw(DWORD offset) const;
	void SetMemSw(DWORD offset, DWORD data);
};

//---------------------------------------------------------------------------
//
//	VM::SearchDevice / MemDevice::Init — 上のスタブデバイスクラス群が
//	出そろったので、ここで定義する。
//
//---------------------------------------------------------------------------
inline void* VM::SearchDevice(DWORD id) {
	static CPU s_cpu;
	static Scheduler s_sched;
	static Memory s_mem;
	static SRAM s_sram;
	switch (id) {
		case MAKEID('C', 'P', 'U', ' '): return &s_cpu;
		case MAKEID('S', 'C', 'H', 'E'): return &s_sched;
		case MAKEID('M', 'E', 'M', ' '): return &s_mem;
		case MAKEID('S', 'R', 'A', 'M'): return &s_sram;
		default: return nullptr;
	}
}

inline BOOL MemDevice::Init() {
	cpu = (CPU*)vm->SearchDevice(MAKEID('C', 'P', 'U', ' '));
	scheduler = (Scheduler*)vm->SearchDevice(MAKEID('S', 'C', 'H', 'E'));
	return TRUE;
}

#endif	// scsi_compat_shim_h
