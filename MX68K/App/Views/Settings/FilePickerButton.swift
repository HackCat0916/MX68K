import SwiftUI
#if os(iOS)
import UniformTypeIdentifiers
#endif

// P706 §B — クロスプラットフォームのファイル選択ボタン。
//
// 呼び出し側はプラットフォームを意識しない。受け取るのは常に「C API へ
// そのまま渡せる生のファイルシステムパス」である。
//
//  * macOS: NSOpenPanel をそのまま使い、選ばれたパスを **コピーせずに** 返す
//           (BIOSSettingsView.swift:47-53 の逐語移設。destination は無視する)。
//  * iOS  : .fileImporter で選ばせ、アプリコンテナ内へ **コピーイン** してから
//           コピー先の絶対パスを返す。Bridge の C API
//           (mx68k_set_bios_path / mx68k_fdd_insert) は const char* しか受け取らず
//           Core 側が任意の時点で fopen するため、セキュリティスコープ付き URL の
//           ままでは動作しない(P706 計画 §根拠種別 E-3)。

/// コピーイン先の区分。macOS では無視される(コピーしない = 現行挙動と同一)。
enum FilePickerDestination {
    /// 選ばれたパスをそのまま使う(コピーしない)。macOS の既定。
    case inPlace
    /// iOS: ~/Library/Application Support/MX68K/bios/ へコピーイン。
    case biosDir
    /// iOS: ~/Library/Application Support/MX68K/disks/ へコピーイン。
    case disksDir
}

struct FilePickerButton: View {
    let titleKey: LocalizedStringKey
    let destination: FilePickerDestination
    let onPick: (String) -> Void
    let onError: (String) -> Void

    #if os(iOS)
    @State private var isPresenting = false
    #endif

    // 明示的な init。stored property に private なものがあると
    // 暗黙のメンバワイズ init も private 化されるため、他ファイルからの
    // 生成を保証するために自前で書く。
    init(titleKey: LocalizedStringKey,
         destination: FilePickerDestination,
         onPick: @escaping (String) -> Void,
         onError: @escaping (String) -> Void = { _ in }) {
        self.titleKey = titleKey
        self.destination = destination
        self.onPick = onPick
        self.onError = onError
    }

    var body: some View {
        #if os(macOS)
        // ★ BIOSSettingsView.swift:47-53(P706 適用前)の逐語移設。
        //   代入先だけが onPick(url.path) に置き換わっている。
        Button(titleKey) {
            let panel = NSOpenPanel()
            panel.allowsMultipleSelection = false
            panel.canChooseFiles = true
            panel.canChooseDirectories = false
            if panel.runModal() == .OK, let url = panel.url {
                onPick(url.path)
            }
        }
        #else
        Button(titleKey) {
            ImportedFileStore.logOpen(destination: destination)
            isPresenting = true
        }
        // X68000 のディスクイメージ/ROM は宣言済み UTType を持たないため、
        // 種類で絞らずに受け取り、サイズ検証(BIOSSettingsView の Validation 節)で
        // 後段判定する。
        .fileImporter(isPresented: $isPresenting,
                      allowedContentTypes: [.data],
                      allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                guard let url = urls.first else {
                    ImportedFileStore.logCancel(destination: destination)
                    return
                }
                do {
                    let path = try ImportedFileStore.importFile(from: url, to: destination)
                    onPick(path)
                } catch {
                    onError(ImportedFileStore.message(for: error))
                }
            case .failure(let error):
                if (error as NSError).code == NSUserCancelledError {
                    ImportedFileStore.logCancel(destination: destination)
                } else {
                    ImportedFileStore.logPickerFailure(destination: destination, error: error)
                    onError(ImportedFileStore.message(for: error))
                }
            }
        }
        #endif
    }
}

#if os(iOS)

// P706 §B-5 — コピーイン。
//
// 一時ファイルへコピー → バイト数検証 → アトミックにリネーム、という
// 3 段構えにして「途中で失敗したときに中途半端なファイルが残らない」ことを保証する
// (§残留リスク R-1)。どの経路を通っても必ず 1 行ログを出す(§自己反証可能性)。
enum ImportedFileStore {

    enum ImportError: Error {
        case sizeMismatch(src: Int64, dst: Int64)
    }

    // §記号表 — macOS 実装(ConfigManager.swift:80,83)と同一の組み立て方。
    static var appSupportDir: String {
        NSHomeDirectory() + "/Library/Application Support/MX68K"
    }

    static func directory(for destination: FilePickerDestination) -> String {
        switch destination {
        case .biosDir:  return appSupportDir + "/bios"
        case .disksDir: return appSupportDir + "/disks"
        case .inPlace:  return appSupportDir
        }
    }

    static func kind(for destination: FilePickerDestination) -> String {
        switch destination {
        case .biosDir:  return "bios"
        case .disksDir: return "disk"
        case .inPlace:  return "inplace"
        }
    }

    /// 選ばれた URL をコピーインし、コピー先の絶対パスを返す。
    @discardableResult
    static func importFile(from url: URL, to destination: FilePickerDestination) throws -> String {
        let fm = FileManager.default
        let destDir = directory(for: destination)
        let name = url.lastPathComponent
        let finalPath = (destDir as NSString).appendingPathComponent(name)
        let tmpPath = (destDir as NSString)
            .appendingPathComponent(".incoming-\(UUID().uuidString)-\(name)")

        // §B-5-2: 戻り値が false でも即エラーにせず先へ進む。.fileImporter 経由の
        // URL ではスコープ取得が不要な場合があり、ここで打ち切ると正常系を誤って落とす
        // (§根拠種別 E-5)。生値はログの scoped= に出す。
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }

        var srcBytes: Int64 = -1
        var dstBytes: Int64 = -1

        func emit(_ result: String, _ err: Error?) {
            log(kind: kind(for: destination), scoped: scoped, src: name,
                srcBytes: srcBytes, dst: finalPath, dstBytes: dstBytes,
                result: result, err: err)
        }

        do {
            try fm.createDirectory(atPath: destDir, withIntermediateDirectories: true)
        } catch {
            emit("copyfail", error)
            throw error
        }

        srcBytes = fileSize(of: url)

        // 万一残骸があっても上書きできるよう、一時ファイルは事前に掃除する。
        try? fm.removeItem(atPath: tmpPath)
        do {
            try fm.copyItem(at: url, to: URL(fileURLWithPath: tmpPath))
        } catch {
            try? fm.removeItem(atPath: tmpPath)
            emit("copyfail", error)
            throw error
        }

        dstBytes = fileSize(of: URL(fileURLWithPath: tmpPath))
        if srcBytes >= 0 && dstBytes != srcBytes {
            try? fm.removeItem(atPath: tmpPath)
            emit("sizemismatch", nil)
            throw ImportError.sizeMismatch(src: srcBytes, dst: dstBytes)
        }

        do {
            if fm.fileExists(atPath: finalPath) {
                try fm.removeItem(atPath: finalPath)
            }
            // 同一ボリューム内の rename なのでアトミック。
            try fm.moveItem(atPath: tmpPath, toPath: finalPath)
        } catch {
            try? fm.removeItem(atPath: tmpPath)
            emit("movefail", error)
            throw error
        }

        emit("ok", nil)
        return finalPath
    }

    /// 表示用のエラー文言。無言で何も起きない状態を作らないための材料(§B-6)。
    static func message(for error: Error) -> String {
        if case ImportError.sizeMismatch(let src, let dst) = error {
            return "Import failed: copied \(dst) of \(src) bytes."
        }
        let ns = error as NSError
        return "Import failed: \(ns.localizedDescription) (\(ns.domain):\(ns.code))"
    }

    // ---- ログ(§自己反証可能性 プローブ P706-IMPORT) ----

    static func logOpen(destination: FilePickerDestination) {
        emitLine("[P706-IMPORT-OPEN] kind=\(kind(for: destination)) f=\(mx68k_get_frame_num())")
    }

    static func logCancel(destination: FilePickerDestination) {
        log(kind: kind(for: destination), scoped: false, src: "-",
            srcBytes: -1, dst: "-", dstBytes: -1, result: "cancel", err: nil)
    }

    static func logPickerFailure(destination: FilePickerDestination, error: Error) {
        log(kind: kind(for: destination), scoped: false, src: "-",
            srcBytes: -1, dst: "-", dstBytes: -1, result: "pickerfail", err: error)
    }

    // 派生値 result= には、それを導出した生値 srcBytes / dstBytes / err を必ず併記する。
    private static func log(kind: String, scoped: Bool, src: String,
                            srcBytes: Int64, dst: String, dstBytes: Int64,
                            result: String, err: Error?) {
        var errText = "-"
        if let err = err {
            let ns = err as NSError
            errText = "\(ns.domain):\(ns.code)"
        }
        emitLine("[P706-IMPORT] kind=\(kind) scoped=\(scoped ? 1 : 0) src=\(src) " +
                 "srcBytes=\(srcBytes) dst=\(dst) dstBytes=\(dstBytes) " +
                 "result=\(result) err=\(errText) f=\(mx68k_get_frame_num())")
    }

    private static func emitLine(_ line: String) {
        line.withCString { mx68k_log($0) }
    }

    /// バイト数。取得できなければ -1(生値としてそのままログへ出す)。
    private static func fileSize(of url: URL) -> Int64 {
        if let values = try? url.resourceValues(forKeys: [.fileSizeKey]),
           let size = values.fileSize {
            return Int64(size)
        }
        if let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
           let size = attrs[.size] as? Int64 {
            return size
        }
        return -1
    }
}

#endif
