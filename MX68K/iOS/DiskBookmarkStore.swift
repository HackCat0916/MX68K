//
//  DiskBookmarkStore.swift
//  MX68K-iOS
//
//  P712 §方針1 — SASI / 内蔵SCSI / MO / CD の大容量イメージを iOS で扱うための、
//  コピーインしない(その場アクセス)方式のブックマーク永続化層。
//
//  FDD0/1・BIOS が使う `FilePickerButton`(コピーイン方式、~/…/MX68K/disks・bios/)とは
//  意図的に別方式 —— ユーザーの選択(2026-08-30、大容量イメージのコピー時間 /
//  ストレージ二重消費を避ける)。既存のコピーイン経路には一切手を入れない。
//
//  ★自己反証可能性(§R-D): 保存/解決の失敗を無言(`try?` の握り潰し)にしない。
//    `ImportedFileStore` が確立した「全経路で 1 行ログ」を踏襲し、`[P712-DISKBM]`
//    タグで結果を必ず出す。`save` は成否を `Bool` で返し、呼び出し側が
//    「ライブマウントは成功したが次回起動での復元は保証できない」を区別できる。
//

#if os(iOS)
import Foundation

enum DiskBookmarkStore {

    // キーは "hdd0".."hdd7" / "scsi0".."scsi6" / "mo" / "cd"。
    // 保存先の組み立て方は `ImportedFileStore.appSupportDir`(FilePickerButton.swift)と
    // 同一の規約に従う。
    private static var storePath: String {
        NSHomeDirectory() + "/Library/Application Support/MX68K/disk_bookmarks.json"
    }

    private static func load() -> [String: Data] {
        guard let data = FileManager.default.contents(atPath: storePath),
              let dict = try? JSONDecoder().decode([String: Data].self, from: data)
        else { return [:] }
        return dict
    }

    @discardableResult
    private static func persist(_ dict: [String: Data]) -> Bool {
        guard let data = try? JSONEncoder().encode(dict) else { return false }
        try? FileManager.default.createDirectory(
            atPath: (storePath as NSString).deletingLastPathComponent,
            withIntermediateDirectories: true)
        do {
            try data.write(to: URL(fileURLWithPath: storePath), options: .atomic)
            return true
        } catch {
            return false
        }
    }

    /// Select… で選ばれた URL をブックマークとして保存する。
    /// 呼び出し側は既に fileImporter のスコープ内(url 自体がセキュリティスコープ付き)。
    /// - Returns: 保存に成功したか。
    @discardableResult
    static func save(key: String, url: URL) -> Bool {
        guard let bookmark = try? url.bookmarkData() else {
            emitLine("[P712-DISKBM] op=save key=\(key) result=bookmarkfail")
            return false
        }
        var dict = load()
        dict[key] = bookmark
        let ok = persist(dict)
        emitLine("[P712-DISKBM] op=save key=\(key) result=\(ok ? "ok" : "persistfail")")
        return ok
    }

    static func remove(key: String) {
        var dict = load()
        dict.removeValue(forKey: key)
        let ok = persist(dict)
        emitLine("[P712-DISKBM] op=remove key=\(key) result=\(ok ? "ok" : "persistfail")")
    }

    /// 保存済みブックマークを解決する。スコープアクセスは呼び出し側が
    /// `startAccessingSecurityScopedResource()` で明示的に開始/終了すること
    /// (この store は開始/終了のライフサイクルを管理しない —— マウント中は
    /// アプリのプロセス生存期間ずっとアクセスを開いたままにする必要があるため、
    /// 呼び出し側 = `MX68KiOSViewModel` が「いつ閉じるか」を判断する)。
    static func resolve(key: String) -> URL? {
        guard let bookmark = load()[key] else {
            emitLine("[P712-DISKBM] op=resolve key=\(key) result=nobookmark")
            return nil
        }
        var isStale = false
        guard let url = try? URL(resolvingBookmarkData: bookmark,
                                 options: [], relativeTo: nil,
                                 bookmarkDataIsStale: &isStale) else {
            emitLine("[P712-DISKBM] op=resolve key=\(key) result=resolvefail")
            return nil
        }
        if isStale {
            // 新しいブックマークで置き換える(iOS では典型的に発生しないが、
            // 発生した場合に無言で失敗させない)。
            save(key: key, url: url)
        }
        emitLine("[P712-DISKBM] op=resolve key=\(key) result=ok stale=\(isStale ? 1 : 0)")
        return url
    }

    private static func emitLine(_ line: String) {
        line.withCString { mx68k_log($0) }
    }
}
#endif
