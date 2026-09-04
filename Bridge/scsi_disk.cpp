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
//	+ CD-ROM. The hard-disk classes (DiskTrack / DiskCache / Disk / SASIHD /
//	SCSIHD) are ported verbatim from XM6 vm/disk.cpp; only Win32 type/macro
//	shims are applied and the file I/O is delegated to px68k's host file
//	API. No logic is changed. SCSIMO is included (P668); CDTrack / SCSICD
//	are included (P676). CDDABuf and the CD-DA paths are not ported —
//	upstream XM6 itself leaves them unimplemented.
//	See NOTICE-THIRD-PARTY.md at the repository root.
//
//	NOTE: This translation unit is compiled but NOT yet wired into the
//	runtime — nothing instantiates or calls these classes yet.
//
//---------------------------------------------------------------------------

#include <cstring>
#include <cstdio>
#include <strings.h>					// strncasecmp (replaces XM6's strnicmp)

#include "scsi_disk.h"

// P676: [P676-CD] probe lines. Declared individually for the same reason as
// in scsi_spc.cpp — EmulatorBridge.h is a plain C header with no __cplusplus
// guard, so including it here would C++-mangle the symbol and fail to link.
extern "C" void debug_log(const char* fmt, ...);

// px68k host file I/O (File_Open / File_Seek / File_Read / File_Write /
// File_Close). Provided by Bridge/dosio_macos.c. dosio.h already wraps its
// declarations in its own extern "C" guard, so it is included directly.
#include "dosio.h"

//===========================================================================
//
//	File path (minimal wrapper)
//
//===========================================================================
Filepath::Filepath()
{
	m_szPath[0] = '\0';
}

Filepath::~Filepath()
{
}

Filepath& Filepath::operator=(const Filepath& path)
{
	memcpy(m_szPath, path.m_szPath, sizeof(m_szPath));
	return *this;
}

void FASTCALL Filepath::SetPath(const char* path)
{
	ASSERT(path);

	strncpy(m_szPath, path, sizeof(m_szPath) - 1);
	m_szPath[sizeof(m_szPath) - 1] = '\0';
}

// XM6:vm/filepath.cpp:70-79 の逐語移植。XM6 は分解済み drive/dir/file/ext も
// クリアするが、MX の Filepath はパス文字列単独のためこの 1 行が全体に相当する。
void FASTCALL Filepath::Clear()
{
	m_szPath[0] = '\0';
}

BOOL FASTCALL Filepath::CmpPath(const Filepath& path) const
{
	// Ported from XM6 vm/filepath.cpp — TRUE when both paths match exactly.
	// Needed by the SCSI (MB89352) port's Construct() (scsi_spc.cpp), which
	// skips re-opening a drive whose path is unchanged.
	if (strcmp(path.GetPath(), GetPath()) == 0) {
		return TRUE;
	}
	return FALSE;
}

BOOL FASTCALL Filepath::Save(Fileio *fio, int /*ver*/)
{
	// Minimal length-prefixed serialization (not yet exercised — state
	// save/load wiring is done in a later stage).
	int len;

	ASSERT(fio);

	len = (int)strlen(m_szPath);
	if (!fio->Write(&len, (int)sizeof(len))) {
		return FALSE;
	}
	if (len > 0) {
		if (!fio->Write(m_szPath, len)) {
			return FALSE;
		}
	}
	return TRUE;
}

BOOL FASTCALL Filepath::Load(Fileio *fio, int /*ver*/)
{
	int len;

	ASSERT(fio);

	if (!fio->Read(&len, (int)sizeof(len))) {
		return FALSE;
	}
	if ((len < 0) || (len >= (int)sizeof(m_szPath))) {
		return FALSE;
	}
	if (len > 0) {
		if (!fio->Read(m_szPath, len)) {
			return FALSE;
		}
	}
	m_szPath[len] = '\0';
	return TRUE;
}

//===========================================================================
//
//	File I/O (minimal wrapper — delegates to px68k host file API)
//
//===========================================================================
Fileio::Fileio()
{
	handle = 0;
}

Fileio::~Fileio()
{
	Close();
}

BOOL FASTCALL Fileio::Open(const Filepath& path, OpenMode /*mode*/)
{
	// px68k's File_Open opens read/write, falling back to read-only, so all
	// modes map onto it here. (Precise read-only vs read-write handling is
	// refined when this class is wired into the runtime.)
	if (handle != 0) {
		Close();
	}

	handle = (void*)File_Open((char*)path.GetPath());
	return IsValid();
}

BOOL FASTCALL Fileio::Seek(long offset)
{
	ASSERT(handle);

	return (BOOL)(File_Seek((FILEH)handle, (uint32_t)offset, FSEEK_SET) != (uint32_t)-1);
}

BOOL FASTCALL Fileio::Read(void *buffer, int size)
{
	ASSERT(buffer);

	if ((handle == 0) || (size <= 0)) {
		return FALSE;
	}
	return (BOOL)(File_Read((FILEH)handle, buffer, (uint32_t)size) == (uint32_t)size);
}

BOOL FASTCALL Fileio::Write(const void *buffer, int size)
{
	ASSERT(buffer);

	if ((handle == 0) || (size <= 0)) {
		return FALSE;
	}
	return (BOOL)(File_Write((FILEH)handle, (void*)buffer, (uint32_t)size) == (uint32_t)size);
}

DWORD FASTCALL Fileio::GetFileSize() const
{
	uint32_t cur;
	DWORD size;

	ASSERT(handle);

	cur = File_Seek((FILEH)handle, 0, FSEEK_CUR);
	size = (DWORD)File_Seek((FILEH)handle, 0, FSEEK_END);
	File_Seek((FILEH)handle, cur, FSEEK_SET);
	return size;
}

DWORD FASTCALL Fileio::GetFilePos() const
{
	ASSERT(handle);

	return (DWORD)File_Seek((FILEH)handle, 0, FSEEK_CUR);
}

void FASTCALL Fileio::Close()
{
	if (handle != 0) {
		File_Close((FILEH)handle);
		handle = 0;
	}
}

//===========================================================================
//
//	Disk track
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
DiskTrack::DiskTrack(int track, int size, int sectors, BOOL raw)
{
	ASSERT(track >= 0);
	ASSERT((size == 8) || (size == 9) || (size == 11));
	ASSERT((sectors > 0) && (sectors <= 0x100));

	// set parameters
	dt.track = track;
	dt.size = size;
	dt.sectors = sectors;
	dt.raw = raw;

	// not initialized (needs to be loaded)
	dt.init = FALSE;

	// not changed
	dt.changed = FALSE;

	// dynamic work does not exist
	dt.buffer = NULL;
	dt.changemap = NULL;
}

//---------------------------------------------------------------------------
//
//	Destructor
//
//---------------------------------------------------------------------------
DiskTrack::~DiskTrack()
{
	// free memory but do not auto-save
	if (dt.buffer) {
		delete[] dt.buffer;
		dt.buffer = NULL;
	}
	if (dt.changemap) {
		delete[] dt.changemap;
		dt.changemap = NULL;
	}
}

//---------------------------------------------------------------------------
//
//	Load
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskTrack::Load(const Filepath& path)
{
	Fileio fio;
	DWORD offset;
	int i;
	int length;

	ASSERT(this);

	// no need if already loaded
	if (dt.init) {
		ASSERT(dt.buffer);
		ASSERT(dt.changemap);
		return TRUE;
	}

	ASSERT(!dt.buffer);
	ASSERT(!dt.changemap);

	// calculate offset (earlier tracks are assumed to hold 256 sectors)
	offset = (dt.track << 8);
	if (dt.raw) {
		ASSERT(dt.size == 11);
		offset *= 0x930;
		offset += 0x10;
	}
	else {
		offset <<= dt.size;
	}

	// calculate length (data size of this track)
	length = dt.sectors << dt.size;

	// allocate buffer memory
	ASSERT((dt.size == 8) || (dt.size == 9) || (dt.size == 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= 0x100));
	try {
		dt.buffer = new BYTE[ length ];
	}
	catch (...) {
		dt.buffer = NULL;
		return FALSE;
	}
	if (!dt.buffer) {
		return FALSE;
	}

	// allocate change-map memory
	try {
		dt.changemap = new BOOL[dt.sectors];
	}
	catch (...) {
		dt.changemap = NULL;
		return FALSE;
	}
	if (!dt.changemap) {
		return FALSE;
	}

	// clear change map
	for (i=0; i<dt.sectors; i++) {
		dt.changemap[i] = FALSE;
	}

	// read from file
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}
	if (dt.raw) {
		// split read
		for (i=0; i<dt.sectors; i++) {
			// seek
			if (!fio.Seek(offset)) {
				fio.Close();
				return FALSE;
			}

			// read
			if (!fio.Read(&dt.buffer[i << dt.size], 1 << dt.size)) {
				fio.Close();
				return FALSE;
			}

			// next offset
			offset += 0x930;
		}
	}
	else {
		// contiguous read
		if (!fio.Seek(offset)) {
			fio.Close();
			return FALSE;
		}
		if (!fio.Read(dt.buffer, length)) {
			fio.Close();
			return FALSE;
		}
	}
	fio.Close();

	// set flag, normal exit
	dt.init = TRUE;
	dt.changed = FALSE;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Save
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskTrack::Save(const Filepath& path)
{
	DWORD offset;
	int i;
	Fileio fio;
	int length;

	ASSERT(this);

	// no need if not initialized
	if (!dt.init) {
		return TRUE;
	}

	// no need if not changed
	if (!dt.changed) {
		return TRUE;
	}

	// needs to be written
	ASSERT(dt.buffer);
	ASSERT(dt.changemap);
	ASSERT((dt.size == 8) || (dt.size == 9) || (dt.size == 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= 0x100));

	// writes are impossible in RAW mode
	ASSERT(!dt.raw);

	// calculate offset (earlier tracks are assumed to hold 256 sectors)
	offset = (dt.track << 8);
	offset <<= dt.size;

	// calculate length per sector
	length = 1 << dt.size;

	// open file
	if (!fio.Open(path, Fileio::ReadWrite)) {
		return FALSE;
	}

	// write loop
	for (i=0; i<dt.sectors; i++) {
		// if changed
		if (dt.changemap[i]) {
			// seek, write
			if (!fio.Seek(offset + (i << dt.size))) {
				fio.Close();
				return FALSE;
			}
			if (!fio.Write(&dt.buffer[i << dt.size], length)) {
				fio.Close();
				return FALSE;
			}

			// clear change flag
			dt.changemap[i] = FALSE;
		}
	}

	// close
	fio.Close();

	// clear change flag, exit
	dt.changed = FALSE;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Read sector
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskTrack::Read(BYTE *buf, int sec) const
{
	ASSERT(this);
	ASSERT(buf);
	ASSERT((sec >= 0) & (sec < 0x100));

	// error if not initialized
	if (!dt.init) {
		return FALSE;
	}

	// error if sector exceeds the valid count
	if (sec >= dt.sectors) {
		return FALSE;
	}

	// copy
	ASSERT(dt.buffer);
	ASSERT((dt.size == 8) || (dt.size == 9) || (dt.size == 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= 0x100));
	memcpy(buf, &dt.buffer[sec << dt.size], 1 << dt.size);

	// success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Write sector
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskTrack::Write(const BYTE *buf, int sec)
{
	int offset;
	int length;

	ASSERT(this);
	ASSERT(buf);
	ASSERT((sec >= 0) & (sec < 0x100));
	ASSERT(!dt.raw);

	// error if not initialized
	if (!dt.init) {
		return FALSE;
	}

	// error if sector exceeds the valid count
	if (sec >= dt.sectors) {
		return FALSE;
	}

	// calculate offset, length
	offset = sec << dt.size;
	length = 1 << dt.size;

	// compare
	ASSERT(dt.buffer);
	ASSERT((dt.size == 8) || (dt.size == 9) || (dt.size == 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= 0x100));
	if (memcmp(buf, &dt.buffer[offset], length) == 0) {
		// trying to write the same data, so normal exit
		return TRUE;
	}

	// copy, mark changed
	memcpy(&dt.buffer[offset], buf, length);
	dt.changemap[sec] = TRUE;
	dt.changed = TRUE;

	// success
	return TRUE;
}

//===========================================================================
//
//	Disk cache
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
DiskCache::DiskCache(const Filepath& path, int size, int blocks)
{
	int i;

	ASSERT((size == 8) || (size == 9) || (size == 11));
	ASSERT(blocks > 0);

	// cache work
	for (i=0; i<CacheMax; i++) {
		cache[i].disktrk = NULL;
		cache[i].serial = 0;
	}

	// misc
	serial = 0;
	sec_path = path;
	sec_size = size;
	sec_blocks = blocks;
	cd_raw = FALSE;
}

//---------------------------------------------------------------------------
//
//	Destructor
//
//---------------------------------------------------------------------------
DiskCache::~DiskCache()
{
	// clear tracks
	Clear();
}

//---------------------------------------------------------------------------
//
//	RAW mode setting
//
//---------------------------------------------------------------------------
void FASTCALL DiskCache::SetRawMode(BOOL raw)
{
	ASSERT(this);
	ASSERT(sec_size == 11);

	// set
	cd_raw = raw;
}

//---------------------------------------------------------------------------
//
//	Save
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskCache::Save()
{
	int i;

	ASSERT(this);

	// save tracks
	for (i=0; i<CacheMax; i++) {
		// valid track?
		if (cache[i].disktrk) {
			// save
			if (!cache[i].disktrk->Save(sec_path)) {
				return FALSE;
			}
		}
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Get disk-cache info
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskCache::GetCache(int index, int& track, DWORD& serial) const
{
	ASSERT(this);
	ASSERT((index >= 0) && (index < CacheMax));

	// FALSE if unused
	if (!cache[index].disktrk) {
		return FALSE;
	}

	// set track and serial
	track = cache[index].disktrk->GetTrack();
	serial = cache[index].serial;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Clear
//
//---------------------------------------------------------------------------
void FASTCALL DiskCache::Clear()
{
	int i;

	ASSERT(this);

	// release cache work
	for (i=0; i<CacheMax; i++) {
		if (cache[i].disktrk) {
			delete cache[i].disktrk;
			cache[i].disktrk = NULL;
		}
	}
}

//---------------------------------------------------------------------------
//
//	Sector read
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskCache::Read(BYTE *buf, int block)
{
	int track;
	DiskTrack *disktrk;

	ASSERT(this);
	ASSERT(sec_size != 0);

	// update first
	Update();

	// compute track (fixed at 256 sectors/track)
	track = block >> 8;

	// get that track data
	disktrk = Assign(track);
	if (!disktrk) {
		return FALSE;
	}

	// delegate to the track
	return disktrk->Read(buf, (BYTE)block);
}

//---------------------------------------------------------------------------
//
//	Sector write
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskCache::Write(const BYTE *buf, int block)
{
	int track;
	DiskTrack *disktrk;

	ASSERT(this);
	ASSERT(sec_size != 0);

	// update first
	Update();

	// compute track (fixed at 256 sectors/track)
	track = block >> 8;

	// get that track data
	disktrk = Assign(track);
	if (!disktrk) {
		return FALSE;
	}

	// delegate to the track
	return disktrk->Write(buf, (BYTE)block);
}

//---------------------------------------------------------------------------
//
//	Track assignment
//
//---------------------------------------------------------------------------
DiskTrack* FASTCALL DiskCache::Assign(int track)
{
	int i;
	int c;
	DWORD s;

	ASSERT(this);
	ASSERT(sec_size != 0);
	ASSERT(track >= 0);

	// first, check whether it is already assigned
	for (i=0; i<CacheMax; i++) {
		if (cache[i].disktrk) {
			if (cache[i].disktrk->GetTrack() == track) {
				// track matches
				cache[i].serial = serial;
				return cache[i].disktrk;
			}
		}
	}

	// next, check whether there is a free slot
	for (i=0; i<CacheMax; i++) {
		if (!cache[i].disktrk) {
			// try to load
			if (Load(i, track)) {
				// load success
				cache[i].serial = serial;
				return cache[i].disktrk;
			}

			// load failure
			return NULL;
		}
	}

	// finally, find the smallest serial number and delete it

	// candidate c = index 0
	s = cache[0].serial;
	c = 0;

	// compare with candidate serial, update to the smaller one
	for (i=0; i<CacheMax; i++) {
		ASSERT(cache[i].disktrk);

		// compare with existing serial, update
		if (cache[i].serial < s) {
			s = cache[i].serial;
			c = i;
		}
	}

	// save this track
	if (!cache[c].disktrk->Save(sec_path)) {
		return NULL;
	}

	// delete this track
	delete cache[c].disktrk;
	cache[c].disktrk = NULL;

	// load
	if (Load(c, track)) {
		// load success
		cache[c].serial = serial;
		return cache[c].disktrk;
	}

	// load failure
	return NULL;
}

//---------------------------------------------------------------------------
//
//	Track load
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskCache::Load(int index, int track)
{
	int sectors;
	DiskTrack *disktrk;

	ASSERT(this);
	ASSERT((index >= 0) && (index < CacheMax));
	ASSERT(track >= 0);
	ASSERT(!cache[index].disktrk);

	// get the number of sectors for this track
	sectors = sec_blocks - (track << 8);
	ASSERT(sectors > 0);
	if (sectors > 0x100) {
		sectors = 0x100;
	}

	// create disk track
	disktrk = new DiskTrack(track, sec_size, sectors, cd_raw);

	// try to load
	if (!disktrk->Load(sec_path)) {
		// failure
		delete disktrk;
		return FALSE;
	}

	// assignment success, set work
	cache[index].disktrk = disktrk;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Serial-number update
//
//---------------------------------------------------------------------------
void FASTCALL DiskCache::Update()
{
	int i;

	ASSERT(this);

	// update, do nothing unless it wraps to 0
	serial++;
	if (serial != 0) {
		return;
	}

	// clear all cache serials (32-bit loop)
	for (i=0; i<CacheMax; i++) {
		cache[i].serial = 0;
	}
}

//===========================================================================
//
//	Disk
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
Disk::Disk(Device *dev)
{
	// remember the controller device
	ctrl = dev;

	// init work
	disk.id = MAKEID('N', 'U', 'L', 'L');
	disk.ready = FALSE;
	disk.writep = FALSE;
	disk.readonly = FALSE;
	disk.removable = FALSE;
	disk.lock = FALSE;
	disk.attn = FALSE;
	disk.reset = FALSE;
	disk.size = 0;
	disk.blocks = 0;
	disk.lun = 0;
	disk.code = 0;
	disk.dcache = NULL;
}

//---------------------------------------------------------------------------
//
//	Destructor
//
//---------------------------------------------------------------------------
Disk::~Disk()
{
	// save disk cache
	if (disk.ready) {
		// only if ready
		ASSERT(disk.dcache);
		disk.dcache->Save();
	}

	// delete disk cache
	if (disk.dcache) {
		delete disk.dcache;
		disk.dcache = NULL;
	}
}

//---------------------------------------------------------------------------
//
//	Reset
//
//---------------------------------------------------------------------------
void FASTCALL Disk::Reset()
{
	ASSERT(this);

	// no lock, no attention, reset set
	disk.lock = FALSE;
	disk.attn = FALSE;
	disk.reset = TRUE;
}

//---------------------------------------------------------------------------
//
//	Save
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Save(Fileio *fio, int ver)
{
	size_t sz;

	ASSERT(this);
	ASSERT(fio);

	// save size
	sz = sizeof(disk_t);
	if (!fio->Write(&sz, sizeof(sz))) {
		return FALSE;
	}

	// save the body
	if (!fio->Write(&disk, (int)sz)) {
		return FALSE;
	}

	// save the path
	if (!diskpath.Save(fio, ver)) {
		return FALSE;
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Load
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Load(Fileio *fio, int ver)
{
	size_t sz;
	disk_t buf;
	Filepath path;

	ASSERT(this);
	ASSERT(fio);

	// before version 2.03 the disk was not saved
	if (ver <= 0x0202) {
		return TRUE;
	}

	// delete the current disk cache
	if (disk.dcache) {
		disk.dcache->Save();
		delete disk.dcache;
		disk.dcache = NULL;
	}

	// load size and verify
	if (!fio->Read(&sz, sizeof(sz))) {
		return FALSE;
	}
	if (sz != sizeof(disk_t)) {
		return FALSE;
	}

	// load into buffer
	if (!fio->Read(&buf, (int)sz)) {
		return FALSE;
	}

	// load the path
	if (!path.Load(fio, ver)) {
		return FALSE;
	}

	// move only if the ID matches
	if (disk.id == buf.id) {
		// do nothing if NULL
		if (IsNULL()) {
			return TRUE;
		}

		// same kind of device as when saved
		disk.ready = FALSE;
		if (Open(path)) {
			// the disk cache is created inside Open
			// move only properties
			if (!disk.readonly) {
				disk.writep = buf.writep;
			}
			disk.lock = buf.lock;
			disk.attn = buf.attn;
			disk.reset = buf.reset;
			disk.lun = buf.lun;
			disk.code = buf.code;

			// loaded successfully
			return TRUE;
		}
	}

	// recreate disk cache
	if (!IsReady()) {
		disk.dcache = NULL;
	}
	else {
		disk.dcache = new DiskCache(diskpath, disk.size, disk.blocks);
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	NULL check
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::IsNULL() const
{
	ASSERT(this);

	if (disk.id == MAKEID('N', 'U', 'L', 'L')) {
		return TRUE;
	}
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	SASI check
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::IsSASI() const
{
	ASSERT(this);

	if (disk.id == MAKEID('S', 'A', 'H', 'D')) {
		return TRUE;
	}
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	Open
//	* call from the derived class as post-processing after a successful open
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Open(const Filepath& path)
{
	Fileio fio;

	ASSERT(this);
	ASSERT((disk.size == 8) || (disk.size == 9) || (disk.size == 11));
	ASSERT(disk.blocks > 0);

	// ready
	disk.ready = TRUE;

	// init cache
	ASSERT(!disk.dcache);
	disk.dcache = new DiskCache(path, disk.size, disk.blocks);

	// can it be opened read/write?
	if (fio.Open(path, Fileio::ReadWrite)) {
		// write allowed, not read-only
		disk.writep = FALSE;
		disk.readonly = FALSE;
		fio.Close();
	}
	else {
		// write protected, read-only
		disk.writep = TRUE;
		disk.readonly = TRUE;
	}

	// not locked
	disk.lock = FALSE;

	// save path
	diskpath = path;

	// success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Eject
//
//---------------------------------------------------------------------------
void FASTCALL Disk::Eject(BOOL force)
{
	ASSERT(this);

	// cannot eject if not removable
	if (!disk.removable) {
		return;
	}

	// no need to eject if not ready
	if (!disk.ready) {
		return;
	}

	// without the force flag, it must not be locked
	if (!force) {
		if (disk.lock) {
			return;
		}
	}

	// delete disk cache
	disk.dcache->Save();
	delete disk.dcache;
	disk.dcache = NULL;

	// not ready, no attention
	disk.ready = FALSE;
	disk.writep = FALSE;
	disk.readonly = FALSE;
	disk.attn = FALSE;
}

//---------------------------------------------------------------------------
//
//	Write protect
//
//---------------------------------------------------------------------------
void FASTCALL Disk::WriteP(BOOL writep)
{
	ASSERT(this);

	// must be ready
	if (!disk.ready) {
		return;
	}

	// if read-only, only the protect state applies
	if (disk.readonly) {
		ASSERT(disk.writep);
		return;
	}

	// set flag
	disk.writep = writep;
}

//---------------------------------------------------------------------------
//
//	Get internal work
//
//---------------------------------------------------------------------------
void FASTCALL Disk::GetDisk(disk_t *buffer) const
{
	ASSERT(this);
	ASSERT(buffer);

	// copy internal work
	*buffer = disk;
}

//---------------------------------------------------------------------------
//
//	Get path
//
//---------------------------------------------------------------------------
void FASTCALL Disk::GetPath(Filepath& path) const
{
	path = diskpath;
}

//---------------------------------------------------------------------------
//
//	Flush
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Flush()
{
	ASSERT(this);

	// do nothing if there is no cache
	if (!disk.dcache) {
		return TRUE;
	}

	// save cache
	return disk.dcache->Save();
}

//---------------------------------------------------------------------------
//
//	Ready check
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::CheckReady()
{
	ASSERT(this);

	// if reset, return status
	if (disk.reset) {
		disk.code = DISK_DEVRESET;
		disk.reset = FALSE;
		return FALSE;
	}

	// if attention, return status
	if (disk.attn) {
		disk.code = DISK_ATTENTION;
		disk.attn = FALSE;
		return FALSE;
	}

	// if not ready, return status
	if (!disk.ready) {
		disk.code = DISK_NOTREADY;
		return FALSE;
	}

	// initialize with no error
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//	* must always succeed
//
//---------------------------------------------------------------------------
int FASTCALL Disk::Inquiry(const DWORD* /*cdb*/, BYTE* /*buf*/)
{
	ASSERT(this);

	// default is INQUIRY failure
	disk.code = DISK_INVALIDCMD;
	return 0;
}

//---------------------------------------------------------------------------
//
//	REQUEST SENSE
//	* SASI is handled separately
//
//---------------------------------------------------------------------------
int FASTCALL Disk::RequestSense(const DWORD *cdb, BYTE *buf)
{
	int size;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);

	// check not-ready only when there is no error
	if (disk.code == DISK_NOERROR) {
		if (!disk.ready) {
			disk.code = DISK_NOTREADY;
		}
	}

	// determine size (per allocation length)
	size = (int)cdb[4];
	ASSERT((size >= 0) && (size < 0x100));

	// SCSI-1 transfers 4 bytes when the size is 0 (removed in SCSI-2)
	if (size == 0) {
		size = 4;
	}

	// clear the buffer
	memset(buf, 0, size);

	// set 18 bytes including the extended sense data
	buf[0] = 0x70;
	buf[2] = (BYTE)(disk.code >> 16);
	buf[7] = 10;
	buf[12] = (BYTE)(disk.code >> 8);
	buf[13] = (BYTE)disk.code;

	// clear the code
	disk.code = 0x00;

	return size;
}

//---------------------------------------------------------------------------
//
//	MODE SELECT check
//	* not affected by disk.code
//
//---------------------------------------------------------------------------
int FASTCALL Disk::SelectCheck(const DWORD *cdb)
{
	int length;

	ASSERT(this);
	ASSERT(cdb);

	// error if the save-parameter bit is set
	if (cdb[1] & 0x01) {
		disk.code = DISK_INVALIDCDB;
		return 0;
	}

	// receive the data specified by the parameter length
	length = (int)cdb[4];
	return length;
}

//---------------------------------------------------------------------------
//
//	MODE SELECT
//	* not affected by disk.code
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::ModeSelect(const BYTE *buf, int size)
{
	ASSERT(this);
	ASSERT(buf);
	ASSERT(size >= 0);

	// cannot set
	disk.code = DISK_INVALIDPRM;

	return FALSE;
}

//---------------------------------------------------------------------------
//
//	MODE SENSE
//	* not affected by disk.code
//
//---------------------------------------------------------------------------
int FASTCALL Disk::ModeSense(const DWORD *cdb, BYTE *buf)
{
	int page;
	int length;
	int size;
	BOOL valid;
	BOOL change;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x1a);

	// get length, clear buffer
	length = (int)cdb[4];
	ASSERT((length >= 0) && (length < 0x100));
	memset(buf, 0, length);

	// get changeable flag
	if ((cdb[2] & 0xc0) == 0x40) {
		change = TRUE;
	}
	else {
		change = FALSE;
	}

	// get page code (0x00 is valid from the start)
	page = cdb[2] & 0x3f;
	if (page == 0x00) {
		valid = TRUE;
	}
	else {
		valid = FALSE;
	}

	// basic information
	size = 4;
	if (disk.writep) {
		buf[2] = 0x80;
	}

	// if DBD is 0, add block descriptor
	if ((cdb[1] & 0x08) == 0) {
		// mode parameter header
		buf[ 3] = 0x08;

		// only if ready
		if (disk.ready) {
			// block descriptor (number of blocks)
			buf[ 5] = (BYTE)(disk.blocks >> 16);
			buf[ 6] = (BYTE)(disk.blocks >> 8);
			buf[ 7] = (BYTE)disk.blocks;

			// block descriptor (block length)
			size = 1 << disk.size;
			buf[ 9] = (BYTE)(size >> 16);
			buf[10] = (BYTE)(size >> 8);
			buf[11] = (BYTE)size;
		}

		// reset size
		size = 12;
	}

	// page code 1 (read-write error recovery)
	if ((page == 0x01) || (page == 0x3f)) {
		size += AddError(change, &buf[size]);
		valid = TRUE;
	}

	// page code 3 (format device)
	if ((page == 0x03) || (page == 0x3f)) {
		size += AddFormat(change, &buf[size]);
		valid = TRUE;
	}

	// page code 6 (optical)
	if (disk.id == MAKEID('S', 'C', 'M', 'O')) {
		if ((page == 0x06) || (page == 0x3f)) {
			size += AddOpt(change, &buf[size]);
			valid = TRUE;
		}
	}

	// page code 8 (caching)
	if ((page == 0x08) || (page == 0x3f)) {
		size += AddCache(change, &buf[size]);
		valid = TRUE;
	}

	// page code 13 (CD-ROM)
	if (disk.id == MAKEID('S', 'C', 'C', 'D')) {
		if ((page == 0x0d) || (page == 0x3f)) {
			size += AddCDROM(change, &buf[size]);
			valid = TRUE;
		}
	}

	// page code 14 (CD-DA)
	if (disk.id == MAKEID('S', 'C', 'C', 'D')) {
		if ((page == 0x0e) || (page == 0x3f)) {
			size += AddCDDA(change, &buf[size]);
			valid = TRUE;
		}
	}

	// finalize the mode data length
	buf[0] = (BYTE)(size - 1);

	// unsupported page?
	if (!valid) {
		disk.code = DISK_INVALIDCDB;
		return 0;
	}

	// saved values are not supported
	if ((cdb[2] & 0xc0) == 0xc0) {
		disk.code = DISK_PARAMSAVE;
		return 0;
	}

	// MODE SENSE success
	disk.code = DISK_NOERROR;
	return length;
}

//---------------------------------------------------------------------------
//
//	Add error page
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddError(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// set code and length
	buf[0] = 0x01;
	buf[1] = 0x0a;

	// no changeable area
	if (change) {
		return 12;
	}

	// retry count 0, use the device internal default for the limit time
	return 12;
}

//---------------------------------------------------------------------------
//
//	Add format page
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddFormat(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// set code and length
	buf[0] = 0x03;
	buf[1] = 0x16;

	// no changeable area
	if (change) {
		return 24;
	}

	// set removable attribute
	if (disk.removable) {
		buf[20] = 0x20;
	}

	return 24;
}

//---------------------------------------------------------------------------
//
//	Add optical page
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddOpt(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// set code and length
	buf[0] = 0x06;
	buf[1] = 0x02;

	// no changeable area
	if (change) {
		return 4;
	}

	// do not report updated blocks
	return 4;
}

//---------------------------------------------------------------------------
//
//	Add cache page
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddCache(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// set code and length
	buf[0] = 0x08;
	buf[1] = 0x0a;

	// no changeable area
	if (change) {
		return 12;
	}

	// only read cache enabled, no prefetch
	return 12;
}

//---------------------------------------------------------------------------
//
//	Add CD-ROM page
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddCDROM(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// set code and length
	buf[0] = 0x0d;
	buf[1] = 0x06;

	// no changeable area
	if (change) {
		return 8;
	}

	// inactive timer is 2 sec
	buf[3] = 0x05;

	// MSF multiples are 60 and 75 respectively
	buf[5] = 60;
	buf[7] = 75;

	return 8;
}

//---------------------------------------------------------------------------
//
//	Add CD-DA page
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddCDDA(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// set code and length
	buf[0] = 0x0e;
	buf[1] = 0x0e;

	// no changeable area
	if (change) {
		return 16;
	}

	// audio waits for the operation to complete and allows multi-track PLAY
	return 16;
}

//---------------------------------------------------------------------------
//
//	TEST UNIT READY
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::TestUnitReady(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// state check
	if (!CheckReady()) {
		return FALSE;
	}

	// TEST UNIT READY success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	REZERO UNIT
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Rezero(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// state check
	if (!CheckReady()) {
		return FALSE;
	}

	// REZERO success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	FORMAT UNIT
//	* SASI uses opcode $06, SCSI uses opcode $04
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Format(const DWORD *cdb)
{
	ASSERT(this);

	// state check
	if (!CheckReady()) {
		return FALSE;
	}

	// FMTDATA=1 is not supported
	if (cdb[1] & 0x10) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// FORMAT success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	REASSIGN BLOCKS
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Reassign(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// state check
	if (!CheckReady()) {
		return FALSE;
	}

	// REASSIGN BLOCKS success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	READ
//
//---------------------------------------------------------------------------
int FASTCALL Disk::Read(BYTE *buf, int block)
{
	ASSERT(this);
	ASSERT(buf);
	ASSERT(block >= 0);

	// state check
	if (!CheckReady()) {
		return 0;
	}

	// error if it exceeds the total block count
	if (block >= disk.blocks) {
		disk.code = DISK_INVALIDLBA;
		return 0;
	}

	// delegate to the cache
	if (!disk.dcache->Read(buf, block)) {
		disk.code = DISK_READFAULT;
		return 0;
	}

	// success
	return (1 << disk.size);
}

//---------------------------------------------------------------------------
//
//	WRITE check
//
//---------------------------------------------------------------------------
int FASTCALL Disk::WriteCheck(int block)
{
	ASSERT(this);
	ASSERT(block >= 0);

	// state check
	if (!CheckReady()) {
		return 0;
	}

	// error if it exceeds the total block count
	if (block >= disk.blocks) {
		return 0;
	}

	// error if write protected
	if (disk.writep) {
		disk.code = DISK_WRITEPROTECT;
		return 0;
	}

	// success
	return (1 << disk.size);
}

//---------------------------------------------------------------------------
//
//	WRITE
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Write(const BYTE *buf, int block)
{
	ASSERT(this);
	ASSERT(buf);
	ASSERT(block >= 0);

	// error if not ready
	if (!disk.ready) {
		disk.code = DISK_NOTREADY;
		return FALSE;
	}

	// error if it exceeds the total block count
	if (block >= disk.blocks) {
		disk.code = DISK_INVALIDLBA;
		return FALSE;
	}

	// error if write protected
	if (disk.writep) {
		disk.code = DISK_WRITEPROTECT;
		return FALSE;
	}

	// delegate to the cache
	if (!disk.dcache->Write(buf, block)) {
		disk.code = DISK_WRITEFAULT;
		return FALSE;
	}

	// success
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	SEEK
//	* does not check the LBA (SASI IOCS)
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Seek(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// state check
	if (!CheckReady()) {
		return FALSE;
	}

	// SEEK success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	START STOP UNIT
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::StartStop(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x1b);

	// look at the eject bit and eject if necessary
	if (cdb[4] & 0x02) {
		if (disk.lock) {
			// locked, so cannot eject
			disk.code = DISK_PREVENT;
			return FALSE;
		}

		// eject
		Eject(FALSE);
	}

	// OK
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	SEND DIAGNOSTIC
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::SendDiag(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x1d);

	// the PF bit is not supported
	if (cdb[1] & 0x10) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// the parameter list is not supported
	if ((cdb[3] != 0) || (cdb[4] != 0)) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// always success
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	PREVENT/ALLOW MEDIUM REMOVAL
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Removal(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x1e);

	// state check
	if (!CheckReady()) {
		return FALSE;
	}

	// set the lock flag
	if (cdb[4] & 0x01) {
		disk.lock = TRUE;
	}
	else {
		disk.lock = FALSE;
	}

	// REMOVAL success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	READ CAPACITY
//
//---------------------------------------------------------------------------
int FASTCALL Disk::ReadCapacity(const DWORD* /*cdb*/, BYTE *buf)
{
	DWORD blocks;
	DWORD length;

	ASSERT(this);
	ASSERT(buf);

	// clear buffer
	memset(buf, 0, 8);

	// state check
	if (!CheckReady()) {
		return 0;
	}

	// create the end of the logical block address (disk.blocks - 1)
	ASSERT(disk.blocks > 0);
	blocks = disk.blocks - 1;
	buf[0] = (BYTE)(blocks >> 24);
	buf[1] = (BYTE)(blocks >> 16);
	buf[2] = (BYTE)(blocks >>  8);
	buf[3] = (BYTE)blocks;

	// create the block length (1 << disk.size)
	length = 1 << disk.size;
	buf[4] = (BYTE)(length >> 24);
	buf[5] = (BYTE)(length >> 16);
	buf[6] = (BYTE)(length >> 8);
	buf[7] = (BYTE)length;

	// return the returned size
	return 8;
}

//---------------------------------------------------------------------------
//
//	VERIFY
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Verify(const DWORD *cdb)
{
	int record;
    int blocks;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x2f);

	// get parameters
	record = cdb[2];
	record <<= 8;
	record |= cdb[3];
	record <<= 8;
	record |= cdb[4];
	record <<= 8;
	record |= cdb[5];
	blocks = cdb[7];
	blocks <<= 8;
	blocks |= cdb[8];

	// state check
	if (!CheckReady()) {
		return 0;
	}

	// parameter check
	if (disk.blocks < (record + blocks)) {
		disk.code = DISK_INVALIDLBA;
		return FALSE;
	}

	// success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	READ TOC
//
//---------------------------------------------------------------------------
int FASTCALL Disk::ReadToc(const DWORD *cdb, BYTE *buf)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x43);
	ASSERT(buf);

	// this command is not supported
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::PlayAudio(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x45);

	// this command is not supported
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO MSF
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::PlayAudioMSF(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x47);

	// this command is not supported
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO TRACK
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::PlayAudioTrack(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x48);

	// this command is not supported
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//===========================================================================
//
//	SASI hard disk
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
SASIHD::SASIHD(Device *dev) : Disk(dev)
{
	// SASI hard disk
	disk.id = MAKEID('S', 'A', 'H', 'D');
}

//---------------------------------------------------------------------------
//
//	Open
//
//---------------------------------------------------------------------------
BOOL FASTCALL SASIHD::Open(const Filepath& path)
{
	Fileio fio;
	DWORD size;

	ASSERT(this);
	ASSERT(!disk.ready);

	// read open is required
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// get file size
	size = fio.GetFileSize();
	fio.Close();

	// only 10MB, 20MB, 40MB
	switch (size) {
		// 10MB
		case 0x9f5400:
			break;

		// 20MB
		case 0x13c9800:
			break;

		// 40MB
		case 0x2793000:
			break;

		// otherwise (not supported)
		default:
			return FALSE;
	}

	// sector size and number of blocks
	disk.size = 8;
	disk.blocks = size >> 8;

	// base class
	return Disk::Open(path);
}

//---------------------------------------------------------------------------
//
//	Device reset
//
//---------------------------------------------------------------------------
void FASTCALL SASIHD::Reset()
{
	ASSERT(this);

	// release lock state, release attention
	disk.lock = FALSE;
	disk.attn = FALSE;

	// no reset, clear code
	disk.reset = FALSE;
	disk.code = 0x00;
}

//---------------------------------------------------------------------------
//
//	REQUEST SENSE
//
//---------------------------------------------------------------------------
int FASTCALL SASIHD::RequestSense(const DWORD *cdb, BYTE *buf)
{
	int size;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);

	// determine size
	size = (int)cdb[4];
	ASSERT((size >= 0) && (size < 0x100));

	// SASI is fixed to the non-extended format
	memset(buf, 0, size);
	buf[0] = (BYTE)(disk.code >> 16);
	buf[1] = (BYTE)(disk.lun << 5);

	// clear the code
	disk.code = 0x00;

	return size;
}

//===========================================================================
//
//	SCSI hard disk
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
SCSIHD::SCSIHD(Device *dev) : Disk(dev)
{
	// SCSI hard disk
	disk.id = MAKEID('S', 'C', 'H', 'D');
}

//---------------------------------------------------------------------------
//
//	Open
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSIHD::Open(const Filepath& path)
{
	Fileio fio;
	DWORD size;

	ASSERT(this);
	ASSERT(!disk.ready);

	// read open is required
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// get file size
	size = fio.GetFileSize();
	fio.Close();

	// must be in 512-byte units
	if (size & 0x1ff) {
		return FALSE;
	}

	// 10MB or more, less than 4GB
	if (size < 0x9f5400) {
		return FALSE;
	}
	if (size > 0xfff00000) {
		return FALSE;
	}

	// sector size and number of blocks
	disk.size = 9;
	disk.blocks = size >> 9;

	// base class
	return Disk::Open(path);
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
int FASTCALL SCSIHD::Inquiry(const DWORD *cdb, BYTE *buf)
{
	DWORD major;
	DWORD minor;
	char string[32];
	int size;
	int len;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x12);

	// EVPD check
	if (cdb[1] & 0x01) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// ready check (error if there is no image file)
	if (!disk.ready) {
		disk.code = DISK_NOTREADY;
		return FALSE;
	}

	// basic data
	// buf[0] ... Direct Access Device
	// buf[2] ... SCSI-2 compliant command set
	// buf[3] ... SCSI-2 compliant Inquiry response
	// buf[4] ... Inquiry additional data
	memset(buf, 0, 8);
	buf[2] = 0x02;
	buf[3] = 0x02;
	buf[4] = 0x1f;

	// vendor
	memset(&buf[8], 0x20, 28);
	memcpy(&buf[8], "XM6", 3);

	// product name
	size = disk.blocks >> 11;
	if (size < 300)
		sprintf(string, "PRODRIVE LPS%dS", size);
	else if (size < 600)
		sprintf(string, "MAVERICK%dS", size);
	else if (size < 800)
		sprintf(string, "LIGHTNING%dS", size);
	else if (size < 1000)
		sprintf(string, "TRAILBRAZER%dS", size);
	else if (size < 2000)
		sprintf(string, "FIREBALL%dS", size);
	else
		sprintf(string, "FBSE%d.%dS", size / 1000, (size % 1000) / 100);
	memcpy(&buf[16], string, strlen(string));

	// revision (XM6 version number)
	ctrl->GetVM()->GetVersion(major, minor);
	sprintf(string, "0%01d%01d%01d",
				major, (minor >> 4), (minor & 0x0f));
	memcpy((char*)&buf[32], string, 4);

	// transfer whichever is shorter: 36 bytes or the allocation length
	size = 36;
	len = (int)cdb[4];
	if (len < size) {
		size = len;
	}

	// success
	disk.code = DISK_NOERROR;
	return size;
}

//===========================================================================
//
//	SCSI magneto-optical disk (P668)
//
//	Ported verbatim from XM6:vm/disk.cpp:2117-2316 (comments translated to
//	English to match the rest of this translation unit; code unchanged).
//	Load() is intentionally not ported — see the note in scsi_disk.h.
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
SCSIMO::SCSIMO(Device *dev) : Disk(dev)
{
	// SCSI magneto-optical disk
	disk.id = MAKEID('S', 'C', 'M', 'O');

	// removable
	disk.removable = TRUE;
}

//---------------------------------------------------------------------------
//
//	Open
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSIMO::Open(const Filepath& path, BOOL attn)
{
	Fileio fio;
	DWORD size;

	ASSERT(this);
	ASSERT(!disk.ready);

	// read open is required
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// get file size
	size = fio.GetFileSize();
	fio.Close();

	switch (size) {
		// 128MB
		case 0x797f400:
			disk.size = 9;
			disk.blocks = 248826;
			break;

		// 230MB
		case 0xd9eea00:
			disk.size = 9;
			disk.blocks = 446325;
			break;

		// 540MB
		case 0x1fc8b800:
			disk.size = 9;
			disk.blocks = 1041500;
			break;

		// 640MB
		case 0x25e28000:
			disk.size = 11;
			disk.blocks = 310352;
			break;

		// anything else is an error
		default:
			return FALSE;
	}

	// base class
	Disk::Open(path);

	// attention if ready
	if (disk.ready && attn) {
		disk.attn = TRUE;
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
int FASTCALL SCSIMO::Inquiry(const DWORD *cdb, BYTE *buf)
{
	DWORD major;
	DWORD minor;
	char string[32];
	int size;
	int len;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x12);

	// EVPD check
	if (cdb[1] & 0x01) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// basic data
	// buf[0] ... Optical Memory Device
	// buf[1] ... removable
	// buf[2] ... SCSI-2 compliant command set
	// buf[3] ... SCSI-2 compliant Inquiry response
	// buf[4] ... Inquiry additional data
	memset(buf, 0, 8);
	buf[0] = 0x07;
	buf[1] = 0x80;
	buf[2] = 0x02;
	buf[3] = 0x02;
	buf[4] = 0x1f;

	// vendor
	memset(&buf[8], 0x20, 28);
	memcpy(&buf[8], "XM6", 3);

	// product name
	memcpy(&buf[16], "M2513A", 6);

	// revision (XM6 version number)
	ctrl->GetVM()->GetVersion(major, minor);
	sprintf(string, "0%01d%01d%01d",
				major, (minor >> 4), (minor & 0x0f));
	memcpy((char*)&buf[32], string, 4);

	// transfer whichever is shorter: 36 bytes or the allocation length
	size = 36;
	len = (int)cdb[4];
	if (len < size) {
		size = len;
	}

	// success
	disk.code = DISK_NOERROR;
	return size;
}

//===========================================================================
//
//	CD-ROM track (P676)
//
//	Ported verbatim from XM6:vm/disk.cpp:2318-2523 (comments translated to
//	English to match the rest of this translation unit; code unchanged).
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
CDTrack::CDTrack(SCSICD *scsicd)
{
	ASSERT(scsicd);

	// set the parent CD-ROM device
	cdrom = scsicd;

	// track invalid
	valid = FALSE;

	// initialize the remaining data
	track_no = -1;
	first_lba = 0;
	last_lba = 0;
	audio = FALSE;
	raw = FALSE;
}

//---------------------------------------------------------------------------
//
//	Destructor
//
//---------------------------------------------------------------------------
CDTrack::~CDTrack()
{
}

//---------------------------------------------------------------------------
//
//	Initialize
//
//---------------------------------------------------------------------------
BOOL FASTCALL CDTrack::Init(int track, DWORD first, DWORD last)
{
	ASSERT(this);
	ASSERT(!valid);
	ASSERT(track >= 1);
	ASSERT(first < last);

	// set the track number and validate
	track_no = track;
	valid = TRUE;

	// remember the LBAs
	first_lba = first;
	last_lba = last;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Set path
//
//---------------------------------------------------------------------------
void FASTCALL CDTrack::SetPath(BOOL cdda, const Filepath& path)
{
	ASSERT(this);
	ASSERT(valid);

	// CD-DA or data
	audio = cdda;

	// remember the path
	imgpath = path;
}

//---------------------------------------------------------------------------
//
//	Get path
//
//---------------------------------------------------------------------------
void FASTCALL CDTrack::GetPath(Filepath& path) const
{
	ASSERT(this);
	ASSERT(valid);

	// return the path
	path = imgpath;
}

//---------------------------------------------------------------------------
//
//	Add index
//
//---------------------------------------------------------------------------
void FASTCALL CDTrack::AddIndex(int index, DWORD lba)
{
	ASSERT(this);
	ASSERT(valid);
	ASSERT(index > 0);
	ASSERT(first_lba <= lba);
	ASSERT(lba <= last_lba);

	// indexes are not supported at present
	ASSERT(FALSE);
}

//---------------------------------------------------------------------------
//
//	Get start LBA
//
//---------------------------------------------------------------------------
DWORD FASTCALL CDTrack::GetFirst() const
{
	ASSERT(this);
	ASSERT(valid);
	ASSERT(first_lba < last_lba);

	return first_lba;
}

//---------------------------------------------------------------------------
//
//	Get end LBA
//
//---------------------------------------------------------------------------
DWORD FASTCALL CDTrack::GetLast() const
{
	ASSERT(this);
	ASSERT(valid);
	ASSERT(first_lba < last_lba);

	return last_lba;
}

//---------------------------------------------------------------------------
//
//	Get number of blocks
//
//---------------------------------------------------------------------------
DWORD FASTCALL CDTrack::GetBlocks() const
{
	ASSERT(this);
	ASSERT(valid);
	ASSERT(first_lba < last_lba);

	// compute from the start and end LBA
	return (DWORD)(last_lba - first_lba + 1);
}

//---------------------------------------------------------------------------
//
//	Get track number
//
//---------------------------------------------------------------------------
int FASTCALL CDTrack::GetTrackNo() const
{
	ASSERT(this);
	ASSERT(valid);
	ASSERT(track_no >= 1);

	return track_no;
}

//---------------------------------------------------------------------------
//
//	Is this a valid block
//
//---------------------------------------------------------------------------
BOOL FASTCALL CDTrack::IsValid(DWORD lba) const
{
	ASSERT(this);

	// FALSE if the track itself is invalid
	if (!valid) {
		return FALSE;
	}

	// FALSE if before first
	if (lba < first_lba) {
		return FALSE;
	}

	// FALSE if after last
	if (last_lba < lba) {
		return FALSE;
	}

	// this track
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Is this an audio track
//
//---------------------------------------------------------------------------
BOOL FASTCALL CDTrack::IsAudio() const
{
	ASSERT(this);
	ASSERT(valid);

	return audio;
}

//===========================================================================
//
//	CD-DA buffer — not ported.
//
//	XM6:vm/disk.cpp:2525-2547 defines only an empty constructor and
//	destructor; the class body is sealed (#if 0) in upstream XM6's own
//	header. MX implements no CD-DA path, so nothing is ported here.
//
//===========================================================================

//===========================================================================
//
//	SCSI CD-ROM (P676)
//
//	Ported verbatim from XM6:vm/disk.cpp:2549-3259 (comments translated to
//	English to match the rest of this translation unit; code unchanged
//	apart from the two documented substitutions: strnicmp -> strncasecmp,
//	and the inserted [P676-CD] debug_log probe lines, each marked with a
//	/* MX68K probe */ comment).
//
//	Not ported: Load (XM6:2602), PlayAudio / PlayAudioMSF / PlayAudioTrack
//	(XM6:3113 / 3126 / 3139 — all three merely set DISK_INVALIDCDB, which
//	the Disk base class already does), MSFtoLBA (XM6:3188, used only by
//	PlayAudioMSF), NextFrame (XM6:3266) and GetBuf (XM6:3288, an empty
//	function upstream). All of these belong to the CD-DA path.
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
SCSICD::SCSICD(Device *dev) : Disk(dev)
{
	int i;

	// SCSI CD-ROM
	disk.id = MAKEID('S', 'C', 'C', 'D');

	// removable, write protected
	disk.removable = TRUE;
	disk.writep = TRUE;

	// not RAW format
	rawfile = FALSE;

	// initialize the frame
	frame = 0;

	// initialize the tracks
	for (i=0; i<TrackMax; i++) {
		track[i] = NULL;
	}
	tracks = 0;
	dataindex = -1;
	audioindex = -1;
}

//---------------------------------------------------------------------------
//
//	Destructor
//
//---------------------------------------------------------------------------
SCSICD::~SCSICD()
{
	// clear the tracks
	ClearTrack();
}

//---------------------------------------------------------------------------
//
//	Open
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::Open(const Filepath& path, BOOL attn)
{
	Fileio fio;
	DWORD size;
	char file[5];

	ASSERT(this);
	ASSERT(!disk.ready);

	// initialize, clear the tracks
	disk.blocks = 0;
	rawfile = FALSE;
	ClearTrack();

	// a read open is required
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// get the size
	size = fio.GetFileSize();
	if (size <= 4) {
		fio.Close();
		return FALSE;
	}

	// decide whether this is a CUE sheet or an ISO file
	fio.Read(file, 4);
	file[4] = '\0';
	fio.Close();

	// if it starts with FILE, treat it as a CUE sheet
	if (strncasecmp(file, "FILE", 4) == 0) {
		// open as CUE
		if (!OpenCue(path)) {
			return FALSE;
		}
	}
	else {
		// open as ISO
		if (!OpenIso(path)) {
			return FALSE;
		}
	}

	// open succeeded
	ASSERT(disk.blocks > 0);
	disk.size = 11;

	// base class
	Disk::Open(path);

	// set the RAW flag
	ASSERT(disk.dcache);
	disk.dcache->SetRawMode(rawfile);

	// ROM media, so writing is not possible
	disk.writep = TRUE;

	// attention if ready
	if (disk.ready && attn) {
		disk.attn = TRUE;
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Open (CUE)
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::OpenCue(const Filepath& path)
{
	ASSERT(this);

	/* MX68K probe */
	debug_log("[P676-CD] openiso: size=0 raw=0 hdr3=n/a reason=cue path=%s\n",
	          path.GetPath());

	// always fails
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	Open (ISO)
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::OpenIso(const Filepath& path)
{
	Fileio fio;
	DWORD size;
	BYTE header[12];
	BYTE sync[12];

	ASSERT(this);

	// a read open is required
	if (!fio.Open(path, Fileio::ReadOnly)) {
		/* MX68K probe */
		debug_log("[P676-CD] openiso: size=0 raw=0 hdr3=n/a reason=fileopen path=%s\n",
		          path.GetPath());
		return FALSE;
	}

	// get the size
	size = fio.GetFileSize();
	if (size < 0x800) {
		fio.Close();
		/* MX68K probe */
		debug_log("[P676-CD] openiso: size=%u raw=0 hdr3=n/a reason=toosmall path=%s\n",
		          (unsigned)size, path.GetPath());
		return FALSE;
	}

	// read the first 12 bytes and close
	if (!fio.Read(header, sizeof(header))) {
		fio.Close();
		/* MX68K probe */
		debug_log("[P676-CD] openiso: size=%u raw=0 hdr3=n/a reason=headerread path=%s\n",
		          (unsigned)size, path.GetPath());
		return FALSE;
	}

	// check whether this is RAW format
	memset(sync, 0xff, sizeof(sync));
	sync[0] = 0x00;
	sync[11] = 0x00;
	rawfile = FALSE;
	if (memcmp(header, sync, sizeof(sync)) == 0) {
		// 00,FFx10,00, so this is presumed to be RAW format
		if (!fio.Read(header, 4)) {
			fio.Close();
			/* MX68K probe */
			debug_log("[P676-CD] openiso: size=%u raw=1 hdr3=n/a reason=moderead path=%s\n",
			          (unsigned)size, path.GetPath());
			return FALSE;
		}

		// only MODE1/2048 and MODE1/2352 are supported
		if (header[3] != 0x01) {
			// wrong mode
			fio.Close();
			/* MX68K probe */
			debug_log("[P676-CD] openiso: size=%u raw=1 hdr3=0x%02X reason=notmode1 path=%s\n",
			          (unsigned)size, header[3], path.GetPath());
			return FALSE;
		}

		// set to RAW file
		rawfile = TRUE;
	}
	fio.Close();

	if (rawfile) {
		// the size must be a multiple of 2352 and at most 700MB
		if (size % 0x930) {
			/* MX68K probe */
			debug_log("[P676-CD] openiso: size=%u raw=1 hdr3=0x%02X reason=rawmultiple path=%s\n",
			          (unsigned)size, header[3], path.GetPath());
			return FALSE;
		}
		if (size > 912579600) {
			/* MX68K probe */
			debug_log("[P676-CD] openiso: size=%u raw=1 hdr3=0x%02X reason=rawtoobig path=%s\n",
			          (unsigned)size, header[3], path.GetPath());
			return FALSE;
		}

		// set the number of blocks
		disk.blocks = size / 0x930;
	}
	else {
		// the size must be a multiple of 2048 and at most 700MB
		if (size & 0x7ff) {
			/* MX68K probe */
			debug_log("[P676-CD] openiso: size=%u raw=0 hdr3=0x%02X reason=isomultiple path=%s\n",
			          (unsigned)size, header[3], path.GetPath());
			return FALSE;
		}
		if (size > 0x2bed5000) {
			/* MX68K probe */
			debug_log("[P676-CD] openiso: size=%u raw=0 hdr3=0x%02X reason=isotoobig path=%s\n",
			          (unsigned)size, header[3], path.GetPath());
			return FALSE;
		}

		// set the number of blocks
		disk.blocks = size >> 11;
	}

	// create only a single data track
	ASSERT(!track[0]);
	track[0] = new CDTrack(this);
	track[0]->Init(1, 0, disk.blocks - 1);
	track[0]->SetPath(FALSE, path);
	tracks = 1;
	dataindex = 0;

	/* MX68K probe */
	debug_log("[P676-CD] openiso: size=%u raw=%d hdr3=0x%02X reason=ok blocks=%u path=%s\n",
	          (unsigned)size, rawfile ? 1 : 0, header[3],
	          (unsigned)disk.blocks, path.GetPath());

	// open succeeded
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
int FASTCALL SCSICD::Inquiry(const DWORD *cdb, BYTE *buf)
{
	DWORD major;
	DWORD minor;
	char string[32];
	int size;
	int len;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x12);

	// EVPD check
	if (cdb[1] & 0x01) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// basic data
	// buf[0] ... CD-ROM Device
	// buf[1] ... removable
	// buf[2] ... SCSI-2 compliant command set
	// buf[3] ... SCSI-2 compliant Inquiry response
	// buf[4] ... Inquiry additional data
	memset(buf, 0, 8);
	buf[0] = 0x05;
	buf[1] = 0x80;
	buf[2] = 0x02;
	buf[3] = 0x02;
	buf[4] = 0x1f;

	// vendor
	memset(&buf[8], 0x20, 28);
	memcpy(&buf[8], "XM6", 3);

	// product name
	memcpy(&buf[16], "CDU-55S", 7);

	// revision (XM6 version number)
	ctrl->GetVM()->GetVersion(major, minor);
	sprintf(string, "0%01d%01d%01d",
				major, (minor >> 4), (minor & 0x0f));
	memcpy((char*)&buf[32], string, 4);

	// transfer whichever is shorter: 36 bytes or the allocation length
	size = 36;
	len = cdb[4];
	if (len < size) {
		size = len;
	}

	/* MX68K probe */
	debug_log("[P676-CD] inquiry: type=0x%02X alloc=%d size=%d ready=%d\n",
	          buf[0], len, size, disk.ready ? 1 : 0);

	// success
	disk.code = DISK_NOERROR;
	return size;
}

//---------------------------------------------------------------------------
//
//	READ
//
//---------------------------------------------------------------------------
int FASTCALL SCSICD::Read(BYTE *buf, int block)
{
	int index;
	Filepath path;

	ASSERT(this);
	ASSERT(buf);
	ASSERT(block >= 0);

	// check the state
	if (!CheckReady()) {
		return 0;
	}

	// search the track
	index = SearchTrack(block);

	// out of range if invalid
	if (index < 0) {
		disk.code = DISK_INVALIDLBA;
		return 0;
	}
	ASSERT(track[index]);

	// if it differs from the current data track
	if (dataindex != index) {
		// delete the current disk cache (no need to Save)
		delete disk.dcache;
		disk.dcache = NULL;

		// set the number of blocks again
		disk.blocks = track[index]->GetBlocks();
		ASSERT(disk.blocks > 0);

		// rebuild the disk cache
		track[index]->GetPath(path);
		disk.dcache = new DiskCache(path, disk.size, disk.blocks);
		disk.dcache->SetRawMode(rawfile);

		// set the data index again
		dataindex = index;
	}

	// base class
	ASSERT(dataindex >= 0);
	return Disk::Read(buf, block);
}

//---------------------------------------------------------------------------
//
//	READ TOC
//
//---------------------------------------------------------------------------
int FASTCALL SCSICD::ReadToc(const DWORD *cdb, BYTE *buf)
{
	int last;
	int index;
	int length;
	int loop;
	int i;
	BOOL msf;
	DWORD lba;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x43);
	ASSERT(buf);

	// ready check
	if (!CheckReady()) {
		return 0;
	}

	// if ready, at least one track exists
	ASSERT(tracks > 0);
	ASSERT(track[0]);

	// get the allocation length, clear the buffer
	length = cdb[7] << 8;
	length |= cdb[8];
	memset(buf, 0, length);

	// get the MSF flag
	if (cdb[1] & 0x02) {
		msf = TRUE;
	}
	else {
		msf = FALSE;
	}

	// get and check the last track number
	last = track[tracks - 1]->GetTrackNo();
	if ((int)cdb[6] > last) {
		// AA is the exception
		if (cdb[6] != 0xaa) {
			disk.code = DISK_INVALIDCDB;
			return 0;
		}
	}

	// check the start index
	index = 0;
	if (cdb[6] != 0x00) {
		// advance the tracks until the track number matches
		while (track[index]) {
			if ((int)cdb[6] == track[index]->GetTrackNo()) {
				break;
			}
			index++;
		}

		// if not found, it is either AA or an internal error
		if (!track[index]) {
			if (cdb[6] == 0xaa) {
				// AA, so return the last LBA + 1
				buf[0] = 0x00;
				buf[1] = 0x0a;
				buf[2] = (BYTE)track[0]->GetTrackNo();
				buf[3] = (BYTE)last;
				buf[6] = 0xaa;
				lba = track[tracks -1]->GetLast() + 1;
				if (msf) {
					LBAtoMSF(lba, &buf[8]);
				}
				else {
					buf[10] = (BYTE)(lba >> 8);
					buf[11] = (BYTE)lba;
				}
				return length;
			}

			// anything else is an error
			disk.code = DISK_INVALIDCDB;
			return 0;
		}
	}

	// the number of track descriptors to return this time (loop count)
	loop = last - track[index]->GetTrackNo() + 1;
	ASSERT(loop >= 1);

	// build the header
	buf[0] = (BYTE)(((loop << 3) + 2) >> 8);
	buf[1] = (BYTE)((loop << 3) + 2);
	buf[2] = (BYTE)track[0]->GetTrackNo();
	buf[3] = (BYTE)last;
	buf += 4;

	// loop
	for (i=0; i<loop; i++) {
		// ADR and Control
		if (track[index]->IsAudio()) {
			// audio track
			buf[1] = 0x10;
		}
		else {
			// data track
			buf[1] = 0x14;
		}

		// track number
		buf[2] = (BYTE)track[index]->GetTrackNo();

		// track address
		if (msf) {
			LBAtoMSF(track[index]->GetFirst(), &buf[4]);
		}
		else {
			buf[6] = (BYTE)(track[index]->GetFirst() >> 8);
			buf[7] = (BYTE)(track[index]->GetFirst());
		}

		// advance the buffer and the index
		buf += 8;
		index++;
	}

	// always return exactly the allocation length
	return length;
}

//---------------------------------------------------------------------------
//
//	LBA -> MSF conversion
//
//---------------------------------------------------------------------------
void FASTCALL SCSICD::LBAtoMSF(DWORD lba, BYTE *msf) const
{
	DWORD m;
	DWORD s;
	DWORD f;

	ASSERT(this);

	// take the remainders by 75 and by 75*60 respectively
	m = lba / (75 * 60);
	s = lba % (75 * 60);
	f = s % 75;
	s /= 75;

	// the origin is M=0,S=2,F=0
	s += 2;
	if (s >= 60) {
		s -= 60;
		m++;
	}

	// store
	ASSERT(m < 0x100);
	ASSERT(s < 60);
	ASSERT(f < 75);
	msf[0] = 0x00;
	msf[1] = (BYTE)m;
	msf[2] = (BYTE)s;
	msf[3] = (BYTE)f;
}

//---------------------------------------------------------------------------
//
//	Clear tracks
//
//---------------------------------------------------------------------------
void FASTCALL SCSICD::ClearTrack()
{
	int i;

	ASSERT(this);

	// delete the track objects
	for (i=0; i<TrackMax; i++) {
		if (track[i]) {
			delete track[i];
			track[i] = NULL;
		}
	}

	// zero tracks
	tracks = 0;

	// neither data nor audio is set
	dataindex = -1;
	audioindex = -1;
}

//---------------------------------------------------------------------------
//
//	Search track
//	* returns -1 if not found
//
//---------------------------------------------------------------------------
int FASTCALL SCSICD::SearchTrack(DWORD lba) const
{
	int i;

	ASSERT(this);

	// track loop
	for (i=0; i<tracks; i++) {
		// ask the track
		ASSERT(track[i]);
		if (track[i]->IsValid(lba)) {
			return i;
		}
	}

	// not found
	return -1;
}
