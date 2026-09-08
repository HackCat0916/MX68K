import SwiftUI
import AppKit

/// P744 — ログビューワー(⌘⌥V)。`~/Library/Application Support/MX68K/debug.log`
/// をライブテール表示する。
///
/// 既存モニタとの決定的な違いは **Bridge 関数を一切呼ばない** 点
/// (`MemoryMapMonitorView`(P600)と同型の「ObservableObject + Timer ポーリング」
/// 構造だけを踏襲し、データ源はファイル読み取りのみで完結する)。
/// エミュレーションスレッド・`debug_log_mutex` のいずれにも触れないため、
/// ウィンドウを開かない限り本ファイルのコードは一度も実行されない。
///
/// `debug_log()` は `#ifdef DEBUG` ブロック内でのみ実体を持つ
/// (`Bridge/EmulatorBridge.c`)。Release ビルドでは debug.log 自体が生成されない
/// ため、その場合は空状態メッセージを表示する。
struct LogLine: Identifiable {
    let id: Int
    let text: String
}

final class LogViewerModel: ObservableObject {
    /// 初回シード読み込みの上限。大きすぎるログでも起動が重くならない安全マージン。
    static let seedBytes: UInt64 = 64 * 1024
    /// Picker 用の保持行数プリセット。
    static let maxLinesOptions: [Int] = [500, 1000, 2000, 5000, 10000, 20000]

    /// 表示保持行数の上限。超過分は古い行から捨てる(P745 でユーザー可変化)。
    /// ★`didSet` で即座に切り詰める: 上限を下げた場合、次のポーリング(0.5 秒)を
    ///   待たずにその場で古い行が消えることをユーザーは期待する。
    @Published var maxLines: Int = 2000 {
        didSet { trimToMaxLines() }
    }

    @Published private(set) var lines: [LogLine] = []
    @Published private(set) var fileMissing = false
    @Published var isPaused = false

    private var nextID = 0
    private var fileHandle: FileHandle?
    private var pendingOffset: UInt64 = 0
    /// 前回読み取りの末尾にあった、まだ改行が来ていない断片。
    private var partialLine: String = ""
    private var timer: Timer?

    private var logURL: URL {
        // ConfigManager.swift と同じ構成手順(Application Support/MX68K/)。
        let appSupport = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask).first!
        return appSupport.appendingPathComponent("MX68K", isDirectory: true)
            .appendingPathComponent("debug.log")
    }

    func startTailing() {
        stopTailing()
        openAndSeed()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.poll()
        }
    }

    func stopTailing() {
        timer?.invalidate()
        timer = nil
        try? fileHandle?.close()
        fileHandle = nil
    }

    func clearDisplay() {
        lines = []
    }

    /// 配列を保持上限まで切り詰めて返す(`lines` へは書き込まない)。
    /// ★`ingest()` が `lines` へ 1 回だけ代入する構造を保つための純関数
    ///   ——ここで `lines` を直接触ると `@Published` の二重 publish になる。
    private func trimmed(_ array: [LogLine]) -> [LogLine] {
        guard array.count > maxLines else { return array }
        return Array(array.suffix(maxLines))
    }

    /// Picker で上限が下げられた直後に、既存の `lines` をその場で切り詰める専用パス。
    private func trimToMaxLines() {
        lines = trimmed(lines)
    }

    private func openAndSeed() {
        try? fileHandle?.close()
        fileHandle = nil
        partialLine = ""
        let url = logURL
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
              let size = attrs[.size] as? UInt64,
              let handle = try? FileHandle(forReadingFrom: url) else {
            fileMissing = true
            pendingOffset = 0
            return
        }
        fileMissing = false
        let seedStart = size > Self.seedBytes ? size - Self.seedBytes : 0
        handle.seek(toFileOffset: seedStart)
        let data = handle.readDataToEndOfFile()
        fileHandle = handle
        pendingOffset = size
        ingest(data, isSeed: seedStart > 0)
    }

    private func poll() {
        guard let handle = fileHandle else {
            // 前回オープン失敗 — ファイルが後から生成されるケースに備えて再試行。
            // ★Pause ガードより前に置く: Pause 中でもファイル自体の生成には
            //   反応できる必要がある(下のローテーション検出と同じ理由)。
            if !isPaused { openAndSeed() }
            return
        }
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: logURL.path),
              let size = attrs[.size] as? UInt64 else { return }
        // ★Code Review 指摘(P744): ローテーション検出は Pause ガードより必ず先に
        //   行う。Pause 中に旧 fileHandle が指す inode が smoke_test.sh 等により
        //   unlink / 再生成された場合、Pause 解除後に旧 fd(サイズ固定のまま)へ
        //   readData し続けると増分ゼロが恒久的に続き、ウィンドウを開き直すまで
        //   復旧できない「固まったまま」状態になる。ローテーションは Pause の
        //   対象外の帯域外イベントとして扱い、Pause 中でも即座に再シードする。
        //   ★この順序は load-bearing — 「整理」目的で入れ替えないこと。
        if size < pendingOffset {
            openAndSeed()
            return
        }
        guard !isPaused else { return }
        guard size > pendingOffset else { return }
        handle.seek(toFileOffset: pendingOffset)
        let data = handle.readData(ofLength: Int(size - pendingOffset))
        pendingOffset = size
        ingest(data, isSeed: false)
    }

    private func ingest(_ data: Data, isSeed: Bool) {
        guard !data.isEmpty else { return }
        let text = partialLine + (String(data: data, encoding: .utf8) ?? "")
        var parts = text.components(separatedBy: "\n")
        // 末尾要素は次回への繰越候補(まだ改行が来ていない可能性がある)。
        partialLine = parts.removeLast()
        // シード読み込みがファイル途中から始まった場合、先頭要素は不完全行の
        // 可能性があるため行として採用しない(次回以降は完全行のみ)。
        if isSeed, !parts.isEmpty {
            parts.removeFirst()
        }
        guard !parts.isEmpty else { return }
        var newLines = lines
        for p in parts {
            newLines.append(LogLine(id: nextID, text: p))
            nextID += 1
        }
        lines = trimmed(newLines)
    }
}

struct LogViewerView: View {
    @StateObject private var model = LogViewerModel()
    @State private var filterText = ""
    @State private var autoScroll = true
    /// Clear/Copy の操作結果を短時間だけ表示する(P745)。
    @State private var statusMessage: String? = nil
    /// 連打時に古いタイマーが新しいメッセージを消さないための世代カウンタ。
    @State private var statusGeneration = 0

    private var filteredLines: [LogLine] {
        guard !filterText.isEmpty else { return model.lines }
        return model.lines.filter { $0.text.localizedCaseInsensitiveContains(filterText) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Log Viewer").font(.headline)
            controls
            Divider()
            if model.fileMissing {
                Text("debug.log not found yet (Debug builds only — Release builds do not write this file).")
                    .foregroundColor(.secondary)
                    .padding()
                Spacer()
            } else {
                logList
            }
            Text("\(filteredLines.count) / \(model.lines.count) lines shown (last \(model.maxLines) retained)")
                .font(.caption)
                .foregroundColor(.secondary)
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 720, minHeight: 480, alignment: .topLeading)
        .onAppear { model.startTailing() }
        .onDisappear { model.stopTailing() }
    }

    private var controls: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 8) {
                TextField("Filter (e.g. P657, DMAC)", text: $filterText)
                    .frame(width: 220)
                Picker("Max lines", selection: $model.maxLines) {
                    ForEach(LogViewerModel.maxLinesOptions, id: \.self) { n in
                        Text("\(n)").tag(n)
                    }
                }
                .frame(width: 140)
                .help("Number of most-recent lines retained in this window. Lowering this immediately discards older lines; debug.log itself is untouched.")
                Toggle(isOn: $autoScroll) { Text("Auto-scroll") }
                    .help("Automatically scrolls to the newest line as it arrives.")
                Toggle(isOn: $model.isPaused) {
                    model.isPaused ? Text("Resume") : Text("Pause")
                }
                .help("Pauses live tailing. Log rotation (e.g. debug.log being cleared) is still detected and refreshes the view even while paused.")
                Button("Clear") {
                    let n = model.lines.count
                    model.clearDisplay()
                    showStatus(String(localized: "Cleared \(n) lines from view"))
                }
                .help("Clears the lines currently shown in this window. debug.log itself is untouched — tailing continues, so new lines keep appearing.")
                Button("Copy") {
                    let n = filteredLines.count
                    guard n > 0 else {
                        showStatus(String(localized: "Nothing to copy"))
                        return
                    }
                    copyToClipboard()
                    let message = n == 1
                        ? String(localized: "Copied 1 line to clipboard")
                        : String(localized: "Copied \(n) lines to clipboard")
                    showStatus(message)
                }
                .help("Copies the lines currently visible (after the filter above) to the clipboard as plain text.")
            }
            // 出現/消失でレイアウトが揺れないよう、常に 1 行ぶんの高さを確保する。
            Text(statusMessage ?? " ")
                .font(.caption)
                .foregroundColor(.accentColor)
                .frame(height: 14, alignment: .leading)
        }
    }

    /// 操作結果メッセージを 1.5 秒だけ表示する。世代カウンタにより、連打時に
    /// 古いタイマーが新しいメッセージを消してしまうことを防ぐ。
    private func showStatus(_ text: String) {
        statusMessage = text
        statusGeneration += 1
        let generation = statusGeneration
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
            if statusGeneration == generation {
                statusMessage = nil
            }
        }
    }

    private var logList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(filteredLines) { line in
                        Text(line.text)
                            .font(.system(size: 11, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .id(line.id)
                    }
                }
                .textSelection(.enabled)
            }
            .onChange(of: filteredLines.last?.id) { lastID in
                guard autoScroll, let lastID = lastID else { return }
                withAnimation(nil) { proxy.scrollTo(lastID, anchor: .bottom) }
            }
        }
    }

    private func copyToClipboard() {
        let text = filteredLines.map(\.text).joined(separator: "\n")
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(text, forType: .string)
    }
}
