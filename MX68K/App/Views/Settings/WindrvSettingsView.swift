import SwiftUI

// P642: Windrv 設定タブ。Mac の 1 フォルダを X68000 ゲストから 1 台のドライブとして
// 見せるホスト共有機能。ゲスト側ドライバは標準ブートディスク同梱の
// `\SYS\WindrvXM.SYS`(XM6 用の実バイナリ、無改造)を使う。
//
// ★P647: 書込み(作成・上書き・改名・削除)は共有トグルとは**独立した第 2 の
//   トグル**(既定 OFF)で制御する。共有を有効にしただけでは従来どおり
//   読み取り専用で、「書き込みできません」エラーが返る。
//
// ★設計方針(ユーザーとの協議): 公開範囲の判断とリスク受容を利用者自身に委ねる。
//   既定は OFF、フォルダは NSOpenPanel での明示選択のみ、警告文言は常設表示。
struct WindrvSettingsView: View {
    @EnvironmentObject var settingsViewModel: SettingsViewModel
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    // P457 と同じ「wired 値基準」の状態表示。設定を Apply しただけでは変わらず、
    // ハードリセット(⌘R)を実行して初めて true になる。
    private var isWired: Bool {
        mx68k_get_windrv_installed()
    }

    // P647: 書込みも同じく設定値ではなく配線確定値を表示する。
    private var isWriteWired: Bool {
        mx68k_get_windrv_write_wired()
    }

    private var selectedFolderName: String {
        settingsViewModel.windrvHostPath.isEmpty
            ? String(localized: "(no folder selected)")
            : (settingsViewModel.windrvHostPath as NSString).lastPathComponent
    }

    var body: some View {
        Form {
            Section(header: Text("Mac Folder Sharing (Windrv)")) {
                Toggle("Share a Mac folder with the guest",
                       isOn: $settingsViewModel.windrvEnabled)

                HStack {
                    Text("Folder")
                    Text(selectedFolderName)
                        .foregroundColor(settingsViewModel.windrvHostPath.isEmpty ? .secondary : .primary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        // ホバーでフルパスを確認できるようにする(P578 と同じ方針)。
                        .help(settingsViewModel.windrvHostPath)
                    Spacer()
                    Button("Select…") {
                        // ★フォルダ選択なので canChooseDirectories/canChooseFiles を
                        // SASI/SCSI の .hdf 選択パネルとは反転させる。
                        let panel = NSOpenPanel()
                        panel.allowsMultipleSelection = false
                        panel.canChooseDirectories = true
                        panel.canChooseFiles = false
                        panel.canCreateDirectories = false
                        if panel.runModal() == .OK, let url = panel.url {
                            settingsViewModel.windrvHostPath = url.path
                        }
                    }
                    Button("Clear") {
                        settingsViewModel.windrvHostPath = ""
                    }
                    .disabled(settingsViewModel.windrvHostPath.isEmpty)
                }
                .disabled(!settingsViewModel.windrvEnabled)

                // ★警告文言は常設表示 — 利用者のリスク受容を実効あるものにするため、
                // トグルの状態に関わらず常に見える位置に置く。
                // ★1本の文字列リテラルにすること — `"a" + "b"` は String を作るため
                // Text(LocalizedStringKey) ではなく Text(S: StringProtocol) が選ばれ、
                // ローカライズされなくなる(既存タブは全て単一リテラル)。
                // ★文字列リテラルを変えるとローカライズキーも変わるので、
                // MX68K/Localizable.xcstrings の ja エントリも同時に更新すること。
                // ★P647: 旧文言の末尾にあった「Access is read-only: …」は、書込み
                // トグルが存在する時点で無条件には成り立たないため削除した。
                // 書込みについては下の専用トグル側の文言で述べる。
                Text("⚠ The selected folder and everything inside it — including all subfolders — becomes readable by software running on the emulated X68000. Only pick a folder whose contents you are comfortable exposing to the guest.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                // P647: 書込み許可。★共有トグルとは独立した第 2 の同意で、既定 OFF。
                // 読み取り共有を有効にしただけでは書込みは一切できない。
                // 共有トグルが OFF のときは操作不能にする(単独では意味を持たないため)。
                Toggle(isOn: $settingsViewModel.windrvWriteEnabled) {
                    HStack(spacing: 6) {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundColor(.orange)
                        Text("Allow the guest to modify files (write access)")
                    }
                }
                .disabled(!settingsViewModel.windrvEnabled)

                // ★リスクを具体的に述べる。「書き込みできます」ではなく
                // 「Mac 上のファイルを変更・削除できます」と、失われるものを名指しする。
                Text("⚠ With write access enabled, software running on the emulated X68000 can create, overwrite, rename and delete files and folders inside the shared folder — including files it did not create. A buggy or malicious guest program can destroy data this way, and there is no undo. Leave this off unless you specifically need to save files from the guest to your Mac. Changes are confined to the shared folder; nothing outside it can be reached.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                Text("Takes effect after pressing Apply and then performing a hard reset (⌘R). The guest also needs the WindrvXM.SYS driver loaded from its CONFIG.SYS; the drive letter is assigned by Human68k in CONFIG.SYS device order, not by MX68K.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Section(header: Text("Status")) {
                HStack {
                    Text("Wired")
                    Spacer()
                    Text(isWired ? "Installed" : "Not installed")
                        .foregroundColor(isWired ? .primary : .secondary)
                }
                // P647: 設定値ではなく配線確定値を表示する(P457 の wired 値基準 UI
                // ゲートと同じ方針)——「Apply しただけでは変わらず ⌘R で確定する」
                // ことが表示から分かる。
                HStack {
                    Text("Access")
                    Spacer()
                    Text(isWriteWired ? "Read / Write" : "Read-only")
                        .foregroundColor(isWriteWired ? .orange : .secondary)
                }
                Text("Shows whether the shared folder is actually wired into the running machine. This stays \"Not installed\" until a hard reset (⌘R) is performed, and also if the selected folder no longer exists.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        // P580 — macOS 標準のグループスタイル(Form 自体がスクロール可能)。
        .formStyle(.grouped)
    }
}
