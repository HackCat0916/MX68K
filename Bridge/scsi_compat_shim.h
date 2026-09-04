//---------------------------------------------------------------------------
//
//	MX68K — SCSI/SASI disk port compatibility shim
//
//	This header provides the minimal Win32 / XM6 environment types and
//	macros required to compile the disk emulation classes ported from
//	XM6 (vm/disk.cpp, vm/disk.h) into the MX68K Bridge layer.
//
//	MX68K is a macOS port of the open-source Sharp X68000 emulator px68k.
//	"SASI" / "SCSI" here are the 1980s disk-interface standard names used
//	by the X68000 hardware — this is ordinary emulator development.
//
//	The ported disk classes originate from:
//	  X68000 EMULATOR "XM6"
//	  Copyright (C) 2001-2006 PI.(ytanaka@ipc-tokai.or.jp)
//	See NOTICE-THIRD-PARTY.md at the repository root.
//
//---------------------------------------------------------------------------

#if !defined(scsi_compat_shim_h)
#define scsi_compat_shim_h

#include <cstdint>
#include <cassert>
#include <cstddef>

//---------------------------------------------------------------------------
//
//	Basic Win32 integer types used by the XM6 disk code.
//
//	px68k's win32api/windows.h leaves BYTE/WORD/DWORD commented out and
//	provides only BOOL/FASTCALL/TRUE/FALSE. We therefore define the integer
//	types here. Where px68k's headers ARE also included in the same
//	translation unit (via dosio.h), the typedefs below are identical to
//	px68k's (BOOL == int) so the redefinition is legal, and the macros are
//	#ifndef-guarded so they never clash.
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
//	ASSERT / MAKEID — replicated verbatim from XM6 vm/xm6.h so the ported
//	code behaves identically.
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
//	ASSERT_DIAG / LOG macros — replicated from XM6 vm/xm6.h + vm/log.h in
//	their NO_LOG / NDEBUG (release) form.
//
//	Unlike vm/disk.cpp (which uses none of these), vm/scsi.cpp calls
//	ASSERT_DIAG() at the entry of nearly every method and LOG0..LOG4 in many
//	register setters / phase-transition routines outside any #if SCSI_LOG
//	guard. In XM6 these expand to ((void)0) when NO_LOG / NDEBUG is defined;
//	the macro arguments (Log::Warning, etc.) never appear in the replacement
//	list, so the preprocessor simply discards them and no Log class is
//	needed. This lets the ported code be compiled verbatim.
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
//	Forward declarations.
//
//	Device::Callback(Event*) and the MemDevice cpu/scheduler members refer to
//	these types only through pointers, so a forward declaration is enough
//	here; the full definitions follow further down.
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
//	Minimal Device / VM stubs.
//
//	XM6's Disk base class holds a "Device *ctrl" controller pointer, and
//	SCSIHD::Inquiry reads the emulator version through
//	  ctrl->GetVM()->GetVersion(major, minor)
//	to fill the SCSI INQUIRY revision field.
//
//	The SCSI (MB89352) port additionally needs a MemDevice base (address
//	window + cpu/scheduler lookup) and lightweight CPU / Scheduler / Event /
//	Memory / SRAM adapters that satisfy the ~12 external call kinds the SPC
//	state machine makes (cpu->BusErr/IntCancel/Interrupt, scheduler->Wait/
//	Has/Add/DelEvent, memory->GetMemType/GetSCSI, sram->Get/SetMemSw). These
//	adapters implement NO real virtual-machine behaviour; they exist only so
//	the ported protocol logic can compile. Nothing in this translation unit
//	is wired into the runtime yet — the SCSI class is never instantiated.
//
//---------------------------------------------------------------------------
class VM {
public:
	// Placeholder version (major.minor). Real value is wired in a later stage.
	void GetVersion(DWORD& major, DWORD& minor) { major = 1; minor = 0; }
	// Returns the requested device stub (CPU/Scheduler/Memory/SRAM). Defined
	// after those classes below.
	void* SearchDevice(DWORD id);
};

class Device {
public:
	// Device identity (XM6 device.h). Filled by the ported constructor.
	struct { DWORD id = 0; const char* desc = nullptr; } dev;

	Device() {}
	virtual ~Device() {}

	void SetVM(VM* vm) { vm_ = vm; }
	VM* GetVM() { static VM s_dummy; return vm_ ? vm_ : &s_dummy; }

	// Event callback (overridden by the ported SCSI class).
	virtual BOOL Callback(Event* ev) { (void)ev; return TRUE; }

private:
	VM* vm_ = nullptr;
};

//---------------------------------------------------------------------------
//
//	MemDevice — base for a memory-mapped device (XM6 device.h MemDevice).
//	Holds the [first,last] guest-address window and resolves the CPU /
//	Scheduler stubs from the VM at Init().
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
//	CPU stub — interrupt / bus-error surface (XM6 cpu.h).
//
//	P252 Stage 2c-2: Interrupt / IntCancel are now wired to the Bridge-level
//	level-1 interrupt multiplexer (m68000_bridge.c). IntAck stays a no-op —
//	the ACK is driven from the multiplexer side (mx68k_diag_irqh_callback),
//	not from this stub. The whole path is inert by default: the SCSI class
//	only calls Interrupt(1,...) once the guest sets the SPC enable bit, which
//	cannot happen while the wiring toggle is off (P251).
//
//---------------------------------------------------------------------------
extern "C" void mx68k_scsi_irq_raise(int level, int vector);
extern "C" void mx68k_scsi_irq_cancel(int level);
extern "C" void mx68k_scsi_defer_stale(void);   // P264: 未ドレイン pending を supersede した際の診断

class CPU {
public:
	BOOL Interrupt(int level, int vector) { mx68k_scsi_irq_raise(level, vector); return TRUE; }
	void IntAck(int level) { (void)level; }   /* ACK driven from the multiplexer side; keep empty */
	void IntCancel(int level) { mx68k_scsi_irq_cancel(level); }
	void BusErr(DWORD addr, BOOL read) { (void)addr; (void)read; }
};

//---------------------------------------------------------------------------
//
//	Event stub — scheduler event descriptor (XM6 event.h).
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
				s_defer_next_fire = false;   // one-shot 消費
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
//	Scheduler stub — event queue (XM6 schedule.h).
//
//	AddEvent approximates the selection-phase delay by firing the callback
//	synchronously (see P250 plan §2-5). This is a unit-test-only behaviour;
//	it must be re-evaluated against real CPU-cycle progression when the SCSI
//	class is actually wired in (Stage 2c).
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
//	Memory stub — X68000 memory configuration (XM6 memory.h).
//
//	P251 Stage 2c: GetMemType / GetSCSI are now backed by real Bridge data.
//	Their definitions live in scsi_spc_bridge.cpp (machine type threaded in
//	via scsi_real_set_scsi_mode (P510); SCSI IPL ROM = s_scsi_in_rom from
//	EmulatorBridge.c, wired in P247).
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
//	SRAM stub — battery-backed memory switches (XM6 sram.h).
//
//	P251 Stage 2c: GetMemSw / SetMemSw are now backed by the real px68k
//	sram.c (definitions in scsi_spc_bridge.cpp). SetMemSw performs a real,
//	bracketed single-byte write to the user's sram.dat memory-switch region
//	($ED0000+offset). It is reached only when the internal-SCSI double gate
//	(machine==SCSI AND SCSIINROM loaded) is satisfied AND a one-time sram.dat
//	backup has been taken (Fable5 audit #2). It deliberately never routes through
//	SRAM_SetSCSIMode(2), which would also rewrite the ROM boot handle $ED000C-0F.
//
//	P508 update: the blanket ban on rewriting $ED000C has lapsed — its premise
//	(D-18, "an unmapped SCSI IPL behind the boot handle breaks boot") was
//	resolved in P247-P267, and P508 now writes $ED000C-0F deliberately from
//	sasi_bridge.c (sasi_bridge_apply_rom_boot_handle) to keep the ROM boot
//	handle in sync with the wired SCSI configuration. What is retained is the
//	*design* rule, not the ban: never route through SRAM_SetSCSIMode(), whose
//	single mode argument makes an argument mix-up able to rewrite the boot
//	handle by accident — use a dedicated function whose arguments cannot be
//	confused (the same P450/P456 idiom). Reset() never asks us to write
//	$ED000C, so by construction this path still cannot touch it.
//
//---------------------------------------------------------------------------
class SRAM {
public:
	DWORD GetMemSw(DWORD offset) const;
	void SetMemSw(DWORD offset, DWORD data);
};

//---------------------------------------------------------------------------
//
//	VM::SearchDevice / MemDevice::Init — defined here now that the stub
//	device classes above are complete.
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
