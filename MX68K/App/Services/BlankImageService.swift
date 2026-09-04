import Foundation

// P574 — 空(未フォーマット)ディスクイメージの新規作成(Docs/10 A-1 / C-1 / C-2)。
//
// 方針(ユーザー決定 2026-08-14): **物理フォーマットのみ**を生成する。
// 論理フォーマット(IPL/FAT)は SHARP/Hudson 製作物のブロブを埋め込む必要があり、
// BIOS ファイルと同様の配布ライセンス問題になりうるため作らない — 生成した
// イメージはゲスト側の `FORMAT.X` / `SXFORMAT` で論理フォーマットする前提。
//
// Core/Bridge は一切変更しない。生成したファイルは既存の FD/SASI/SCSI マウント
// 経路がそのまま扱う(自動マウントはしない — 生成とマウントは分離する)。

enum BlankImageError: Error {
    case writeFailed
}

/// P672 — MO 空イメージの論理フォーマット(Docs/10 C-4)。
///
/// `.none` = 物理フォーマットのみ(P574 以来の既存挙動、ゲスト側でフォーマットする前提)。
/// `.ibm`  = FAT16 スーパーフロッピー形式。`XM6:mfc/mfc_tool.cpp:2130-2390`
///           (`CMOMakeDlg::FormatIBM` / `MakeBPB` / `MakeSerial`)の逐語移植。
///           SHARP(起動可能)形式は巨大な IPL ブロブの移植が必要なため未対応。
enum MOLogicalFormat {
    case none
    case ibm
}

enum BlankImageService {

    // MARK: - サイズ/埋め値の定数
    //
    // ★これらの値は Bridge / Core 側の受理条件と**バイト単位で一致**していなければ
    // ならない。Bridge の C マクロを Swift から直接参照する公開 API は無いため、
    // 一次情報源を明記した上で同一の整数リテラルを複製している。
    // 値を変更する場合は必ず下記の一次情報源側と同時に更新すること。

    /// A-1: raw(XDF)形式の 2HD イメージの固定バイト数(= 0x134000)。
    /// 一次情報源: `Core:` `x68k/disk_xdf.c`(サイズ固定チェック)。
    /// 1024B/sector × 8 sector/track × 154 track(77 トラック × 2 面)。
    /// この値が違うと MX 自身の disk_xdf ローダがイメージを読めない。
    static let fdRawSize = 1_261_568

    /// A-1: FD 未使用領域の埋め値。
    /// 一次情報源(3 者一致): `px68k本家:` `disk_xdf.c:40` / `disk_dim.c:67`、
    /// `XM6:` `vm/fdi.cpp:527`、`XEiJ:` `FDMedia.java:344-345`。
    static let fdFillByte: UInt8 = 0xE5

    /// C-1: SASI HDD の実機容量制約(10/20/40MB の 3 値のみ)。
    /// 一次情報源: `MX:` `Bridge/EmulatorBridge.c:4868-4870`
    /// (`P458_SASI_10MB` = 0x9f5400 / `P458_SASI_20MB` = 0x13c9800 /
    ///  `P458_SASI_40MB` = 0x2793000。P458/D-29 で確定、大元は `XM6:` `vm/disk.cpp:1878-1920`)。
    /// これ以外のサイズは `mx68k_hdd_insert()` の既存検証がマウントを拒否する。
    static let sasi10MB = 0x9f5400     // 10,441,728 B
    static let sasi20MB = 0x13c9800    // 20,748,288 B
    static let sasi40MB = 0x2793000    // 41,496,576 B

    /// C-2: 外付け SCSI HDD の受理サイズ範囲(512B の倍数 かつ この範囲内)。
    /// 一次情報源: `MX:` `Bridge/EmulatorBridge.c:4871-4872`
    /// (`P458_SCSI_MIN` = 0x9f5400 / `P458_SCSI_MAX` = 0xfff00000)。
    /// 範囲外だと `mx68k_scsi_insert()` の既存検証がマウントを拒否する。
    static let scsiMinSize = 0x9f5400        // 10,441,728 B(10MB)
    static let scsiMaxSize = 0xfff00000      // 4,293,918,720 B(4095MiB)

    /// C-4: SCSI MO の実機容量(128/230/540/640MB の 4 値のみ)。
    /// 一次情報源: `XM6:` `vm/disk.cpp:2131-2187`(`SCSIMO::Open` のサイズ完全一致判定)。
    /// ★これらは Bridge 側 `Bridge/EmulatorBridge.c` の `P668_MO_*` 定数、および
    ///   移植した `SCSIMO::Open` の switch と**バイト単位で一致**していなければ
    ///   ならない。片方だけ変更してはならない。
    /// 検算: 248826×512 / 446325×512 / 1041500×512 / 310352×2048。
    /// ★640MB のみセクタ長 2048B(`disk.size` = 11)、他 3 者は 512B(= 9)。
    static let mo128MB = 0x0797F400   // 127,398,912 B
    static let mo230MB = 0x0D9EEA00   // 228,518,400 B
    static let mo540MB = 0x1FC8B800   // 533,248,000 B
    static let mo640MB = 0x25E28000   // 635,600,896 B

    // MARK: - 生成 API

    /// A-1: 2HD(1,261,568B)の空 FD イメージを 0xE5 埋めで生成する。
    static func createBlankFD(at url: URL) throws {
        let data = Data(repeating: fdFillByte, count: fdRawSize)
        do {
            try data.write(to: url, options: .atomic)
        } catch {
            throw BlankImageError.writeFailed
        }
    }

    /// C-1: SASI HDD(10/20/40MB のいずれか)の空イメージを 0x00 埋めで生成する。
    /// `sizeBytes` は呼び出し側が上記 `sasi10MB` 等の定数から渡す。
    static func createBlankSASI(at url: URL, sizeBytes: Int) throws {
        try createZeroFilled(at: url, sizeBytes: sizeBytes)
    }

    /// C-2: SCSI HDD(10MB〜4095MiB、512B 単位)の空イメージを 0x00 埋めで生成する。
    static func createBlankSCSI(at url: URL, sizeBytes: Int) throws {
        try createZeroFilled(at: url, sizeBytes: sizeBytes)
    }

    /// C-4: MO(上記 4 容量のいずれか)の空イメージを 0x00 埋めで生成する。
    /// `format` が `.ibm` のときのみ、ゼロ埋め後に FAT16 スーパーフロッピーの
    /// BPB / ブートシグネチャ / FAT 先頭シードを書き込む。
    static func createBlankMO(at url: URL, sizeBytes: Int, format: MOLogicalFormat = .none) throws {
        try createZeroFilled(at: url, sizeBytes: sizeBytes)
        guard format == .ibm else { return }
        do {
            let fileHandle = try FileHandle(forWritingTo: url)
            defer { try? fileHandle.close() }
            try writeIBMFormatBPB(fileHandle: fileHandle, sizeBytes: sizeBytes)
        } catch {
            // 中途半端な(ゼロ埋めだけ済んで BPB が無い)イメージを残置しない。
            try? FileManager.default.removeItem(at: url)
            throw BlankImageError.writeFailed
        }
    }

    // MARK: - IBM フォーマット(FAT16 スーパーフロッピー)

    /// 容量ごとに異なる BPB パラメータ。
    /// 一次情報源: `XM6:mfc/mfc_tool.cpp:2206-2360`(`CMOMakeDlg::MakeBPB` の switch)。
    /// `sectorsPerFAT` は `⌈必要FATバイト数 ÷ バイト/セクタ⌉` として独立検算済み
    /// (`.mx68k_cycles/P672_spec_inv.md` §5 — 4 容量ともクラスタ数が
    ///  FAT16 の 4085〜65524 域に収まることも確認)。
    private struct IBMFormatParams {
        let bytesPerSector: Int
        let sectorsPerCluster: UInt8
        let sectorsPerFAT: UInt16
        let sectorsPerTrack: UInt16
    }

    private static func ibmFormatParams(forSize sizeBytes: Int) -> IBMFormatParams? {
        switch sizeBytes {
        case mo128MB:
            return IBMFormatParams(bytesPerSector: 512, sectorsPerCluster: 4, sectorsPerFAT: 243, sectorsPerTrack: 25)
        case mo230MB:
            return IBMFormatParams(bytesPerSector: 512, sectorsPerCluster: 8, sectorsPerFAT: 218, sectorsPerTrack: 32)
        case mo540MB:
            return IBMFormatParams(bytesPerSector: 512, sectorsPerCluster: 16, sectorsPerFAT: 255, sectorsPerTrack: 32)
        case mo640MB:
            // ★640MB のみセクタ長 2048B。`Bridge/scsi_disk.cpp:2351-2354`
            //   (`SCSIMO::Open`)が同じ 2048B を設定するので整合する。
            return IBMFormatParams(bytesPerSector: 2048, sectorsPerCluster: 8, sectorsPerFAT: 38, sectorsPerTrack: 32)
        default:
            return nil
        }
    }

    /// ゼロ埋め済みの MO イメージへ FAT16 スーパーフロッピー構造を書き込む。
    /// ルートディレクトリ・FAT 本体・データ領域はゼロのまま(= 空ボリューム)で、
    /// 書き込むのは BPB・ブートシグネチャ・各 FAT 先頭 4 バイトの 3 箇所だけ。
    private static func writeIBMFormatBPB(fileHandle: FileHandle, sizeBytes: Int) throws {
        guard let params = ibmFormatParams(forSize: sizeBytes) else {
            throw BlankImageError.writeFailed
        }

        let reservedSectors: UInt16 = 1
        let fatCount: UInt8 = 2
        let rootEntries: UInt16 = 512
        let mediaID: UInt8 = 0xF0

        var bpb = [UInt8](repeating: 0, count: 62)

        // 0-2: ブート先頭のジャンプ命令。x86 の無限ループ(= 非起動)。
        bpb[0] = 0xEB
        bpb[1] = 0xFE
        bpb[2] = 0x90

        // 3-10: OEM 名。★XM6 は `"XM6 X.XX"` を書くが、ここだけは MX68K 独自の
        //   識別文字列にする — FAT16 の動作に影響しない唯一の意図的な差分で、
        //   BPB の他フィールドはすべて XM6 と同一値。
        var oemName = Array("MX68K   ".utf8.prefix(8))
        oemName.append(contentsOf: [UInt8](repeating: 0x20, count: 8 - oemName.count))
        bpb.replaceSubrange(3..<11, with: oemName)

        // 11-12: バイト/セクタ
        writeLE16(&bpb, 11, UInt16(params.bytesPerSector))
        // 13: セクタ/クラスタ
        bpb[13] = params.sectorsPerCluster
        // 14-15: 予約セクタ数
        writeLE16(&bpb, 14, reservedSectors)
        // 16: FAT 数
        bpb[16] = fatCount
        // 17-18: ルートディレクトリのエントリ数
        writeLE16(&bpb, 17, rootEntries)
        // 19-20: 論理セクタ数(16bit)。32bit 側(32-35)を使うので 0。
        writeLE16(&bpb, 19, 0)
        // 21: メディア ID バイト
        bpb[21] = mediaID
        // 22-23: セクタ/FAT
        writeLE16(&bpb, 22, params.sectorsPerFAT)
        // 24-25: セクタ/トラック
        writeLE16(&bpb, 24, params.sectorsPerTrack)
        // 26-27: ヘッド数
        writeLE16(&bpb, 26, 1)
        // 28-31: 隠しセクタ数(0 のまま)

        // 32-35: 論理セクタ数(32bit)。
        // ★XM6 は総セクタ数から 1 を引いた値を書く(ソースに理由のコメントは無い)。
        //   一般的な FAT スーパーフロッピー仕様は総セクタ数そのものだが、
        //   「MX は後発移植」の原則に従い参照実装と同一の値を採用する。
        let totalSectors = UInt32(sizeBytes / params.bytesPerSector - 1)
        bpb[32] = UInt8(totalSectors & 0xFF)
        bpb[33] = UInt8((totalSectors >> 8) & 0xFF)
        bpb[34] = UInt8((totalSectors >> 16) & 0xFF)
        bpb[35] = 0x00   // XM6 は bit24-31 を常に 0 で書く

        // 36: INT 13h ドライブ番号(C: = 0x80)
        bpb[36] = 0x80
        // 37: 予約(0 のまま)
        // 38: 拡張ブート識別子
        bpb[38] = 0x29
        // 39-42: ボリュームシリアル番号
        bpb.replaceSubrange(39..<43, with: makeVolumeSerial())
        // 43-53: ボリュームラベル / 54-61: ファイルシステム種別。
        //   XM6 は 43 から 18 文字 + NUL を strcpy したうえで 61 を 0x20 で
        //   上書きしており、結果はこの 19 バイトと同一になる。
        bpb.replaceSubrange(43..<62, with: Array("NO NAME    FAT16   ".utf8))

        // 1. オフセット 0 へ BPB。
        try fileHandle.seek(toOffset: 0)
        try fileHandle.write(contentsOf: Data(bpb))

        // 2. オフセット 0x1FE へブートシグネチャ。
        try fileHandle.seek(toOffset: 0x1FE)
        try fileHandle.write(contentsOf: Data([0x55, 0xAA]))

        // 3. 各 FAT の先頭 4 バイトへメディア ID シード。
        //    セクタオフセット = セクタ/FAT × FATインデックス + 予約セクタ数。
        let fatSeed = Data([mediaID, 0xFF, 0xFF, 0xFF])
        for index in 0..<Int(fatCount) {
            let sectorOffset = Int(params.sectorsPerFAT) * index + Int(reservedSectors)
            try fileHandle.seek(toOffset: UInt64(sectorOffset * params.bytesPerSector))
            try fileHandle.write(contentsOf: fatSeed)
        }

        try fileHandle.synchronize()
    }

    /// ボリュームシリアル番号(4 バイト)を生成時刻から算出する。
    /// `XM6:mfc/mfc_tool.cpp:2369-2390`(`CMOMakeDlg::MakeSerial`)の逐語移植で、
    /// 各バイトへの代入時に 8bit へ切り詰められる(桁あふれは意図どおり捨てる)。
    private static func makeVolumeSerial(now: Date = Date()) -> [UInt8] {
        let parts = Calendar.current.dateComponents([.year, .day, .hour, .minute, .second], from: now)
        let year = parts.year ?? 0
        let day = parts.day ?? 0
        let hour = parts.hour ?? 0
        let minute = parts.minute ?? 0
        let second = parts.second ?? 0
        return [
            UInt8(((year & 0xFF) + minute) & 0xFF),
            UInt8((((year >> 8) & 0xFF) + hour) & 0xFF),
            UInt8(day & 0xFF),
            UInt8(second & 0xFF)
        ]
    }

    private static func writeLE16(_ buffer: inout [UInt8], _ offset: Int, _ value: UInt16) {
        buffer[offset] = UInt8(value & 0xFF)
        buffer[offset + 1] = UInt8(value >> 8)
    }

    // MARK: - 内部

    /// 0x00 埋めファイルを `truncate()` ベースのスパースファイルとして生成する
    /// (SCSI は最大 4095MiB になりうるため、実書込みではなくスパース化で高速化・
    ///  ディスク使用量削減。APFS はスパースファイルをサポートする)。
    /// 失敗時は作成済みの空ファイルを残置しない。
    private static func createZeroFilled(at url: URL, sizeBytes: Int) throws {
        guard FileManager.default.createFile(atPath: url.path, contents: nil) else {
            throw BlankImageError.writeFailed
        }
        if truncate(url.path, off_t(sizeBytes)) != 0 {
            try? FileManager.default.removeItem(at: url)
            throw BlankImageError.writeFailed
        }
    }
}
