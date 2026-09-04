import SwiftUI

struct HardwareSettingsView: View {
    @EnvironmentObject var settingsViewModel: SettingsViewModel
    #if os(macOS)
    /// P454 — SRAM初期化(工場出荷時設定へ)の実行と確認ダイアログのフラグを持つ。
    ///
    /// ★P706 §C-3: **宣言そのものを `#if os(macOS)` で囲む**(`#else` は空)。
    /// 使用箇所を全部ガードしても、宣言が残っていれば iOS では
    /// `Cannot find type 'EmulatorViewModel' in scope` で型解決の時点で落ちる。
    /// このファイルはガード後に iOS から `emulatorViewModel` を一度も参照しないため、
    /// 代替を宣言すると未使用の `@EnvironmentObject` が増えるだけで、注入漏れ時の
    /// クラッシュ面を無意味に作る。よって `#else` 側は宣言しない。
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    #endif

    #if os(iOS)
    /// P721 — 「Clear SRAM…」の実行と確認ダイアログのフラグを持つ。
    /// macOS 側の `emulatorViewModel`(上の `#if os(macOS)` ブロック)に対応する
    /// iOS 側の受け皿であり、`SASISettingsView.swift:32` /
    /// `SCSISettingsView.swift:41` で既に確立済みの命名・宣言パターンと同一。
    /// 注入は `MX68KiOSApp.swift:251` と `IOSSettingsPresentation.swift:340` の
    /// 2 経路がともに `.environmentObject(viewModel)` で行っている。
    @EnvironmentObject var iosViewModel: MX68KiOSViewModel

    /// P714 — 仮想パッド(オンスクリーン D-pad)の不透明度。
    ///
    /// ★`@AppStorage`(UserDefaults)であって `config.json` ではない: これは
    /// ゲスト機械状態ではなく **ホスト側の純粋な表示設定**であり、ハードリセット同期
    /// (`ConfigManager` 管轄)の対象ではない。`TouchJoystickView` が同じキーを直接
    /// 読むため、スライダーを動かすと即座に見た目へ反映される(P714 計画 §変更内容 6)。
    ///
    /// ★宣言そのものを `#if os(iOS)` で囲む —— このファイルが既に
    /// `emulatorViewModel` で確立している流儀(§C-3、上のコメント参照)に揃える。
    @AppStorage("virtualPadOpacity") private var virtualPadOpacity: Double = 0.6

    /// P716 — トリガーボタン(TRIG1/TRIG2)のサイズ段階。
    /// 0=Small(82pt、P715 の現行サイズ)/ 1=Medium(103pt)/ 2=Large(123pt)。
    /// `virtualPadOpacity` と同じ理由で `@AppStorage`(ホスト側の表示設定であり
    /// `config.json` 管轄外)。`TouchJoystickView` が同じキーを直接読むため、
    /// 選択を変えると即座に見た目へ反映される。
    @AppStorage("triggerButtonSizeLevel") private var triggerButtonSizeLevel: Int = 0

    /// P720 — ソフトキーボード(P710、iOS では画面下部の帯として重なる)の不透明度。
    /// `virtualPadOpacity` と同じ理由で `@AppStorage`(ホスト側の表示設定であり
    /// `config.json` 管轄外)。`MX68KiOSRootView.softKeyboardBand(containerSize:)` が
    /// 同じキーを直接読むため、スライダーを動かすと即座に見た目へ反映される。
    /// 既定 1.0 は現状の見た目(完全不透明)を維持するための意図的な選択。
    @AppStorage("softKeyboardOpacity") private var softKeyboardOpacity: Double = 1.0

    /// P724 — トリガー A / B のオートファイア(連射)有効/無効。
    /// `virtualPadOpacity` と同じ理由で `@AppStorage`(ホスト側の操作設定であり
    /// `config.json` 管轄外)。`TouchJoystickView` が **同じキー**を直接読むため、
    /// トグルを動かすと即座に仮想パッドの挙動へ反映される。
    /// 既定 false は既存挙動(連射なし)をそのまま維持するための意図的な選択。
    @AppStorage("triggerAutoFireA") private var triggerAutoFireA: Bool = false
    @AppStorage("triggerAutoFireB") private var triggerAutoFireB: Bool = false
    #endif

    /// P221b §6-bis(A): armed only after the view is on screen, so a
    /// normalize-on-load write to `machineType` (e.g. old "X68030" -> "SCSI")
    /// cannot misfire the clock default and clobber a saved clock. The clock
    /// default connection is intentionally a user-interaction-only proposal.
    @State private var machineTypeReady = false

    var body: some View {
        Form {
            Section(header: Text("Machine Configuration")) {
                // P221b: machine type is the real-hardware storage-bus axis
                // (SASI: 初代〜EXPERT II / SCSI: SUPER〜XVI). Only the machine
                // -> default-clock proposal is connected; memory is independent.
                Picker("Machine Type", selection: $settingsViewModel.machineType) {
                    Text("SASI Model (Initial – EXPERT II)").tag("SASI")
                    Text("SCSI Model (SUPER – XVI)").tag("SCSI")
                }
                .onChange(of: settingsViewModel.machineType) { newValue in
                    guard machineTypeReady else { return }
                    switch newValue {
                    case "SASI":
                        settingsViewModel.clockMHz = 10   // 機種既定クロック(後から自由に変更可)
                        settingsViewModel.scsiMode = "none"       // P274: 内蔵SCSI不可の機種へ切替時、既定へ戻す
                    case "SCSI":
                        settingsViewModel.clockMHz = 16
                        settingsViewModel.scsiMode = "internal"   // P274: この機種は内蔵SCSI固定
                    default: break
                    }
                }
                .onAppear { machineTypeReady = true }

                Picker("Memory", selection: $settingsViewModel.memoryMB) {
                    Text("1 MB").tag(1)
                    Text("2 MB").tag(2)
                    Text("4 MB").tag(4)
                    Text("6 MB").tag(6)
                    Text("8 MB").tag(8)
                    Text("10 MB").tag(10)
                    Text("12 MB").tag(12)
                }

                Picker("Clock", selection: $settingsViewModel.clockMHz) {
                    Text("10 MHz").tag(10)
                    Text("12 MHz (ACE mod)").tag(12)
                    Text("15 MHz (PRO mod)").tag(15)
                    Text("16 MHz").tag(16)
                    Text("17 MHz (EXPERT mod)").tag(17)
                    Text("20 MHz (Lucky!)").tag(20)
                    Text("24 MHz (RedZone)").tag(24)
                    Text("25 MHz").tag(25)
                }
                Text("Clock speed applies after pressing Apply — no hard reset needed.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                Toggle("FPU Enabled", isOn: $settingsViewModel.fpuEnabled)

                // P686 (D-70): 外付け FDD ユニット(ドライブ 2/3 = C:/D:)。実機で外付けを
                // 接続していなければドライブ 2/3 は EC=1(未接続)を返し、Human68k に
                // C:/D: は現れない(テクニカルデータブック 印刷 p.157 5-5(3))。既定 = 未装着。
                Toggle("External FDD Unit (drives 2 and 3)", isOn: $settingsViewModel.externalFDDUnit)

                Text("Adds two more floppy drives, as an external FDD unit does on real hardware. When it is off, Human68k shows no C:/D: drive — matching a machine with nothing attached.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                #if os(macOS)
                Text("⚠ Takes effect after pressing Apply and then performing a hard reset (⌘R).")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #else
                // ★P706改訂1(2026-08-30): Applyの自動ハードリセットは廃止し、iOSにも
                //   明示的なHard Resetボタンを追加した(macOSの⌘Rと同じ意味論)。
                Text("⚠ Takes effect after pressing Apply and then tapping Hard Reset.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #endif
            }

            // P706 §C-3: 「Speed-up Options」セクションは iOS では丸ごと非表示。
            // ターボ ON/OFF は Emulator メニュー(⌘⇧T)から行う機能で iOS にメニューが
            // 無く、`setTurboTargetMultiplier` / `setFDFastAccess` の即時反映経路も
            // iOS に存在しない。値だけ設定できて効かないトグルは出さない(§0-4 提案(3))。
            #if os(macOS)
            // P555: ターボ(高速実行)。実機に対応物のない、エミュレータ側だけの
            // 機能なので「Machine Configuration」ではなく独立セクションへ置く。
            // ここで選ぶのは「ターボを ON にしたときの倍率」で、ON/OFF の切替は
            // Emulator メニューの「ターボ」(⌘⇧T)から行う。
            // P581: ターボ(P555)と FD アクセス高速化(P557)は、どちらも「実機に無い、
            // エミュレータ側だけの高速化機能」という同じ性質なので 1 セクションへ統合した
            // (ユーザーフィードバック: 解説が別領域に分かれていて読みにくい)。
            Section(header: Text("Speed-up Options")) {
                Picker("Turbo Target", selection: $settingsViewModel.turboTargetMultiplier) {
                    Text("2x").tag(2)
                    Text("3x").tag(3)
                    Text("4x").tag(4)
                    Text("5x").tag(5)
                    // P556: 倍率上限なし。tag は kTurboNoWaitMultiplier(= -1)。
                    Text("No Wait").tag(kTurboNoWaitMultiplier)
                }
                .onChange(of: settingsViewModel.turboTargetMultiplier) { newValue in
                    // クロック速度と同じく即時反映(ターボが既に ON の場合のみ意味を持つ)。
                    emulatorViewModel.setTurboTargetMultiplier(newValue)
                }

                Text("Runs the emulator faster than real time. Toggle it from the Emulator menu (⌘⇧T). Sound is muted while turbo is on.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                // P556: ノーウェイトは他の選択肢と性質が違う(上限なし・CPU 占有・
                // 表示のティアリングが起こり得る)ため、その注記を 1 行だけ添える。
                Text("“No Wait” runs as fast as the host CPU allows, with no multiplier cap. It keeps one CPU core fully busy, and the display may tear.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                // P557: FD アクセス高速化。実機の FDD 機構そのものではなく「エミュレータ側の
                // 待ち時間を詰める」機能(XM6 の「フロッピーディスク高速化」設定に相当)。
                // P581: ターボと同じ「高速化オプション」セクションへ統合。
                Divider()

                Toggle("Fast FD Access", isOn: $settingsViewModel.fdFastAccess)
                    .onChange(of: settingsViewModel.fdFastAccess) { newValue in
                        // クロック速度・ターボ倍率と同じく即時反映(リセット不要)。
                        emulatorViewModel.setFDFastAccess(newValue)
                    }

                // P497: 文字列連結(`+`)は非ローカライズオーバーロードへ解決される
                // ため、単一リテラルで書く。
                Text("Shortens the emulated seek/read/write waits to a fixed 64 µs, matching XM6's floppy disk speed-up option. Applies immediately.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                // 既定 OFF を選んだ理由(既知のトレードオフ)を明示する。
                Text("⚠ Leave this off for titles whose copy protection depends on real floppy rotation timing — they may behave differently when it is on.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            #endif   // P706 §C-3: Speed-up Options(macOS のみ)ここまで

            // P454: バッテリバックアップ SRAM の初期化(工場出荷時設定へ)(Docs/01 2-7)。
            // P505 (D-46): 全ゼロ化から IPL-ROM 内蔵既定値へのシードへ意味論を変更。
            // P493: 容量選択(標準 16KB / 改造 64KB)を同じセクションに置く。
            //
            // P706 §C-3: 当初 iOS では「Clear SRAM…」ボタンと確認アラートを出して
            // いなかった(`emulatorViewModel.clearSRAM()` の iOS 対応物が無かったため)。
            // ★P721 でその対応物(`MX68KiOSViewModel.clearSRAM()` /
            // `showSRAMClearConfirm`)を追加し、iOS 側にも同じ導線を置いた。
            // アラートは Section へ付くモディファイアなので分岐は Section ごと行う
            // 形のまま(macOS 側は従来とバイト同一に保つことを最優先、§C 大原則)。
            #if os(macOS)
            Section(header: Text("SRAM")) {
                Picker("Capacity", selection: $settingsViewModel.sram64kEnabled) {
                    Text("16 KB (standard)").tag(false)
                    Text("64 KB (mod)").tag(true)
                }
                // P497: 文字列連結(`+`)は Text(String) 側の非ローカライズ
                // オーバーロードに解決されるため、単一リテラルへ統合(表示文言は不変)。
                // P581: 注意文の先頭記号を `⚠` に統一。★リテラルを変えるとローカライズ
                // キーも変わるため、MX68K/Localizable.xcstrings へ新リテラルの ja 訳を
                // 必ず追加すること(追加漏れは ja 表示が英語へ後退する = 孤児化)。
                Text("⚠ Expands internal battery-backed SRAM from the standard 16 KB to 64 KB, matching the real-hardware modification. The upper 48 KB is stored separately in sram_ext.dat. Takes effect after pressing Apply and then performing a hard reset (⌘R).")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                Button("Clear SRAM…", role: .destructive) {
                    emulatorViewModel.showSRAMClearConfirm = true
                }
                // P497: 同上(単一リテラル化。表示文言は不変)。
                Text("Resets all battery-backed SRAM settings (machine type flags, memory switches, boot device) to factory defaults (ROM built-in defaults) — equivalent to removing the backup battery on real hardware. Takes effect after a reset.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            // P451 で確立した新 .alert(_:isPresented:actions:message:) API パターンに倣い、
            // Enter=実行 / Esc=キャンセルを明示的に割り当てる。
            .alert("Clear SRAM", isPresented: $emulatorViewModel.showSRAMClearConfirm) {
                Button("Cancel", role: .cancel) { }
                    .keyboardShortcut(.cancelAction)
                Button("Clear", role: .destructive) {
                    emulatorViewModel.clearSRAM()
                }
                .keyboardShortcut(.defaultAction)
            } message: {
                Text("This will erase all SRAM settings. This cannot be undone. Continue?")
            }
            #else
            Section(header: Text("SRAM")) {
                Picker("Capacity", selection: $settingsViewModel.sram64kEnabled) {
                    Text("16 KB (standard)").tag(false)
                    Text("64 KB (mod)").tag(true)
                }
                // ★P706改訂1(2026-08-30): 文言をApply後の明示的Hard Reset必須へ更新(BIOSSettingsView.swift参照)。
                Text("⚠ Expands internal battery-backed SRAM from the standard 16 KB to 64 KB, matching the real-hardware modification. The upper 48 KB is stored separately in sram_ext.dat. Takes effect after pressing Apply and then tapping Hard Reset.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                // P721 — macOS 版(上の `#if os(macOS)` ブロック)と同内容。表示文言は
                // 逐語再利用し、新しい文言を発明しない(ローカライズ済みの既存キーを
                // そのまま使う)。参照先だけ `emulatorViewModel` → `iosViewModel`。
                Button("Clear SRAM…", role: .destructive) {
                    iosViewModel.showSRAMClearConfirm = true
                }
                Text("Resets all battery-backed SRAM settings (machine type flags, memory switches, boot device) to factory defaults (ROM built-in defaults) — equivalent to removing the backup battery on real hardware. Takes effect after a reset.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            // P721 — 確認アラート。文言・ボタン・ロールは macOS 版を逐語再利用する。
            // ★`.keyboardShortcut` は付けない —— `MX68KiOSApp.swift:256-259` の Hard Reset
            // 確認アラート(P706)が同じ問いに対し既に「タッチ操作専用 UI でありスコープ外」
            // として付けない判断を確立済みであり、新規アラートもその慣例へ揃える。
            .alert("Clear SRAM", isPresented: $iosViewModel.showSRAMClearConfirm) {
                Button("Cancel", role: .cancel) { }
                Button("Clear", role: .destructive) {
                    iosViewModel.clearSRAM()
                }
            } message: {
                Text("This will erase all SRAM settings. This cannot be undone. Continue?")
            }

            // P714 — タッチ操作(iOS のみ)。macOS には仮想パッドが存在しないため
            // Section ごと iOS 側にだけ置く(SRAM と同じ Section 単位の分岐)。
            Section(header: Text("Touch Controls")) {
                // ★範囲の両端は意図的に 0/1 ではない: 0 だと存在に気づけず操作不能に
                //   なり、1.0 だとゲーム画面を過度に覆う(P714 計画 §変更内容 6)。
                //   既定 0.6 は既存の帯(`.black.opacity(0.6)`)と同じ値を踏襲。
                // ★iOS の `Slider` は `label` を**描画しない**(アクセシビリティ用途
                //   のみ)。見出しは独立した `Text` として出し、スライダー側へは
                //   `.accessibilityLabel` で同じ文言を与える。
                VStack(alignment: .leading, spacing: 4) {
                    Text("Virtual Pad Opacity")
                    Slider(value: $virtualPadOpacity, in: 0.15...0.9)
                        .accessibilityLabel("Virtual Pad Opacity")
                }
                Text("Sets how visible the on-screen joystick overlay is. Applies immediately.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                // P716 — トリガーボタンのサイズ段階。倍率は 1.0 / 1.25 / 1.5(基準 82pt)。
                Picker("Trigger Button Size", selection: $triggerButtonSizeLevel) {
                    Text("Small").tag(0)
                    Text("Medium").tag(1)
                    Text("Large").tag(2)
                }
                .pickerStyle(.segmented)
                Text("Adjusts the size of the TRIG1/TRIG2 buttons. Applies immediately.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                // P724 — A / B トリガーのオートファイア(連射)。既定は両方 OFF
                // (既存挙動を維持する opt-in)。ボタンごとに独立させているのは
                // 「片方は単発・もう片方は連射」という実機の連射コントローラと
                // 同じ使い分けを可能にするため。連射周期は内部固定値
                // (`TouchJoystickView.swift` の `autoFirePhaseInterval`)であり、
                // 可変設定は本サイクルのスコープ外。
                Toggle("Auto-fire A", isOn: $triggerAutoFireA)
                Toggle("Auto-fire B", isOn: $triggerAutoFireB)
                Text("Rapid-fires the A/B buttons while held. Applies immediately.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                // P720 — ソフトキーボードの不透明度。既定 1.0(現状の見た目を維持)。
                // ★下限は仮想パッドの 0.15 ではなく 0.3 —— キーボードは文字情報が多く、
                //   透明化しすぎるとキー文字の可読性が直接失われるため、読める手前で止める。
                // ★上限 1.0(= 既定)は完全不透明。仮想パッドの上限 0.9 と異なるのは、
                //   こちらは「現状維持を既定にして必要なら下げる」設計だからである。
                VStack(alignment: .leading, spacing: 4) {
                    Text("Soft Keyboard Opacity")
                    Slider(value: $softKeyboardOpacity, in: 0.3...1.0)
                        .accessibilityLabel("Soft Keyboard Opacity")
                }
                Text("Sets how visible the on-screen keyboard is. Applies immediately.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            #endif
        }
        // P580 — macOS 標準のグループスタイル(Form 自体がスクロール可能)。
        .formStyle(.grouped)
    }
}
