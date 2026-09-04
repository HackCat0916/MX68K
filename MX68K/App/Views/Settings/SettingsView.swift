import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var settingsViewModel: SettingsViewModel
    @EnvironmentObject var configManager: ConfigManager
    #if os(macOS)
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    #else
    /// P706 §C-2 — iOS 側は Apply で **実際に** ViewModel を呼ぶ必要があるため、
    /// macOS と対称な形で `@EnvironmentObject` を宣言する(クロージャ注入のような
    /// 別方式を持ち込まない)。★`HardwareSettingsView` は事情が違い、ガード後に
    /// iOS から一度も参照しないので `#else` 側の宣言を **置かない**。
    @EnvironmentObject var iosViewModel: MX68KiOSViewModel
    #endif
    /// P274: 既定タブは「一般」(ユーザー要望)。P682 で初期値は `init` へ移した。
    @State private var selectedTab: Int
    /// P579 — 専用ウィンドウ(`Window(id: "settings")`)を閉じるため。
    /// BIOS 未設定フォールバック(`RootView` 埋め込み)では `showCloseButton == false`
    /// なので呼ばれない。
    @Environment(\.dismiss) private var dismiss
    /// P229 — 独立提示の文脈では「Close」ボタンを出す(既定)。BIOS 未設定
    /// フォールバック(メイン画面としての表示)では戻り先がないため false。
    var showCloseButton: Bool

    /// P682 — BIOS未設定時の自動表示(`showCloseButton == false`、`RootView`経由)では
    /// 既定タブを「BIOS」(tag 0)にする。通常の設定ウィンドウ(⌘,、`showCloseButton == true`)
    /// は既定「一般」(tag -1、P274の決定)のまま変更しない。
    init(showCloseButton: Bool = true) {
        self.showCloseButton = showCloseButton
        #if os(macOS)
        _selectedTab = State(initialValue: showCloseButton ? -1 : 0)
        #else
        // P706 §C-2 / S-9: タグ -1(一般)は iOS で移植対象外なので、提示経路に
        // よらず常に BIOS(0)から開く。
        _selectedTab = State(initialValue: 0)
        #endif
    }

    // P706 §C-2 — `.frame(minWidth: 680…)` は iPhone 幅(390pt)を超えるため
    // macOS でだけ適用する。SwiftUI のモディファイア連鎖の途中へ `#if` を挟む
    // 書き方はこのリポジトリに前例が無いので、本体を `settingsBody` へ括り出して
    // `body` 側で分岐する(macOS 側の連鎖内容は従来とバイト同一のまま)。
    var body: some View {
        #if os(macOS)
        settingsBody
            // P578: 560pt では Form のラベル列を差し引くと説明文に使える幅が 400pt 前後
            // しかなく、各タブの説明文(`.font(.caption)`)が読めなかった。BIOS 未設定時の
            // フォールバック表示(MX68KApp.swift RootView の 640x480)より広い 680x480 へ
            // 統一し、同じ画面が経路によって幅が違う不統一も解消する。
            // P579: 専用ウィンドウ化で親ウィンドウ幅の頭打ちは消えたが、上限が無いままだと
            // 入力タブの長文説明が ideal 幅を 1000pt 超へ押し上げ、初回表示で不自然に巨大な
            // ウィンドウが開いてしまう。ideal/max でクランプする(ユーザーの手動リサイズは
            // `Window` シーンの既定どおり可能)。
            .frame(minWidth: 680, idealWidth: 700, maxWidth: 720,
                   minHeight: 480, idealHeight: 600, maxHeight: 720)
        #else
        settingsBody
        #endif
    }

    private var settingsBody: some View {
        VStack {
            Picker("Tab", selection: $selectedTab) {
                #if os(macOS)
                Text("General").tag(-1)
                Text("BIOS").tag(0)
                Text("Hardware").tag(1)
                Text("Audio").tag(2)
                Text("Input").tag(3)   // P194
                Text("SASI").tag(4)    // P273
                Text("SCSI").tag(5)    // P273
                Text("Windrv").tag(6)  // P642
                #else
                // P706 §0-4 提案(3): 未移植のタブは **表示しない**。Browse… が死んだ
                // タブを見せるより、動く数枚だけを出す方が完成品である。
                // P712: SASI / SCSI を移植したので追加(タグ番号は macOS と共通)。
                Text("BIOS").tag(0)
                Text("Hardware").tag(1)
                Text("SASI").tag(4)
                Text("SCSI").tag(5)
                #endif
            }
            .pickerStyle(.segmented)
            // P583: セグメント帯の左に「Tab」というラベルが描画されていたため隠す。
            // アクセシビリティラベルとしては "Tab" 文字列が残るため、xcstrings には
            // `Tab` キーを ja 訳付きで登録してある(孤児化回避)。
            .labelsHidden()
            .padding()

            // P580 — P579 で追加した外側 `ScrollView` を撤去。`Form` を `ScrollView` へ
            // 入れ子にすると、`ScrollView` の水平クリップ境界と `Form` 内 `Text` の
            // 折返し計算幅が食い違い、日本語(任意文字境界で折り返す)では行末の文字が
            // まるごと欠落した。スクロールは各タブの `Form` に付けた
            // `.formStyle(.grouped)`(macOS 標準、Form 自体がスクロール可能)が担う。
            Group {
                #if os(macOS)
                switch selectedTab {
                case -1:
                    GeneralSettingsView()
                case 0:
                    BIOSSettingsView()
                case 1:
                    HardwareSettingsView()
                case 2:
                    AudioSettingsView()
                case 3:
                    InputSettingsView()   // P194
                case 4:
                    SASISettingsView()   // P273
                case 5:
                    SCSISettingsView()   // P273
                case 6:
                    WindrvSettingsView()   // P642
                default:
                    EmptyView()
                }
                #else
                // P706 §C-2: iOS へ移植済みのタブのみ。`#if` は switch の case を
                // 個別に囲えないため、switch ごと分岐する。
                // P712: SASI / SCSI を追加(View 自体が内部で `#if os(iOS)` 分岐する)。
                switch selectedTab {
                case 0:
                    BIOSSettingsView()
                case 1:
                    HardwareSettingsView()
                case 4:
                    SASISettingsView()   // P712
                case 5:
                    SCSISettingsView()   // P712
                default:
                    EmptyView()
                }
                #endif
            }
            .padding()

            // P580 — ここにあった `Spacer()` は撤去。`.formStyle(.grouped)` の `Form` は
            // 利用可能な高さいっぱいに伸びてスクロールするため、`Spacer()` を残すと
            // 垂直方向の伸縮を奪い合い、Form のスクロール領域が半分に削られてしまう。
            HStack {
                Spacer()
                // P229 — シート文脈でのみ「Close」ボタンを表示(Esc で閉じる)。
                // 保存せずに閉じる = Apply 未押下時に変更が反映されない既存挙動と同じ。
                if showCloseButton {
                    Button("Close") { dismiss() }
                        .keyboardShortcut(.cancelAction)
                }
                Button("Apply") {
                    configManager.applySettings(from: settingsViewModel)
                    #if os(macOS)
                    emulatorViewModel.applySettings(configManager.config)
                    #else
                    // P706 §D-3: iOS には ⌘R が無いため、applySettings 側が
                    // 自動でハードリセットする(macOS との意図的な差異)。
                    // BIOS 未設定で起動を保留していた場合はここで起動を再試行する。
                    iosViewModel.applySettings(configManager.config)
                    #endif
                    // P452: 独立提示時は Apply で反映と同時に閉じる(ユーザー要望)。
                    // フォールバック表示(showCloseButton==false、BIOS未設定時のメイン画面)には
                    // 「閉じる」という概念自体が無いため対象外。
                    if showCloseButton {
                        dismiss()
                    }
                }
                .keyboardShortcut(.defaultAction)
            }
            .padding()
        }
        .onAppear {
            settingsViewModel.load(from: configManager.config)
            #if os(macOS)
            emulatorViewModel.requestAutoPause()   // P229: 設定表示中は自動一時停止。
            #endif
            // P706 §C-2: iOS では自動一時停止しない。macOS の自動一時停止は
            // `NSOpenPanel.runModal()` 中も CVDisplayLink が回り続けることへの対処で
            // あり、iOS の `.fileImporter` はモーダルループを持たない別機構。加えて
            // iOS には一時停止/再開の UI が無く、片方だけ呼ばれて止まったままになる
            // 事故の面を作らない。
        }
        .onDisappear {
            #if os(macOS)
            emulatorViewModel.releaseAutoPause()    // P229: 閉じたら再開(手動停止中なら維持)。
            #endif
        }
    }
}
