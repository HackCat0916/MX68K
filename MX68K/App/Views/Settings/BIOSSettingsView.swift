import SwiftUI

struct BIOSSettingsView: View {
    @EnvironmentObject var settingsViewModel: SettingsViewModel

    #if os(iOS)
    /// P706 §B-6 — 取り込み(コピーイン)の失敗を **無言にしない**。macOS 経路では
    /// `FilePickerButton` の `onError` が呼ばれないため、この状態は iOS でしか存在しない。
    ///
    /// ★計画 §B-6 は「該当フィールドの直下に赤字 1 行」と書いているが、フィールド 5 枚
    /// それぞれの直下へ行を足すと `Section` の直下が 11 ビューになり、SwiftUI の
    /// `ViewBuilder` の上限(10)を超えてコンパイルできない。ファイル取り込みは同時に
    /// 1 件しか進行しないので、**どのフィールドで失敗したかを文言の先頭に持たせた
    /// 共有の 1 行**に集約する(情報は失われない)。5 行の ROM 欄の直後に置く。
    @State private var importError: String?

    @ViewBuilder
    private var importErrorRow: some View {
        if let importError {
            Text(importError)
                .font(.subheadline)
                .foregroundColor(.red)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
    #endif

    private let iplromSize: Int64 = 131_072
    private let cgromSize: Int64 = 786_432
    private let iplrom30Size: Int64 = 131_072
    private let scsiRomSize: Int64 = 8_192   // P274: SCSIINROM/SCSIEXROM は機種非依存で常時表示

    private func fileSize(of path: String) -> Int64? {
        guard !path.isEmpty else { return nil }
        let attrs = try? FileManager.default.attributesOfItem(atPath: path)
        return attrs?[.size] as? Int64
    }

    private func validationColor(expected: Int64, path: String) -> Color {
        guard !path.isEmpty else { return .primary }
        let size = fileSize(of: path)
        if size == nil {
            return .red
        }
        return size == expected ? .green : .red
    }

    private func validationText(expected: Int64, path: String, name: String) -> String {
        guard !path.isEmpty else { return ""
        }
        let size = fileSize(of: path)
        if size == nil {
            return String(localized: "\(name): File not found")
        }
        if size == expected {
            return String(localized: "\(name): OK (\(expected) bytes)")
        } else {
            return String(localized: "\(name): Invalid size (expected: \(expected) bytes, actual: \(size!) bytes)")
        }
    }

    var body: some View {
        Form {
            Section(header: Text("BIOS ROM Paths")) {
                // P706 §C-1: 5 つの `Button("Browse...") { NSOpenPanel… }` を
                // `FilePickerButton` へ差し替えた。macOS 分岐は同ボタンの
                // `#if os(macOS)` 側へ **逐語移設**されており(FilePickerButton.swift:56-64)、
                // macOS の実効挙動は変わらない。iOS ではアプリコンテナの bios/ へ
                // コピーインしたうえで、そのローカル絶対パスが返る。
                HStack {
                    Text("CGROM")
                    TextField("", text: $settingsViewModel.cgromPath)
                    FilePickerButton(titleKey: "Browse...", destination: .biosDir) { path in
                        settingsViewModel.cgromPath = path
                        #if os(iOS)
                        importError = nil
                        #endif
                    } onError: { message in
                        #if os(iOS)
                        importError = "CGROM — \(message)"
                        #endif
                    }
                }

                HStack {
                    Text("IPLROM")
                    TextField("", text: $settingsViewModel.iplromPath)
                    FilePickerButton(titleKey: "Browse...", destination: .biosDir) { path in
                        settingsViewModel.iplromPath = path
                        #if os(iOS)
                        importError = nil
                        #endif
                    } onError: { message in
                        #if os(iOS)
                        importError = "IPLROM — \(message)"
                        #endif
                    }
                }

                HStack {
                    Text("IPLROM30 (X68030)")
                    TextField("", text: $settingsViewModel.iplrom30Path)
                    FilePickerButton(titleKey: "Browse...", destination: .biosDir) { path in
                        settingsViewModel.iplrom30Path = path
                        #if os(iOS)
                        importError = nil
                        #endif
                    } onError: { message in
                        #if os(iOS)
                        importError = "IPLROM30 — \(message)"
                        #endif
                    }
                }

                // P274: SCSIINROM/SCSIEXROM は BIOS なので機種条件に関係なく常時表示。
                HStack {
                    Text("SCSIINROM")
                    TextField("", text: $settingsViewModel.scsiInRomPath)
                    FilePickerButton(titleKey: "Browse...", destination: .biosDir) { path in
                        settingsViewModel.scsiInRomPath = path
                        #if os(iOS)
                        importError = nil
                        #endif
                    } onError: { message in
                        #if os(iOS)
                        importError = "SCSIINROM — \(message)"
                        #endif
                    }
                }

                HStack {
                    Text("SCSIEXROM")
                    TextField("", text: $settingsViewModel.scsiExtRomPath)
                    FilePickerButton(titleKey: "Browse...", destination: .biosDir) { path in
                        settingsViewModel.scsiExtRomPath = path
                        #if os(iOS)
                        importError = nil
                        #endif
                    } onError: { message in
                        #if os(iOS)
                        importError = "SCSIEXROM — \(message)"
                        #endif
                    }
                }
                #if os(iOS)
                importErrorRow
                #endif

                #if os(macOS)
                Text("⚠ Takes effect after pressing Apply and then performing a hard reset (⌘R).")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #else
                // ★P706改訂1(2026-08-30): Applyの自動ハードリセットは廃止し、iOSにも
                //   明示的なHard Resetボタンを追加した(macOSの⌘Rと同じ意味論)。
                //   文言もmacOS側("Apply and then performing a hard reset (⌘R)")と
                //   揃える——旧文言("which restarts the emulated machine")は
                //   廃止済みの自動リセット挙動を指したまま取り残されていた。
                Text("⚠ Takes effect after pressing Apply and then tapping Hard Reset.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #endif
            }

            Section(header: Text("Validation")) {
                Text(validationText(expected: cgromSize, path: settingsViewModel.cgromPath, name: "CGROM.DAT"))
                    .foregroundColor(validationColor(expected: cgromSize, path: settingsViewModel.cgromPath))
                Text(validationText(expected: iplromSize, path: settingsViewModel.iplromPath, name: "IPLROM.DAT"))
                    .foregroundColor(validationColor(expected: iplromSize, path: settingsViewModel.iplromPath))
                if !settingsViewModel.iplrom30Path.isEmpty {
                    Text(validationText(expected: iplrom30Size, path: settingsViewModel.iplrom30Path, name: "IPLROM30.DAT"))
                        .foregroundColor(validationColor(expected: iplrom30Size, path: settingsViewModel.iplrom30Path))
                }
                if !settingsViewModel.scsiInRomPath.isEmpty {
                    Text(validationText(expected: scsiRomSize, path: settingsViewModel.scsiInRomPath, name: "SCSIINROM.DAT"))
                        .foregroundColor(validationColor(expected: scsiRomSize, path: settingsViewModel.scsiInRomPath))
                }
                if !settingsViewModel.scsiExtRomPath.isEmpty {
                    Text(validationText(expected: scsiRomSize, path: settingsViewModel.scsiExtRomPath, name: "SCSIEXROM.DAT"))
                        .foregroundColor(validationColor(expected: scsiRomSize, path: settingsViewModel.scsiExtRomPath))
                }
            }
        }
        // P580 — macOS 標準のグループスタイル(Form 自体がスクロール可能)。
        .formStyle(.grouped)
    }
}
