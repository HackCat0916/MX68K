//
//  ArchiveMountService.swift
//  MX68K
//
//  P554 — zip 圧縮ディスクイメージの展開サービス(Docs/10 A-5)。
//  /usr/bin/unzip をサブプロセスとして起動し、一時ディレクトリへ展開して
//  ディスクイメージ(拡張子: xdf/dim/d88/hdm/2hd/img)を探す。
//  7z は未検証のため本サイクルでは対象外。
//
//  FD イメージはマウント時に一括読込され、eject 時のみ書き戻される設計のため、
//  展開先のパスを既存のパスベース API(mx68k_fdd_insert)へ渡すだけでよく、
//  Core/Bridge 側の変更は不要。
//

import Foundation

enum ArchiveMountService {
    /// 展開後に「ディスクイメージ」とみなす拡張子。ToolbarView / EmulatorView の
    /// 通常マウント経路が受け付けている一覧と同一。
    static let diskImageExtensions = ["xdf", "dim", "d88", "hdm", "2hd", "img"]

    /// 一時ディレクトリ名の接頭辞。起動時スイープ(sweepStaleTempDirs)の
    /// 判定にも使うため、生成側と掃除側でこの 1 箇所を共有する。
    static let tempDirPrefix = "MX68K-zip-"

    struct ExtractedArchive {
        let tempDir: URL       // 展開先(cleanup 用に保持)
        let images: [URL]      // 見つかったディスクイメージ(1 個以上)
    }

    enum ExtractError: Error {
        case unzipFailed(status: Int32)
        case noDiskImageFound
    }

    /// 同期実行。zip 展開は数百ms〜数秒かかりうるため、呼出し元は既存の
    /// `viewModel.requestAutoPause()` / `releaseAutoPause()` で囲むこと
    /// (EmulatorViewModel.handleArchiveSelection がその責務を負う)。
    static func extract(zipPath: String) -> Result<ExtractedArchive, Error> {
        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(tempDirPrefix)\(UUID().uuidString)", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        } catch {
            return .failure(error)
        }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/unzip")
        process.arguments = ["-o", "-q", zipPath, "-d", tempDir.path]
        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            try? FileManager.default.removeItem(at: tempDir)
            return .failure(error)
        }
        guard process.terminationStatus == 0 else {
            try? FileManager.default.removeItem(at: tempDir)
            return .failure(ExtractError.unzipFailed(status: process.terminationStatus))
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
            return .failure(ExtractError.noDiskImageFound)
        }
        return .success(ExtractedArchive(tempDir: tempDir, images: found))
    }

    static func cleanup(_ tempDir: URL) {
        try? FileManager.default.removeItem(at: tempDir)
    }

    /// アプリ起動時のベストエフォート掃除(前回セッションのクラッシュ/強制終了で
    /// 残った一時ディレクトリを削除する)。MX68KApp.init() から 1 回だけ呼ぶ。
    static func sweepStaleTempDirs() {
        let tmp = FileManager.default.temporaryDirectory
        guard let items = try? FileManager.default.contentsOfDirectory(at: tmp, includingPropertiesForKeys: nil) else { return }
        for item in items where item.lastPathComponent.hasPrefix(tempDirPrefix) {
            try? FileManager.default.removeItem(at: item)
        }
    }
}
