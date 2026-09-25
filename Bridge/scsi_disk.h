//---------------------------------------------------------------------------
//
//	X68000 EMULATOR "XM6"
//
//	Copyright (C) 2001-2006 PI.(Twitter: @xm6_original)
//	[ Disk ]
//
//---------------------------------------------------------------------------
//
//	MX68K (px68k の macOS 移植) への移植 — Bridge 層。SCSI/SASI HD + MO +
//	CD-ROM。ハードディスク系クラス (DiskTrack / DiskCache / Disk / SASIHD /
//	SCSIHD)、MO クラス (SCSIMO, P668)、CD-ROM 系クラス (CDTrack / SCSICD,
//	P676) は XM6 vm/disk.h から、Win32 環境の型/マクロのシムだけを適用して
//	逐語的に移植したもので、ランタイムへ配線済み。
//	CDDABuf は除外 (#if 0) のまま — 上流 XM6 自身がそのクラス本体を封印して
//	おり (XM6:vm/disk.h:452-498)、MX は CD-DA 経路を一切実装していない。
//	リポジトリ直下の NOTICE-THIRD-PARTY.md を参照のこと。
//
//---------------------------------------------------------------------------

#if !defined(scsi_disk_h)
#define scsi_disk_h

#include "scsi_compat_shim.h"

//---------------------------------------------------------------------------
//
//	クラスの前方宣言 (XM6 オリジナル)
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
//	ファイルパス (MX68K 独自の最小ラッパー — XM6 vm/filepath.h の代替)
//
//	移植したディスクコードが実際に使うインターフェースだけを提供する:
//	デフォルト構築、代入、パス参照、状態の Save/Load。
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
	char m_szPath[520];					// ファイルパス (ホスト側エンコーディング)
};

//===========================================================================
//
//	ファイル I/O (MX68K 独自の最小ラッパー — XM6 vm/fileio.h の代替)
//
//	px68k のホストファイル API (Core/px68k dosio の File_Open / File_Seek /
//	File_Read / File_Write / File_Close) へ処理を委譲する薄いラッパー。移植した
//	ディスクコードが使うインターフェースだけを提供する。ハンドルは不透明な
//	ポインタとして保持する (px68k の FILEH == HANDLE == void*、NULL = 無効) ため、
//	このヘッダは px68k のヘッダを include する必要がない。
//
//===========================================================================
class Fileio
{
public:
	enum OpenMode {
		ReadOnly,						// 読み込みのみ
		WriteOnly,						// 書き込みのみ
		ReadWrite,						// 読み書き
		Append							// 追記
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
	void *handle;						// ホストファイルハンドル (NULL = 無効)
};

//---------------------------------------------------------------------------
//
//	エラー定義 (REQUEST SENSE が返すセンスコード)
//
//	MSB		予約 (0x00)
//			センスキー
//			追加センスコード (ASC)
//	LSB		追加センスコード修飾子 (ASCQ)
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
//	ディスクトラック
//
//===========================================================================
class DiskTrack
{
public:
	// 内部データ定義
	typedef struct {
		int track;						// トラック番号
		int size;						// セクタサイズ (8 または 9)
		int sectors;					// セクタ数 (<=0x100)
		BYTE *buffer;					// データバッファ
		BOOL init;						// ロード済みフラグ
		BOOL changed;					// 変更済みフラグ
		BOOL *changemap;				// 変更済みマップ
		BOOL raw;						// RAW モード
	} disktrk_t;

public:
	// 基本ファンクション
	DiskTrack(int track, int size, int sectors, BOOL raw = FALSE);
										// コンストラクタ
	virtual ~DiskTrack();
										// デストラクタ
	BOOL FASTCALL Load(const Filepath& path);
										// ロード
	BOOL FASTCALL Save(const Filepath& path);
										// セーブ

	// 読み書き
	BOOL FASTCALL Read(BYTE *buf, int sec) const;
										// セクタ読み込み
	BOOL FASTCALL Write(const BYTE *buf, int sec);
										// セクタ書き込み

	// その他
	int FASTCALL GetTrack() const		{ return dt.track; }
										// トラック取得
	BOOL FASTCALL IsChanged() const		{ return dt.changed; }
										// 変更済みフラグチェック

private:
	// 内部データ
	disktrk_t dt;
										// 内部データ
};

//===========================================================================
//
//	ディスクキャッシュ
//
//===========================================================================
class DiskCache
{
public:
	// 内部データ定義
	typedef struct {
		DiskTrack *disktrk;				// 割り当てトラック
		DWORD serial;					// 最終シリアル
	} cache_t;

	// キャッシュ数
	enum {
		CacheMax = 16					// キャッシュするトラック数
	};

public:
	// 基本ファンクション
	DiskCache(const Filepath& path, int size, int blocks);
										// コンストラクタ
	virtual ~DiskCache();
										// デストラクタ
	void FASTCALL SetRawMode(BOOL raw);
										// CD-ROM RAW モード設定

	// アクセス
	BOOL FASTCALL Save();
										// 全セーブ & 解放
	BOOL FASTCALL Read(BYTE *buf, int block);
										// セクタ読み込み
	BOOL FASTCALL Write(const BYTE *buf, int block);
										// セクタ書き込み
	BOOL FASTCALL GetCache(int index, int& track, DWORD& serial) const;
										// キャッシュ情報取得

private:
	// 内部管理
	void FASTCALL Clear();
										// 全トラッククリア
	DiskTrack* FASTCALL Assign(int track);
										// トラックロード
	BOOL FASTCALL Load(int index, int track);
										// トラックロード
	void FASTCALL Update();
										// シリアル番号更新

	// 内部データ
	cache_t cache[CacheMax];
										// キャッシュ管理
	DWORD serial;
										// 最終アクセスシリアル番号
	Filepath sec_path;
										// パス
	int sec_size;
										// セクタサイズ (8 または 9 または 11)
	int sec_blocks;
										// セクタブロック数
	BOOL cd_raw;
										// CD-ROM RAW モード
};

//===========================================================================
//
//	ディスク
//
//===========================================================================
class Disk
{
public:
	// 内部ワーク
	typedef struct {
		DWORD id;						// メディア ID
		BOOL ready;						// 有効なディスク
		BOOL writep;					// 書き込み禁止
		BOOL readonly;					// 読み込みのみ
		BOOL removable;					// リムーバブル
		BOOL lock;						// ロック中
		BOOL attn;						// アテンション
		BOOL reset;						// リセット
		int size;						// セクタサイズ
		int blocks;						// 総セクタ数
		DWORD lun;						// LUN
		DWORD code;						// ステータスコード
		DiskCache *dcache;				// ディスクキャッシュ
	} disk_t;

public:
	// 基本ファンクション
	Disk(Device *dev);
										// コンストラクタ
	virtual ~Disk();
										// デストラクタ
	virtual void FASTCALL Reset();
										// デバイスリセット
	virtual BOOL FASTCALL Save(Fileio *fio, int ver);
										// セーブ
	virtual BOOL FASTCALL Load(Fileio *fio, int ver);
										// ロード

	// ID
	DWORD FASTCALL GetID() const		{ return disk.id; }
										// メディア ID 取得
	BOOL FASTCALL IsNULL() const;
										// NULL チェック
	BOOL FASTCALL IsSASI() const;
										// SASI チェック

	// メディア操作
	virtual BOOL FASTCALL Open(const Filepath& path);
										// オープン
	void FASTCALL GetPath(Filepath& path) const;
										// パス取得
	void FASTCALL Eject(BOOL force);
										// イジェクト
	BOOL FASTCALL IsReady() const		{ return disk.ready; }
										// レディチェック
	void FASTCALL WriteP(BOOL flag);
										// 書き込み禁止
	BOOL FASTCALL IsWriteP() const		{ return disk.writep; }
										// 書き込み禁止チェック
	BOOL FASTCALL IsReadOnly() const	{ return disk.readonly; }
										// read-only チェック
	BOOL FASTCALL IsRemovable() const	{ return disk.removable; }
										// リムーバブルチェック
	BOOL FASTCALL IsLocked() const		{ return disk.lock; }
										// ロックチェック
	BOOL FASTCALL IsAttn() const		{ return disk.attn; }
										// 交換チェック
	BOOL FASTCALL Flush();
										// キャッシュフラッシュ
	void FASTCALL GetDisk(disk_t *buffer) const;
										// 内部ワーク取得

	// プロパティ
	void FASTCALL SetLUN(DWORD lun)		{ disk.lun = lun; }
										// LUN 設定
	DWORD FASTCALL GetLUN()				{ return disk.lun; }
										// LUN 取得

	// コマンド
	virtual int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf);
										// INQUIRY コマンド
	virtual int FASTCALL RequestSense(const DWORD *cdb, BYTE *buf);
										// REQUEST SENSE コマンド
	int FASTCALL SelectCheck(const DWORD *cdb);
										// SELECT チェック
	BOOL FASTCALL ModeSelect(const BYTE *buf, int size);
										// MODE SELECT コマンド
	int FASTCALL ModeSense(const DWORD *cdb, BYTE *buf);
										// MODE SENSE コマンド
	BOOL FASTCALL TestUnitReady(const DWORD *cdb);
										// TEST UNIT READY コマンド
	BOOL FASTCALL Rezero(const DWORD *cdb);
										// REZERO コマンド
	BOOL FASTCALL Format(const DWORD *cdb);
										// FORMAT UNIT コマンド
	BOOL FASTCALL Reassign(const DWORD *cdb);
										// REASSIGN UNIT コマンド
	virtual int FASTCALL Read(BYTE *buf, int block);
										// READ コマンド
	int FASTCALL WriteCheck(int block);
										// WRITE チェック
	BOOL FASTCALL Write(const BYTE *buf, int block);
										// WRITE コマンド
	BOOL FASTCALL Seek(const DWORD *cdb);
										// SEEK コマンド
	BOOL FASTCALL StartStop(const DWORD *cdb);
										// START STOP UNIT コマンド
	BOOL FASTCALL SendDiag(const DWORD *cdb);
										// SEND DIAGNOSTIC コマンド
	BOOL FASTCALL Removal(const DWORD *cdb);
										// PREVENT/ALLOW MEDIUM REMOVAL コマンド
	int FASTCALL ReadCapacity(const DWORD *cdb, BYTE *buf);
										// READ CAPACITY コマンド
	BOOL FASTCALL Verify(const DWORD *cdb);
										// VERIFY コマンド
	virtual int FASTCALL ReadToc(const DWORD *cdb, BYTE *buf);
										// READ TOC コマンド
	virtual BOOL FASTCALL PlayAudio(const DWORD *cdb);
										// PLAY AUDIO コマンド
	virtual BOOL FASTCALL PlayAudioMSF(const DWORD *cdb);
										// PLAY AUDIO MSF コマンド
	virtual BOOL FASTCALL PlayAudioTrack(const DWORD *cdb);
										// PLAY AUDIO TRACK コマンド
	void FASTCALL InvalidCmd()			{ disk.code = DISK_INVALIDCMD; }
										// 未サポートコマンド

protected:
	// サブ処理
	int FASTCALL AddError(BOOL change, BYTE *buf);
										// エラーページ追加
	int FASTCALL AddFormat(BOOL change, BYTE *buf);
										// フォーマットページ追加
	int FASTCALL AddOpt(BOOL change, BYTE *buf);
										// オプティカルページ追加
	int FASTCALL AddCache(BOOL change, BYTE *buf);
										// キャッシュページ追加
	int FASTCALL AddCDROM(BOOL change, BYTE *buf);
										// CD-ROM ページ追加
	int FASTCALL AddCDDA(BOOL change, BYTE *buf);
										// CD-DA ページ追加
	BOOL FASTCALL CheckReady();
										// レディチェック

	// 内部データ
	disk_t disk;
										// ディスク内部データ
	Device *ctrl;
										// コントローラデバイス
	Filepath diskpath;
										// パス (GetPath 用)
};

//===========================================================================
//
//	SASI ハードディスク
//
//===========================================================================
class SASIHD : public Disk
{
public:
	// 基本ファンクション
	SASIHD(Device *dev);
										// コンストラクタ
	BOOL FASTCALL Open(const Filepath& path);
										// オープン

	// メディア操作
	void FASTCALL Reset();
										// デバイスリセット

	// コマンド
	int FASTCALL RequestSense(const DWORD *cdb, BYTE *buf);
										// REQUEST SENSE コマンド
};

//===========================================================================
//
//	SCSI ハードディスク
//
//===========================================================================
class SCSIHD : public Disk
{
public:
	// 基本ファンクション
	SCSIHD(Device *dev);
										// コンストラクタ
	BOOL FASTCALL Open(const Filepath& path);
										// オープン

	// コマンド
	int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf);
										// INQUIRY コマンド
};

//===========================================================================
//
//	SCSI 光磁気ディスク (P668)
//
//	XM6:vm/disk.cpp:2117-2316 から移植。Load() は意図的に省略している:
//	MX の SCSI 状態セーブ/ロード経路 (scsi_spc.cpp の SCSI::Save/Load) はスタブで
//	未配線であり、上の SCSIHD も同様に Load() を持たない。将来のサイクルで SCSI
//	状態のセーブ/ロードを配線する際は、SCSIHD と SCSIMO へ Load() をまとめて追加すること。
//
//===========================================================================
class SCSIMO : public Disk
{
public:
	SCSIMO(Device *dev);
										// コンストラクタ
	BOOL FASTCALL Open(const Filepath& path, BOOL attn = TRUE);
										// オープン

	// コマンド
	int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf);
										// INQUIRY コマンド
};

//===========================================================================
//
//	CD-ROM トラック (P676)
//
//	XM6:vm/disk.h:397-445 から移植。コメントはこのヘッダの他の部分と表記を
//	揃えている (宣言は無変更)。
//
//===========================================================================
class CDTrack
{
public:
	// 基本ファンクション
	CDTrack(SCSICD *scsicd);
										// コンストラクタ
	virtual ~CDTrack();
										// デストラクタ
	BOOL FASTCALL Init(int track, DWORD first, DWORD last);
										// 初期化

	// プロパティ
	void FASTCALL SetPath(BOOL cdda, const Filepath& path);
										// パス設定
	void FASTCALL GetPath(Filepath& path) const;
										// パス取得
	void FASTCALL AddIndex(int index, DWORD lba);
										// インデックス追加
	DWORD FASTCALL GetFirst() const;
										// 開始 LBA 取得
	DWORD FASTCALL GetLast() const;
										// 終了 LBA 取得
	DWORD FASTCALL GetBlocks() const;
										// ブロック数取得
	int FASTCALL GetTrackNo() const;
										// トラック番号取得
	BOOL FASTCALL IsValid(DWORD lba) const;
										// この LBA は有効か
	BOOL FASTCALL IsAudio() const;
										// オーディオトラックか

private:
	SCSICD *cdrom;
										// 親デバイス
	BOOL valid;
										// 有効なトラック
	int track_no;
										// トラック番号
	DWORD first_lba;
										// 開始 LBA
	DWORD last_lba;
										// 終了 LBA
	BOOL audio;
										// オーディオトラックフラグ
	BOOL raw;
										// RAW データフラグ
	Filepath imgpath;
										// イメージファイルパス
};

//===========================================================================
//
//	CD-DA バッファ — 除外 (#if 0)。
//
//	上流 XM6 自身がクラス本体を封印しており (XM6:vm/disk.h:460-497)、MX も
//	CD-DA 経路を実装していない (P676) ため、クラスを丸ごと削除するのではなく、
//	上流への忠実さを保つために封印をそのまま残している。
//
//===========================================================================
#if 0
class CDDABuf
{
public:
	CDDABuf();
	virtual ~CDDABuf();
};
#endif	// 0 (CD-DA バッファは除外)

//===========================================================================
//
//	SCSI CD-ROM (P676)
//
//	XM6:vm/disk.h:505-588 から移植。Load() と CD-DA 関連メンバ
//	(PlayAudio / PlayAudioMSF / PlayAudioTrack / NextFrame / GetBuf /
//	MSFtoLBA) は意図的に宣言していない: それらの実装は移植していないため。
//	Load() は SCSIHD / SCSIMO に倣っている (MX の SCSI 状態セーブ/ロード経路が
//	スタブなので、両クラスとも同様に Load() を持たない)。CD-DA 系メソッドは上流
//	XM6 でも死んだコードであり (PlayAudio* は DISK_INVALIDCDB を設定するだけで、
//	GetBuf は空関数)、Disk 基底クラスがそれらに対して既に INVALIDCMD を返すので、
//	カバーされずに残るものは無い。
//	一方 audioindex / frame メンバは残している: コンストラクタがそれらを初期化
//	しており、そのコンストラクタは XM6 からバイト単位で忠実に移植しているため。
//
//===========================================================================
class SCSICD : public Disk
{
public:
	// トラック数
	enum {
		TrackMax = 96					// 最大トラック数
	};

public:
	// 基本ファンクション
	SCSICD(Device *dev);
										// コンストラクタ
	virtual ~SCSICD();
										// デストラクタ
	BOOL FASTCALL Open(const Filepath& path, BOOL attn = TRUE);
										// オープン

	// コマンド
	int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf);
										// INQUIRY コマンド
	int FASTCALL Read(BYTE *buf, int block);
										// READ コマンド
	int FASTCALL ReadToc(const DWORD *cdb, BYTE *buf);
										// READ TOC コマンド

	// LBA-MSF 変換
	void FASTCALL LBAtoMSF(DWORD lba, BYTE *msf) const;
										// LBA -> MSF 変換

private:
	// オープン
	BOOL FASTCALL OpenCue(const Filepath& path);
										// オープン (CUE)
	BOOL FASTCALL OpenIso(const Filepath& path);
										// オープン (ISO)
	BOOL rawfile;
										// RAW フラグ

	// トラック管理
	void FASTCALL ClearTrack();
										// トラッククリア
	int FASTCALL SearchTrack(DWORD lba) const;
										// トラック検索
	CDTrack* track[TrackMax];
										// トラックオブジェクト
	int tracks;
										// 有効なトラックオブジェクト数
	int dataindex;
										// カレントデータトラック
	int audioindex;
										// カレントオーディオトラック

	int frame;
										// フレーム番号
};

#endif	// scsi_disk_h
