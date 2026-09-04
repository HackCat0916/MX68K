//---------------------------------------------------------------------------
//
//	X68000 EMULATOR "XM6"
//
//	Copyright (C) 2001-2006 PI.(ytanaka@ipc-tokai.or.jp)
//	[ Disk ]
//
//---------------------------------------------------------------------------
//
//	Ported to MX68K (macOS port of px68k) — Bridge layer, SCSI/SASI HD + MO
//	+ CD-ROM. Hard-disk classes (DiskTrack / DiskCache / Disk / SASIHD /
//	SCSIHD), the MO class (SCSIMO, P668) and the CD-ROM classes (CDTrack /
//	SCSICD, P676) are ported verbatim from XM6 vm/disk.h with only Win32
//	type/macro shims applied and are wired into the runtime.
//	CDDABuf remains excluded (#if 0) — upstream XM6 itself seals its class
//	body (XM6:vm/disk.h:452-498), and MX implements no CD-DA path.
//	See NOTICE-THIRD-PARTY.md at the repository root.
//
//---------------------------------------------------------------------------

#if !defined(scsi_disk_h)
#define scsi_disk_h

#include "scsi_compat_shim.h"

//---------------------------------------------------------------------------
//
//	Class forward declarations (XM6 original)
//
//---------------------------------------------------------------------------
class DiskTrack;
class DiskCache;
class Disk;
class SASIHD;
class SCSIHD;
class SCSIMO;
class SCSICDTrack;
class SCSICD;

class Fileio;
class Filepath;

//===========================================================================
//
//	File path (minimal MX68K wrapper — replaces XM6 vm/filepath.h)
//
//	Only the interface actually used by the ported disk code is provided:
//	default construction, assignment, path access, and state Save/Load.
//
//===========================================================================
class Filepath
{
public:
	Filepath();
	virtual ~Filepath();
	Filepath& operator=(const Filepath& path);

	void FASTCALL SetPath(const char* path);
	void FASTCALL Clear();
	const char* FASTCALL GetPath() const { return m_szPath; }
	BOOL FASTCALL CmpPath(const Filepath& path) const;

	BOOL FASTCALL Save(Fileio *fio, int ver);
	BOOL FASTCALL Load(Fileio *fio, int ver);

private:
	char m_szPath[520];					// file path (host encoding)
};

//===========================================================================
//
//	File I/O (minimal MX68K wrapper — replaces XM6 vm/fileio.h)
//
//	Thin wrapper delegating to px68k's host file API (File_Open / File_Seek
//	/ File_Read / File_Write / File_Close in Core/px68k dosio). Only the
//	interface used by the ported disk code is provided. The handle is stored
//	as an opaque pointer (px68k FILEH == HANDLE == void*, NULL = invalid) so
//	this header does not need to include px68k headers.
//
//===========================================================================
class Fileio
{
public:
	enum OpenMode {
		ReadOnly,						// read only
		WriteOnly,						// write only
		ReadWrite,						// read / write
		Append							// append
	};

public:
	Fileio();
	virtual ~Fileio();

	BOOL FASTCALL Open(const Filepath& path, OpenMode mode);
	BOOL FASTCALL Seek(long offset);
	BOOL FASTCALL Read(void *buffer, int size);
	BOOL FASTCALL Write(const void *buffer, int size);
	DWORD FASTCALL GetFileSize() const;
	DWORD FASTCALL GetFilePos() const;
	void FASTCALL Close();
	BOOL FASTCALL IsValid() const		{ return (BOOL)(handle != 0); }

private:
	void *handle;						// host file handle (NULL = invalid)
};

//---------------------------------------------------------------------------
//
//	Error definitions (sense codes returned by REQUEST SENSE)
//
//	MSB		reserved (0x00)
//			sense key
//			additional sense code (ASC)
//	LSB		additional sense code qualifier (ASCQ)
//
//---------------------------------------------------------------------------
#define DISK_NOERROR		0x00000000	// NO ADDITIONAL SENSE INFO.
#define DISK_DEVRESET		0x00062900	// POWER ON OR RESET OCCURED
#define DISK_NOTREADY		0x00023a00	// MEDIUM NOT PRESENT
#define DISK_ATTENTION		0x00062800	// MEDIUIM MAY HAVE CHANGED
#define DISK_PREVENT		0x00045302	// MEDIUM REMOVAL PREVENTED
#define DISK_READFAULT		0x00031100	// UNRECOVERED READ ERROR
#define DISK_WRITEFAULT		0x00030300	// PERIPHERAL DEVICE WRITE FAULT
#define DISK_WRITEPROTECT	0x00042700	// WRITE PROTECTED
#define DISK_MISCOMPARE		0x000e1d00	// MISCOMPARE DURING VERIFY
#define DISK_INVALIDCMD		0x00052000	// INVALID COMMAND OPERATION CODE
#define DISK_INVALIDLBA		0x00052100	// LOGICAL BLOCK ADDR. OUT OF RANGE
#define DISK_INVALIDCDB		0x00052400	// INVALID FIELD IN CDB
#define DISK_INVALIDLUN		0x00052500	// LOGICAL UNIT NOT SUPPORTED
#define DISK_INVALIDPRM		0x00052600	// INVALID FIELD IN PARAMETER LIST
#define DISK_INVALIDMSG		0x00054900	// INVALID MESSAGE ERROR
#define DISK_PARAMLEN		0x00051a00	// PARAMETERS LIST LENGTH ERROR
#define DISK_PARAMNOT		0x00052601	// PARAMETERS NOT SUPPORTED
#define DISK_PARAMVALUE		0x00052602	// PARAMETERS VALUE INVALID
#define DISK_PARAMSAVE		0x00053900	// SAVING PARAMETERS NOT SUPPORTED

//===========================================================================
//
//	Disk track
//
//===========================================================================
class DiskTrack
{
public:
	// internal data definition
	typedef struct {
		int track;						// track number
		int size;						// sector size (8 or 9)
		int sectors;					// number of sectors (<=0x100)
		BYTE *buffer;					// data buffer
		BOOL init;						// loaded flag
		BOOL changed;					// changed flag
		BOOL *changemap;				// changed map
		BOOL raw;						// RAW mode
	} disktrk_t;

public:
	// basic functions
	DiskTrack(int track, int size, int sectors, BOOL raw = FALSE);
										// constructor
	virtual ~DiskTrack();
										// destructor
	BOOL FASTCALL Load(const Filepath& path);
										// load
	BOOL FASTCALL Save(const Filepath& path);
										// save

	// read / write
	BOOL FASTCALL Read(BYTE *buf, int sec) const;
										// sector read
	BOOL FASTCALL Write(const BYTE *buf, int sec);
										// sector write

	// misc
	int FASTCALL GetTrack() const		{ return dt.track; }
										// get track
	BOOL FASTCALL IsChanged() const		{ return dt.changed; }
										// changed-flag check

private:
	// internal data
	disktrk_t dt;
										// internal data
};

//===========================================================================
//
//	Disk cache
//
//===========================================================================
class DiskCache
{
public:
	// internal data definition
	typedef struct {
		DiskTrack *disktrk;				// assigned track
		DWORD serial;					// last serial
	} cache_t;

	// number of caches
	enum {
		CacheMax = 16					// number of cached tracks
	};

public:
	// basic functions
	DiskCache(const Filepath& path, int size, int blocks);
										// constructor
	virtual ~DiskCache();
										// destructor
	void FASTCALL SetRawMode(BOOL raw);
										// CD-ROM raw mode setting

	// access
	BOOL FASTCALL Save();
										// save all & release
	BOOL FASTCALL Read(BYTE *buf, int block);
										// sector read
	BOOL FASTCALL Write(const BYTE *buf, int block);
										// sector write
	BOOL FASTCALL GetCache(int index, int& track, DWORD& serial) const;
										// get cache info

private:
	// internal management
	void FASTCALL Clear();
										// clear all tracks
	DiskTrack* FASTCALL Assign(int track);
										// track load
	BOOL FASTCALL Load(int index, int track);
										// track load
	void FASTCALL Update();
										// serial-number update

	// internal data
	cache_t cache[CacheMax];
										// cache management
	DWORD serial;
										// last-access serial number
	Filepath sec_path;
										// path
	int sec_size;
										// sector size (8 or 9 or 11)
	int sec_blocks;
										// number of sector blocks
	BOOL cd_raw;
										// CD-ROM RAW mode
};

//===========================================================================
//
//	Disk
//
//===========================================================================
class Disk
{
public:
	// internal work
	typedef struct {
		DWORD id;						// media ID
		BOOL ready;						// valid disk
		BOOL writep;					// write protected
		BOOL readonly;					// read only
		BOOL removable;					// removable
		BOOL lock;						// locked
		BOOL attn;						// attention
		BOOL reset;						// reset
		int size;						// sector size
		int blocks;						// total sectors
		DWORD lun;						// LUN
		DWORD code;						// status code
		DiskCache *dcache;				// disk cache
	} disk_t;

public:
	// basic functions
	Disk(Device *dev);
										// constructor
	virtual ~Disk();
										// destructor
	virtual void FASTCALL Reset();
										// device reset
	virtual BOOL FASTCALL Save(Fileio *fio, int ver);
										// save
	virtual BOOL FASTCALL Load(Fileio *fio, int ver);
										// load

	// ID
	DWORD FASTCALL GetID() const		{ return disk.id; }
										// get media ID
	BOOL FASTCALL IsNULL() const;
										// NULL check
	BOOL FASTCALL IsSASI() const;
										// SASI check

	// media operations
	virtual BOOL FASTCALL Open(const Filepath& path);
										// open
	void FASTCALL GetPath(Filepath& path) const;
										// get path
	void FASTCALL Eject(BOOL force);
										// eject
	BOOL FASTCALL IsReady() const		{ return disk.ready; }
										// ready check
	void FASTCALL WriteP(BOOL flag);
										// write protect
	BOOL FASTCALL IsWriteP() const		{ return disk.writep; }
										// write-protect check
	BOOL FASTCALL IsReadOnly() const	{ return disk.readonly; }
										// read-only check
	BOOL FASTCALL IsRemovable() const	{ return disk.removable; }
										// removable check
	BOOL FASTCALL IsLocked() const		{ return disk.lock; }
										// lock check
	BOOL FASTCALL IsAttn() const		{ return disk.attn; }
										// change check
	BOOL FASTCALL Flush();
										// cache flush
	void FASTCALL GetDisk(disk_t *buffer) const;
										// get internal work

	// properties
	void FASTCALL SetLUN(DWORD lun)		{ disk.lun = lun; }
										// set LUN
	DWORD FASTCALL GetLUN()				{ return disk.lun; }
										// get LUN

	// commands
	virtual int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf);
										// INQUIRY command
	virtual int FASTCALL RequestSense(const DWORD *cdb, BYTE *buf);
										// REQUEST SENSE command
	int FASTCALL SelectCheck(const DWORD *cdb);
										// SELECT check
	BOOL FASTCALL ModeSelect(const BYTE *buf, int size);
										// MODE SELECT command
	int FASTCALL ModeSense(const DWORD *cdb, BYTE *buf);
										// MODE SENSE command
	BOOL FASTCALL TestUnitReady(const DWORD *cdb);
										// TEST UNIT READY command
	BOOL FASTCALL Rezero(const DWORD *cdb);
										// REZERO command
	BOOL FASTCALL Format(const DWORD *cdb);
										// FORMAT UNIT command
	BOOL FASTCALL Reassign(const DWORD *cdb);
										// REASSIGN UNIT command
	virtual int FASTCALL Read(BYTE *buf, int block);
										// READ command
	int FASTCALL WriteCheck(int block);
										// WRITE check
	BOOL FASTCALL Write(const BYTE *buf, int block);
										// WRITE command
	BOOL FASTCALL Seek(const DWORD *cdb);
										// SEEK command
	BOOL FASTCALL StartStop(const DWORD *cdb);
										// START STOP UNIT command
	BOOL FASTCALL SendDiag(const DWORD *cdb);
										// SEND DIAGNOSTIC command
	BOOL FASTCALL Removal(const DWORD *cdb);
										// PREVENT/ALLOW MEDIUM REMOVAL command
	int FASTCALL ReadCapacity(const DWORD *cdb, BYTE *buf);
										// READ CAPACITY command
	BOOL FASTCALL Verify(const DWORD *cdb);
										// VERIFY command
	virtual int FASTCALL ReadToc(const DWORD *cdb, BYTE *buf);
										// READ TOC command
	virtual BOOL FASTCALL PlayAudio(const DWORD *cdb);
										// PLAY AUDIO command
	virtual BOOL FASTCALL PlayAudioMSF(const DWORD *cdb);
										// PLAY AUDIO MSF command
	virtual BOOL FASTCALL PlayAudioTrack(const DWORD *cdb);
										// PLAY AUDIO TRACK command
	void FASTCALL InvalidCmd()			{ disk.code = DISK_INVALIDCMD; }
										// unsupported command

protected:
	// sub-processing
	int FASTCALL AddError(BOOL change, BYTE *buf);
										// add error page
	int FASTCALL AddFormat(BOOL change, BYTE *buf);
										// add format page
	int FASTCALL AddOpt(BOOL change, BYTE *buf);
										// add optical page
	int FASTCALL AddCache(BOOL change, BYTE *buf);
										// add cache page
	int FASTCALL AddCDROM(BOOL change, BYTE *buf);
										// add CD-ROM page
	int FASTCALL AddCDDA(BOOL change, BYTE *buf);
										// add CD-DA page
	BOOL FASTCALL CheckReady();
										// ready check

	// internal data
	disk_t disk;
										// disk internal data
	Device *ctrl;
										// controller device
	Filepath diskpath;
										// path (for GetPath)
};

//===========================================================================
//
//	SASI hard disk
//
//===========================================================================
class SASIHD : public Disk
{
public:
	// basic functions
	SASIHD(Device *dev);
										// constructor
	BOOL FASTCALL Open(const Filepath& path);
										// open

	// media operations
	void FASTCALL Reset();
										// device reset

	// commands
	int FASTCALL RequestSense(const DWORD *cdb, BYTE *buf);
										// REQUEST SENSE command
};

//===========================================================================
//
//	SCSI hard disk
//
//===========================================================================
class SCSIHD : public Disk
{
public:
	// basic functions
	SCSIHD(Device *dev);
										// constructor
	BOOL FASTCALL Open(const Filepath& path);
										// open

	// commands
	int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf);
										// INQUIRY command
};

//===========================================================================
//
//	SCSI magneto-optical disk (P668)
//
//	Ported from XM6:vm/disk.cpp:2117-2316. Load() is intentionally omitted:
//	MX's SCSI state save/load path (SCSI::Save/Load in scsi_spc.cpp) is a stub
//	and is not wired, and SCSIHD above likewise has no Load(). When a future
//	cycle wires SCSI state save/load, add Load() to SCSIHD and SCSIMO together.
//
//===========================================================================
class SCSIMO : public Disk
{
public:
	SCSIMO(Device *dev);
										// constructor
	BOOL FASTCALL Open(const Filepath& path, BOOL attn = TRUE);
										// open

	// commands
	int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf);
										// INQUIRY command
};

//===========================================================================
//
//	CD-ROM track (P676)
//
//	Ported from XM6:vm/disk.h:397-445. Comments translated to English to
//	match the rest of this header; declarations unchanged.
//
//===========================================================================
class CDTrack
{
public:
	// basic functions
	CDTrack(SCSICD *scsicd);
										// constructor
	virtual ~CDTrack();
										// destructor
	BOOL FASTCALL Init(int track, DWORD first, DWORD last);
										// initialize

	// properties
	void FASTCALL SetPath(BOOL cdda, const Filepath& path);
										// set path
	void FASTCALL GetPath(Filepath& path) const;
										// get path
	void FASTCALL AddIndex(int index, DWORD lba);
										// add index
	DWORD FASTCALL GetFirst() const;
										// get start LBA
	DWORD FASTCALL GetLast() const;
										// get end LBA
	DWORD FASTCALL GetBlocks() const;
										// get number of blocks
	int FASTCALL GetTrackNo() const;
										// get track number
	BOOL FASTCALL IsValid(DWORD lba) const;
										// is this LBA valid
	BOOL FASTCALL IsAudio() const;
										// is this an audio track

private:
	SCSICD *cdrom;
										// parent device
	BOOL valid;
										// valid track
	int track_no;
										// track number
	DWORD first_lba;
										// start LBA
	DWORD last_lba;
										// end LBA
	BOOL audio;
										// audio-track flag
	BOOL raw;
										// RAW-data flag
	Filepath imgpath;
										// image file path
};

//===========================================================================
//
//	CD-DA buffer — excluded (#if 0).
//
//	Upstream XM6 itself seals the class body (XM6:vm/disk.h:460-497) and MX
//	implements no CD-DA path (P676), so the seal is kept to stay faithful to
//	upstream rather than deleting the class outright.
//
//===========================================================================
#if 0
class CDDABuf
{
public:
	CDDABuf();
	virtual ~CDDABuf();
};
#endif	// 0 (CD-DA buffer excluded)

//===========================================================================
//
//	SCSI CD-ROM (P676)
//
//	Ported from XM6:vm/disk.h:505-588. Load() and the CD-DA members
//	(PlayAudio / PlayAudioMSF / PlayAudioTrack / NextFrame / GetBuf /
//	MSFtoLBA) are intentionally not declared: their implementations are not
//	ported. Load() follows SCSIHD / SCSIMO, which likewise have no Load()
//	because MX's SCSI state save/load path is a stub. The CD-DA methods are
//	dead in upstream XM6 as well (PlayAudio* only set DISK_INVALIDCDB and
//	GetBuf is an empty function), and the Disk base class already returns
//	INVALIDCMD for them, so nothing is left uncovered.
//	The audioindex / frame members ARE kept: the constructor initializes them
//	and is ported byte-for-byte from XM6.
//
//===========================================================================
class SCSICD : public Disk
{
public:
	// number of tracks
	enum {
		TrackMax = 96					// maximum number of tracks
	};

public:
	// basic functions
	SCSICD(Device *dev);
										// constructor
	virtual ~SCSICD();
										// destructor
	BOOL FASTCALL Open(const Filepath& path, BOOL attn = TRUE);
										// open

	// commands
	int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf);
										// INQUIRY command
	int FASTCALL Read(BYTE *buf, int block);
										// READ command
	int FASTCALL ReadToc(const DWORD *cdb, BYTE *buf);
										// READ TOC command

	// LBA-MSF conversion
	void FASTCALL LBAtoMSF(DWORD lba, BYTE *msf) const;
										// LBA -> MSF conversion

private:
	// open
	BOOL FASTCALL OpenCue(const Filepath& path);
										// open (CUE)
	BOOL FASTCALL OpenIso(const Filepath& path);
										// open (ISO)
	BOOL rawfile;
										// RAW flag

	// track management
	void FASTCALL ClearTrack();
										// clear tracks
	int FASTCALL SearchTrack(DWORD lba) const;
										// search track
	CDTrack* track[TrackMax];
										// track objects
	int tracks;
										// number of valid track objects
	int dataindex;
										// current data track
	int audioindex;
										// current audio track

	int frame;
										// frame number
};

#endif	// scsi_disk_h
