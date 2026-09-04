//
//  IOSArchiveMountService.swift
//  MX68K-iOS
//
//  P729 — ZIP 圧縮ディスクイメージの展開。
//
//  macOS 版 `ArchiveMountService.swift`(P554)の移植だが、**展開手段だけが
//  違う**。macOS 版は `/usr/bin/unzip` をサブプロセスとして起動するが、
//  iOS ではこの方式が根本的に使えない —— iOS アプリサンドボックスはサブ
//  プロセス起動を許可せず、`Process` クラス自体が iOS API に存在せず、
//  `/usr/bin/unzip` も同梱されていない。そのため本サイクルは展開処理を
//  ZIPFoundation(SPM 依存、本プロジェクト初の外部依存)へ委ねる。
//
//  ★これまでの iOS 機能(セーブステート・SASI/SCSI 等)のような「既存 Bridge
//    API を UI 層だけ差し替えて逐語移植」というパターンが使えない唯一の箇所
//    である。展開ロジック自体は新規だが、**その周りの設計(拡張子一覧・
//    2 段階の展開→事後スキャン・一時ディレクトリのスイープ)は macOS 版から
//    逐語で持ち込み、新規に発明しない**。
//

import Foundation
import ZIPFoundation

enum IOSArchiveMountService {
    /// macOS `ArchiveMountService.diskImageExtensions`(:20)と同一。
    static let diskImageExtensions = ["xdf", "dim", "d88", "hdm", "2hd", "img"]

    /// §記号表——iOS固有の接頭辞(macOS版の"MX68K-zip-"とは意図的に分離)。
    static let tempDirPrefix = "MX68K-ios-zip-"

    struct ExtractedArchive {
        let tempDir: URL
        let images: [URL]
    }

    enum ExtractError: Error {
        case accessDenied
        case archiveOpenFailed(Error)
        case extractFailed(Error)
        case noDiskImageFound
    }

    /// 同期実行。zip展開は数百ms〜数秒かかりうるため、呼出し元
    /// (`MX68KiOSViewModel.handleArchiveSelection`)がユーザーの体感を
    /// 損なわないよう配慮すること(macOS版のrequestAutoPause相当は
    /// iOSに無いため——iOSはバックグラウンド以外は常時run_frame動作中で、
    /// 展開中の一時的な体感カクつきは許容する。既存iOS機能もファイル
    /// I/O中に同期実行する箇所は他に無いため、新規の設計判断が必要
    /// だが、数秒程度の同期処理はメインスレッドをブロックするとはいえ
    /// ユーザー操作(ファイルピッカーからの選択直後)の一環として自然な
    /// 待ち時間の範囲内と判断する)。
    static func extract(zipURL: URL) -> Result<ExtractedArchive, Error> {
        guard zipURL.startAccessingSecurityScopedResource() else {
            emitLine("op=extract result=accessdenied")
            return .failure(ExtractError.accessDenied)
        }
        defer { zipURL.stopAccessingSecurityScopedResource() }

        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(tempDirPrefix)\(UUID().uuidString)", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        } catch {
            emitLine("op=extract result=tempdirfail error=\(error)")
            return .failure(error)
        }

        let archive: Archive
        do {
            archive = try Archive(url: zipURL, accessMode: .read)
        } catch {
            try? FileManager.default.removeItem(at: tempDir)
            emitLine("op=extract result=archiveopenfail error=\(error)")
            return .failure(ExtractError.archiveOpenFailed(error))
        }

        // macOS版(unzip -o -q 一括展開→FileManager.enumeratorで事後スキャン)と
        // 同じ2段階方式——全エントリを展開してから拡張子でフィルタする
        // (エントリ列挙中に選別すると、ディレクトリエントリの扱い漏れ等の
        // 取りこぼしが起きやすいため、macOS版の設計をそのまま踏襲)。
        for entry in archive {
            let destURL = tempDir.appendingPathComponent(entry.path)
            do {
                _ = try archive.extract(entry, to: destURL)
            } catch {
                // 1エントリの展開失敗で全体を諦めない(macOS版のunzipも
                // 個別エントリのエラーは黙って先へ進む挙動——`-o`オプション
                // 相当)。ただしログには残す(自己反証可能性)。
                emitLine("op=extract entry=\(entry.path) result=entryfail error=\(error)")
            }
        }

        var found: [URL] = []
        if let enumerator = FileManager.default.enumerator(at: tempDir, includingPropertiesForKeys: nil) {
            for case let fileURL as URL in enumerator {
                if diskImageExtensions.contains(fileURL.pathExtension.lowercased()) {
                    found.append(fileURL)
                }
            }
        }
        found.sort { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }

        if found.isEmpty {
            try? FileManager.default.removeItem(at: tempDir)
            emitLine("op=extract result=nodiskimage")
            return .failure(ExtractError.noDiskImageFound)
        }
        emitLine("op=extract result=ok images=\(found.count)")
        return .success(ExtractedArchive(tempDir: tempDir, images: found))
    }

    static func cleanup(_ tempDir: URL) {
        try? FileManager.default.removeItem(at: tempDir)
    }

    /// macOS版`sweepStaleTempDirs()`(:86-92)の逐語移植。
    /// `MX68KiOSViewModel.start(config:)`の先頭から1回だけ呼ぶ。
    static func sweepStaleTempDirs() {
        let tmp = FileManager.default.temporaryDirectory
        guard let items = try? FileManager.default.contentsOfDirectory(at: tmp, includingPropertiesForKeys: nil) else { return }
        for item in items where item.lastPathComponent.hasPrefix(tempDirPrefix) {
            try? FileManager.default.removeItem(at: item)
        }
    }

    private static func emitLine(_ line: String) {
        mx68k_log("[Swift][iOS][P729-ARCHIVE] \(line)")
    }
}
