import SwiftUI
import AppKit

struct GeneralSettingsView: View {
    @AppStorage("appLanguage") private var appLanguage = "system"
    @AppStorage("screenshotDirectory") private var screenshotDir = ""
    // P697: 動画録画の保存先(screenshotDirectory と同型 —— 空 = 既定 ~/Movies/MX68K)。
    @AppStorage("videoRecordingDirectory") private var videoRecordingDir = ""
    // P199 — 表示フィルタ(メニューと同一キーを共有・@AppStorage 直書き=即時反映)。
    @AppStorage("displayFilter") private var displayFilter: String = "smooth"
    // P664: 走査線エフェクト(MVP: オン/オフのみ)。displayFilterと同じ
    // @AppStorage直書きパターン(メニュー項目は本サイクルでは追加しない)。
    @AppStorage("scanlineEffect") private var scanlineEffect: Bool = false
    // P1007: 設定画面を開いている間の自動一時停止(P229)をON/OFF可能にする。
    // 既定OFF(iOSの挙動[P706 §C-2、一時停止しない]に合わせる、ユーザー指示)。
    // SettingsView 側でも同じキーを独立宣言して読む(displayFilter と同じパターン)。
    @AppStorage("pauseWhileSettingsOpen") private var pauseWhileSettingsOpen: Bool = false
    @EnvironmentObject private var configManager: ConfigManager

    // P1090: LogViewerView.swift の loggingBinding と同一実装(同じ Bridge 実行時フラグを
    // 共有する)。永続化は Release ビルドでの変更のみ——Debug ビルドでの一時操作が
    // config.json 経由で Release ビルドの既定値へ波及しないようにするため。
    private var loggingBinding: Binding<Bool> {
        Binding(
            get: { mx68k_get_debug_log_enabled() != 0 },
            set: { newValue in
                mx68k_set_debug_log_enabled(newValue ? 1 : 0)
                if mx68k_is_debug_build() == 0 {
                    configManager.config.performance.debugLogEnabled = newValue
                    configManager.save()
                }
            }
        )
    }

    var body: some View {
        Form {
            Section(header: Text("General")) {
                Picker("Display Language", selection: $appLanguage) {
                    ForEach(AppLanguage.allCases) { lang in
                        Text(lang.displayName).tag(lang.rawValue)
                    }
                }
                .onChange(of: appLanguage) { newValue in
                    LocalizationManager.apply(AppLanguage(rawValue: newValue) ?? .system)
                }

                // P581: 注意文の先頭記号を `⚠` へ統一(リテラル変更 = キー変更のため
                // xcstrings へ新リテラルの ja 訳を追加済み)。
                Text("⚠ Changes take effect at next launch.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                // P1007: @AppStorage 直書きのため即時反映(Apply 不要)。
                Toggle("Pause While Settings Window Is Open", isOn: $pauseWhileSettingsOpen)

                // P1090: Bridge 関数を直接呼ぶため即時反映(Log Viewer のトグルと同一設定)。
                Toggle("Enable Debug Log", isOn: loggingBinding)
                Text("⚠ Debug logging increases log output and can affect audio timing (e.g. internal MIDI). Leave this off unless troubleshooting.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Section(header: Text("Screenshot Folder")) {
                Text(screenshotDir.isEmpty
                     ? String(localized: "Default (~/Pictures/MX68K)")
                     : screenshotDir)
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    // P578: SASI/SCSI のファイル名列と同型(調査報告 §1-3)。中略は
                    // 意図的設計なので維持し、ホバーでフルパスを確認できるようにする。
                    // 折り返し(.fixedSize)は lineLimit(1) と趣旨が衝突するため付けない。
                    .help(screenshotDir.isEmpty
                          ? String(localized: "Default (~/Pictures/MX68K)")
                          : screenshotDir)

                HStack {
                    Button("Change…") {
                        let panel = NSOpenPanel()
                        panel.canChooseDirectories = true
                        panel.canChooseFiles = false
                        panel.allowsMultipleSelection = false
                        if panel.runModal() == .OK, let url = panel.url {
                            screenshotDir = url.path
                        }
                    }
                    if !screenshotDir.isEmpty {
                        Button("Reset to Default") { screenshotDir = "" }
                    }
                }
            }

            // P697 — 動画録画の保存先。上の「Screenshot Folder」節と完全に同型
            // (中略表示 + ホバーでフルパス + Change… / Reset to Default)。
            Section(header: Text("Video Recording Folder")) {
                Text(videoRecordingDir.isEmpty
                     ? String(localized: "Default (~/Movies/MX68K)")
                     : videoRecordingDir)
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(videoRecordingDir.isEmpty
                          ? String(localized: "Default (~/Movies/MX68K)")
                          : videoRecordingDir)

                HStack {
                    Button("Change…") {
                        let panel = NSOpenPanel()
                        panel.canChooseDirectories = true
                        panel.canChooseFiles = false
                        panel.allowsMultipleSelection = false
                        if panel.runModal() == .OK, let url = panel.url {
                            videoRecordingDir = url.path
                        }
                    }
                    if !videoRecordingDir.isEmpty {
                        Button("Reset to Default") { videoRecordingDir = "" }
                    }
                }
            }

            Section(header: Text("表示フィルタ")) {
                Picker(selection: $displayFilter) {
                    ForEach(DisplayFilter.allCases) { f in
                        Text(f.labelKey).tag(f.rawValue)
                    }
                } label: {
                    Text("表示フィルタ")
                }

                Text("表示フィルタの説明")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                Toggle("走査線エフェクト", isOn: $scanlineEffect)
            }
        }
        // P580 — macOS 標準のグループスタイル。Form 自体がスクロール可能になるため、
        // P579 で外側に付けていた `ScrollView`(文字欠落の原因)が不要になる。
        .formStyle(.grouped)
    }
}
