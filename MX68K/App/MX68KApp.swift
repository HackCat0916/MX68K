import SwiftUI

/// P563(D-49 土台修正)— アプリ全体で 1 個だけ存在するルートオブジェクトの保管庫。
///
/// これまで `MX68KApp` は 3 つの `ObservableObject` を `@StateObject` で保持していたが、
/// `@StateObject` は「そのプロパティを観測する」ことを意味するため、`emulatorViewModel` の
/// 毎秒/毎フレームの `@Published` 更新で `App.body` 全体(= `.commands{}` を含む)が
/// 再評価され、開いていた Monitor サブメニューが閉じてしまっていた(D-49)。
///
/// Swift の `static let` は型の生存期間中ちょうど 1 回だけ遅延初期化されることが
/// **言語仕様として保証される**ため、`MX68KApp.init()` が SwiftUI の内部都合で何度
/// 呼ばれても参照先の実体は不変。`@StateObject` に頼らず単一インスタンスを保証できる。
/// 既存の `EmulatorRunState.shared`(P546)・`MouseCaptureState.shared`(P237)と同型。
enum AppRootObjects {
    static let configManager = ConfigManager()
    static let emulatorViewModel = EmulatorViewModel()
    static let settingsViewModel = SettingsViewModel()
}

/// P563 — BIOS 設定済み/未設定でルート画面を切り替える薄いラッパー。
///
/// この判定(`configManager.config` の読み取り)は従来 `App.body` 内で直接行っていたが、
/// それだと `App` が `configManager` を観測せざるを得ず、上記の再評価問題を招く。
/// 判定をこの View へ移し、`@EnvironmentObject` で観測することで、`configManager` の
/// 変化に反応するのはこの View 以下だけになる(`.commands{}` は再構築されない)。
struct RootView: View {
    @EnvironmentObject private var configManager: ConfigManager

    /// P578 — Tools メニュー →「イメージ作成」ダイアログの提示状態。
    /// ★提示先を `EmulatorView` ではなく `RootView` にするのは意図的:
    /// BIOS 未設定時は `EmulatorView` がビュー階層に居らず、そこに付けると
    /// 「BIOS 未設定でも空イメージは作れる」という P574 以来の性質が壊れる。
    /// 観測対象は専用の極低頻度オブジェクトのみ(EmulatorViewModel 全体を
    /// ここで観測すると毎フレーム更新でこの分岐ごと再評価されてしまう)。
    @ObservedObject private var blankImage = BlankImagePresentation.shared

    var body: some View {
        Group {
            if configManager.config.bios.iplromPath.isEmpty || configManager.config.bios.cgromPath.isEmpty {
                SettingsView(showCloseButton: false)
                    // P580 — `SettingsView` 内部の `minWidth: 680` と値を統一
                    // (P578 以降 2 箇所で異なる下限値を保持していた重複の解消)。
                    .frame(minWidth: 680, minHeight: 480)
            } else {
                EmulatorView()
                    // P190 — 800x600 だとウィンドウ最小サイズが 1x(768x512+chrome)を
                    // 上回ってしまい ⌘1 が効かないため 640x480 に緩和。
                    .frame(minWidth: 640, minHeight: 480)
            }
        }
        // P578 — Tools メニュー経路専用の `.sheet`。設定タブ内のボタン経路とは
        // 状態を共有しない(SASISettingsView / SCSISettingsView が各自ローカルな
        // `@State` で同じダイアログ View を提示する)。同一ビューへ `.sheet` を
        // 2 つ付けると片方しか機能しない制約(EmulatorView.swift:123-124)と
        // 同種の問題を避けるための分離。
        .sheet(item: $blankImage.kind) { kind in
            BlankImageDialog(kind: kind)
        }
    }
}

@main
struct MX68KApp: App {
    // P53 — install AppDelegate so applicationWillTerminate / SIGTERM
    // signal source actually fire. See /tmp/mx68k_P53_plan.md §7 Edit D.
    @NSApplicationDelegateAdaptor(MX68KAppDelegate.self) var appDelegate

    // P563 — @StateObject(=観測)をやめ、AppRootObjects の静的ストレージを読むだけの
    // プレーンな let にする。実体の生存は AppRootObjects 側の static let が保証する。
    private let configManager = AppRootObjects.configManager
    private let emulatorViewModel = AppRootObjects.emulatorViewModel
    private let settingsViewModel = AppRootObjects.settingsViewModel

    init() {
        // P554 — 前回セッションのクラッシュ/強制終了で残った zip 展開用の一時
        // ディレクトリを起動時に 1 回だけ掃除する(ベストエフォート)。
        ArchiveMountService.sweepStaleTempDirs()
    }

    var body: some Scene {
        WindowGroup {
            // P563 — 分岐は RootView 側へ移動。environmentObject の付与はここ 1 箇所に集約する。
            RootView()
                .environmentObject(emulatorViewModel)
                .environmentObject(configManager)
                .environmentObject(settingsViewModel)
        }
        .commands {
            EmulatorCommands(emulatorViewModel: emulatorViewModel)
            FileCommands(emulatorViewModel: emulatorViewModel)   // ★P673 追加
            ToolsCommands()
            DisplayCommands()
            MonitorCommands()
            SoftKeyboardCommands(emulatorViewModel: emulatorViewModel)
        }

        // P579 — 設定画面を `.sheet`(モーダル)から専用ウィンドウへ移行。
        // `.sheet` は macOS の仕様上、親ウィンドウ幅(実測 768pt)を超えて広げられず、
        // 入力/サウンドタブの長い説明文と下部ボタンが切れていた(P578 hands-on 報告)。
        // 独立シーンにすればこの上限が構造的に消え、ユーザーが自由にリサイズできる。
        // environmentObject の内訳は SettingsView が参照する 3 つすべて。
        Window("Settings", id: "settings") {
            SettingsView()
                .environmentObject(emulatorViewModel)
                .environmentObject(configManager)
                .environmentObject(settingsViewModel)
        }

        Window("CPU / Registers", id: "monitor-cpu") {
            CPUMonitorView()
                .environmentObject(emulatorViewModel)
        }

        // P600 — memory dump viewer / memory map viewer. Both read through the
        // side-effect-free mx68k_read_memory_bytes / mx68k_get_region_map pair,
        // so they need no environment objects.
        Window("Memory", id: "monitor-memory") {
            MemoryDumpMonitorView()
        }
        Window("Memory Map", id: "monitor-memory-map") {
            MemoryMapMonitorView()
        }

        // P286 — developer monitor panels (CRTC / Video Controller / BG・Sprite).
        Window("CRTC", id: "monitor-crtc") {
            CRTCMonitorView()
                .environmentObject(emulatorViewModel)
        }
        Window("Video Controller", id: "monitor-vc") {
            VideoControllerMonitorView()
                .environmentObject(emulatorViewModel)
        }
        Window("BG", id: "monitor-bg") {
            BGMonitorView()
                .environmentObject(emulatorViewModel)
        }
        // P550 — palette monitor (TextPal32 / GrphPal32 256 colors + contrast).
        Window("Palette", id: "monitor-palette") {
            PaletteMonitorView()
                .environmentObject(emulatorViewModel)
        }
        Window("Sprite", id: "monitor-sprite") {
            SpriteMonitorView()
                .environmentObject(emulatorViewModel)
        }
        // P328 — full 128-slot sprite table map (including unused slots).
        Window("Sprite Table Map", id: "monitor-sprite-table") {
            SpriteTableMapView()
                .environmentObject(emulatorViewModel)
        }
        // P328 — renderer panel (composited framebuffer preview, moved from Sprite).
        Window("Renderer", id: "monitor-renderer") {
            RendererMonitorView()
                .environmentObject(emulatorViewModel)
        }
        // P343 — text plane preview (raw TextDrawWork contents).
        Window("Text Plane", id: "monitor-text") {
            TextPlaneMonitorView()
                .environmentObject(emulatorViewModel)
        }
        // P348 — BG page preview (raw BG0/BG1 tilemap+pattern contents).
        Window("BG Page", id: "monitor-bg-page") {
            BGPageMonitorView()
                .environmentObject(emulatorViewModel)
        }
        // P348 — graphics page preview (raw GVRAM contents, 256-color mode).
        Window("Graphics Page", id: "monitor-grp-page") {
            GrpPageMonitorView()
                .environmentObject(emulatorViewModel)
        }
        // P365 — BG+Sprite composite buffer preview (pre-final-priority-merge state).
        Window("BG+Sprite Composite", id: "monitor-bgsp-composite") {
            BGSPCompositeMonitorView()
                .environmentObject(emulatorViewModel)
        }
        // P484 — sound monitor (ADPCM section; OPM section follows in P485).
        Window("Sound", id: "monitor-sound") {
            SoundMonitorView()
                .environmentObject(emulatorViewModel)
        }
        // P631 — OPM synthesizer panel (8 stacked per-channel keyboards + KCF/V/PAN).
        // Independent of the sound monitor window: it has its own visibility gate
        // (EmulatorEngine.opmSynthVisible), so closing one does not stop the other.
        Window("OPM Synthesizer", id: "monitor-opm-synth") {
            OPMSynthesizerView()
                .environmentObject(emulatorViewModel)
        }
        // P551 — input monitor (gamepad port assignment / raw joystick byte / mouse state).
        // The gamepad/mouse sections read InputManager.shared directly, but P695 added a
        // keyboard lock-LED section whose value comes from the bridge through the usual
        // per-frame monitor path, so emulatorViewModel is now required.
        Window("Input", id: "monitor-input") {
            InputMonitorView()
                .environmentObject(emulatorViewModel)
        }
        // P692 — storage monitor (SASI 8 units + SCSI ID0-6 incl. MO/CD mount state).
        // Needs configManager as well: the panel deliberately shows the configured
        // path next to the live one so the two can be compared (⌘R pending badge).
        Window("Storage", id: "monitor-storage") {
            StorageMonitorView()
                .environmentObject(emulatorViewModel)
                .environmentObject(configManager)
        }
        // P693 — MIDI monitor (board wiring / TX-RX counters / YM3802 registers).
        // Needs configManager as well: the panel shows the current MIDI settings
        // (reset command, send-on-init, output delay) and the selected device names.
        Window("MIDI", id: "monitor-midi") {
            MIDIMonitorView()
                .environmentObject(emulatorViewModel)
                .environmentObject(configManager)
        }
        // P694 — RTC monitor (RP5C15 control/alarm registers + host-clock snapshot).
        // Reads only emulatorViewModel.rtcMonitorStatus, so configManager is not needed.
        Window("RTC", id: "monitor-rtc") {
            RTCMonitorView()
                .environmentObject(emulatorViewModel)
        }
        // P287 — performance monitor (measured fps / per-frame processing time).
        Window("Performance", id: "monitor-perf") {
            PerformanceMonitorView()
                .environmentObject(emulatorViewModel)
        }

        // P228 — on-screen software keyboard (full JIS replica). Talks to the
        // guest directly through the C bridge, so it needs no environment objects.
        Window("Software Keyboard", id: "soft-keyboard") {
            SoftKeyboardView()
        }
    }
}

/// P546(D-10根本修正)— 「Emulator」メニューと「設定…」を独立 Commands 構造体へ分離。
/// emulatorViewModel はアクション呼び出し用にプレーンな let で保持し(観測しない)、
/// .disabled 判定だけを専用の EmulatorRunState.shared 経由にする。EmulatorViewModel
/// 全体を観測すると status 等の毎秒更新でメニューバー全体が再構築され、初回オープン時の
/// ちらつき(D-10)を招くため — SoftKeyboardCommands / MouseCaptureState(P237)と同じ手法。
struct EmulatorCommands: Commands {
    let emulatorViewModel: EmulatorViewModel
    @ObservedObject private var runState = EmulatorRunState.shared
    /// P579 — 設定画面の専用ウィンドウ(id: "settings")を開くため。
    /// MonitorCommands / SoftKeyboardCommands と同じパターン。
    @Environment(\.openWindow) private var openWindow

    /// P697 — 録画メニュー項目のラベル。★戻り値の型を LocalizedStringKey と
    /// 明示するのが要点: `Button(cond ? "A" : "B")` と直接書くと三項演算子の
    /// リテラルが String に推論され、`Button(_ title: S)`(非ローカライズ)の
    /// オーバーロードが選ばれて日本語UIでも英語のまま出る(P497 の教訓)。
    private var recordingMenuLabel: LocalizedStringKey {
        runState.isRecording ? "Stop Recording" : "Start Recording"
    }

    var body: some Commands {
        // P191 — Docs/03 準拠に是正: ⌘R=ハード(確認あり)・⌘⇧R=ソフト・⌘⇧N=NMI・⌘S=スクショ。
        // 全項目を isRunning でゲート: BIOS 未設定時は EmulatorView(=alert を持つ
        // ToolbarView)がビュー階層に居ないため、フラグ立てや未初期化コア呼び出しを防ぐ。
        CommandMenu("Emulator") {
            Button("Reset (Hard)") {
                emulatorViewModel.showHardResetConfirm = true   // 確認は ToolbarView の alert
            }
            .keyboardShortcut("r", modifiers: .command)
            .disabled(!runState.isRunning || runState.powerState == .poweringOff)   // P204/S3: フェード中は reset 系を無効化(scheduled reset と shutdown の競合回避)

            Button("Reset (Soft)") {
                emulatorViewModel.softReset()
            }
            .keyboardShortcut("r", modifiers: [.command, .shift])
            .disabled(!runState.isRunning || runState.powerState == .poweringOff)   // P204/S3: フェード中は reset 系を無効化(scheduled reset と shutdown の競合回避)

            Button("Interrupt") {
                emulatorViewModel.nmi()
            }
            .keyboardShortcut("n", modifiers: [.command, .shift])
            .disabled(!runState.isRunning || runState.powerState == .poweringOff)   // P204/S3: フェード中は reset 系を無効化(scheduled reset と shutdown の競合回避)

            Divider()

            Button("Pause") {
                emulatorViewModel.togglePause()
            }
            .keyboardShortcut("p", modifiers: .command)
            .disabled(!runState.isRunning || runState.powerState == .poweringOff)   // P204/S3: フェード中は reset 系を無効化(scheduled reset と shutdown の競合回避)

            // P555 — ターボ(等倍 ⇔ 設定画面で選んだ目標倍率)のワンタッチ切替。
            // ⌘⇧T は既存ショートカット(⌘⌥T = テキスト面モニタ)と衝突しない。
            Button("Turbo") {
                emulatorViewModel.toggleTurbo()
            }
            .keyboardShortcut("t", modifiers: [.command, .shift])
            // P697: 録画中はターボ/ノーウェイトを禁止(壁時計基準の PTS が
            // 実行速度変動で崩れるため)。ショートカット経由の発火は
            // EmulatorViewModel.toggleTurbo() 側のガードが受け止める二重防御。
            .disabled(!runState.isRunning || runState.powerState == .poweringOff || runState.isRecording)

            Divider()

            Button("Screenshot") {
                emulatorViewModel.takeScreenshot()
            }
            .keyboardShortcut("s", modifiers: .command)
            .disabled(!runState.isRunning || runState.powerState == .poweringOff)   // P204/S3: フェード中は reset 系を無効化(scheduled reset と shutdown の競合回避)

            // P697 — 動画録画(映像のみ)。ラベルは録画状態でトグルする。
            // ★観測対象は EmulatorRunState.shared のみ(EmulatorViewModel を観測すると
            // status 等の毎秒更新で Commands 全体が再評価される = D-10/D-49)。
            Button(recordingMenuLabel) {
                emulatorViewModel.toggleRecording()
            }
            .disabled(!runState.isRunning || runState.powerState == .poweringOff)

            Divider()

            // P198 — ステートセーブ/ロード(Phase 2 #4)。⌘⌥1 は monitor なので衝突なし。
            Button("Save State…") {
                emulatorViewModel.saveState()
            }
            .keyboardShortcut("s", modifiers: [.command, .option])
            .disabled(!runState.isRunning || runState.powerState == .poweringOff)   // P204/S3: フェード中は reset 系を無効化(scheduled reset と shutdown の競合回避)

            Button("Load State…") {
                emulatorViewModel.loadState()
            }
            .keyboardShortcut("o", modifiers: [.command, .option])
            .disabled(!runState.isRunning || runState.powerState == .poweringOff)   // P204/S3: フェード中は reset 系を無効化(scheduled reset と shutdown の競合回避)

            // P578 — 旧「新規FDイメージを作成…」は Tools メニュー(ToolsCommands)の
            // 「イメージ作成」へ移設した(FD/SASI/SCSI の 3 導線を 1 箇所へ集約)。
        }
        // P229 — 標準の「設定…」メニュー項目を独自ハンドラに置き換える。
        // P579 — 提示方式をシートから専用ウィンドウ(id: "settings")へ戻した
        // (シートでは親ウィンドウ幅を超えられずタブ内容が切れるため)。
        CommandGroup(replacing: .appSettings) {
            Button("Settings…") { openWindow(id: "settings") }
                .keyboardShortcut(",")
                .disabled(!runState.isRunning)
        }
        // P701 — 標準の「MX68Kについて」を独自ハンドラに置き換え、クレジット欄
        // (px68k クレジット・移植元表記・ライセンス・GitHub リンク)を追加する。
        // ★.disabled() は付けない: コアを一切呼ばないホスト側 UI のため
        //   (ToolsCommands と同じ理由)、BIOS 未設定/停止中でも開ける。
        CommandGroup(replacing: .appInfo) {
            Button("About MX68K") { AboutPanel.show() }
        }
    }
}

/// P673 — File メニュー: FD0/FD1/MO ドライブの挿入・書込禁止挿入・イジェクトを
/// メニューからも操作可能にする(従来はツールバーの導線のみ)。
/// P676 — CD-ROM ドライブ(ID6)を追加。CD は ROM 媒体なので「書込禁止挿入」は無い。
///
/// ★D-49/P563 規律を厳守(ToolsCommands のコメント参照): emulatorViewModel は
/// 非観測の let で保持し、`.disabled()` は EmulatorRunState.shared のみを
/// 観測して行う。個別ドライブのパス空判定はここでは行わない
/// (`fdd0Path` 等を観測すると `.commands{}` 全体が毎秒/毎フレーム再評価され、
/// 開いていたサブメニューが閉じる D-49 と同種の再発を招くため)。
/// Eject はメディア未挿入時に呼んでも Core 側で安全な no-op。
struct FileCommands: Commands {
    let emulatorViewModel: EmulatorViewModel
    @ObservedObject private var runState = EmulatorRunState.shared

    private var isDisabled: Bool {
        !runState.isRunning || runState.powerState == .poweringOff   // P204/S3 と同型
    }

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Menu("FD Drive (FD0)") {
                fddMenuItems(drive: 0)
            }
            Menu("FD Drive (FD1)") {
                fddMenuItems(drive: 1)
            }
            // P684: FD2/FD3。Core fdd.c は元から 4 台対応で、ゲスト側 IOCS も
            // A/B/C/D の 4 ドライブを列挙している。ツールバー/ステータスバー/D&D は
            // 対象外(ユーザーとの合意)なので、導線はこの File メニューのみ。
            Menu("FD Drive (FD2)") {
                fddMenuItems(drive: 2)
            }
            Menu("FD Drive (FD3)") {
                fddMenuItems(drive: 3)
            }
            Menu("MO Drive") {
                Button("Insert…") {
                    emulatorViewModel.browseAndInsertMO()
                }
                .disabled(isDisabled)
                Button("Eject") {
                    emulatorViewModel.ejectMO()
                }
                .disabled(isDisabled)
            }
            // P676: CD-ROM ドライブ(ID6 固定スロット)。MO と同型。
            Menu("CD-ROM Drive") {
                Button("Insert…") {
                    emulatorViewModel.browseAndInsertCD()
                }
                .disabled(isDisabled)
                Button("Eject") {
                    emulatorViewModel.ejectCD()
                }
                .disabled(isDisabled)
            }
        }
    }

    @ViewBuilder
    private func fddMenuItems(drive: Int) -> some View {
        Button("Insert…") {
            emulatorViewModel.browseAndMountFDD(drive: drive)
        }
        .disabled(isDisabled)
        Button("Insert (Write-Protected)…") {
            emulatorViewModel.browseAndMountFDD(drive: drive, forceWriteProtect: true)
        }
        .disabled(isDisabled)
        Button("Eject") {
            emulatorViewModel.ejectFDD(drive: drive)
        }
        .disabled(isDisabled)
    }
}

/// P578 — XM6 の「ツール」メニューに倣った、ホスト側ユーティリティ用のメニュー。
/// 現状は「イメージ作成」(空の FD / SASI / SCSI イメージ)のみ。
///
/// ★D-49/P563 規律を厳守: この構造体は `ObservableObject` を
/// `@ObservedObject`/`@StateObject` で**一切観測しない**(観測すると
/// `App.body` = `.commands{}` 全体が再評価され、開いていたサブメニューが閉じる)。
/// 各項目は `BlankImagePresentation.shared` へ種別を書き込むだけで、
/// 実際の提示は `RootView` の `.sheet(item:)` が行う。
///
/// ★`.disabled()` は付けない: コアを一切呼ばないホスト側ユーティリティ
/// (ファイルを 1 個作るだけ)であり、BIOS 未設定/電源 OFF でも実行して問題ない
/// (P574 の判断を踏襲)。ショートカットは既存と衝突させないため付けない。
struct ToolsCommands: Commands {
    var body: some Commands {
        CommandMenu("Tools") {
            Menu("Create Image") {
                Button("Floppy Disk Image…") {
                    BlankImagePresentation.shared.kind = .floppy
                }
                Button("SASI Hard Disk Image…") {
                    BlankImagePresentation.shared.kind = .sasi
                }
                Button("SCSI Hard Disk Image…") {
                    BlankImagePresentation.shared.kind = .scsi
                }
                Button("MO Disk Image…") {
                    BlankImagePresentation.shared.kind = .mo
                }
            }
        }
    }
}

/// P190 — 標準「表示(View)」メニューにウィンドウスケールを追加。
/// P190b — AppKit の "Enter Full Screen" が生成されなかったため ^⌘F も自前で提供。
struct DisplayCommands: Commands {
    // P212 — ウィンドウスケールは固定 4:3 基準(WindowScaler.baseLogicalSize)になり
    // framebuffer 実寸に依存しなくなったため viewModel 参照は不要。
    // P199 — 表示フィルタ。@AppStorage は選択時のみ Commands ツリーを再評価しチェックを更新。
    @AppStorage("displayFilter") private var displayFilter: String = "smooth"
    // P665 — 走査線エフェクト。displayFilterと同じ「設定画面+表示メニュー」
    // 二重配線パターン(同一キーを共有する別の@AppStorage宣言)。
    @AppStorage("scanlineEffect") private var scanlineEffect: Bool = false

    var body: some Commands {
        CommandGroup(after: .toolbar) {
            Button("Full Screen") {
                WindowScaler.toggleFullScreen()
            }
            .keyboardShortcut("f", modifiers: [.control, .command])

            Divider()

            // P192 — 3x は一般的なディスプレイに収まらないため 1x / 1.5x / 2x に変更。
            Button("Window Scale 1x") {
                WindowScaler.applyScale(1.0)
            }
            .keyboardShortcut("1", modifiers: .command)

            Button("Window Scale 1.5x") {
                WindowScaler.applyScale(1.5)
            }
            .keyboardShortcut("2", modifiers: .command)

            Button("Window Scale 2x") {
                WindowScaler.applyScale(2.0)
            }
            .keyboardShortcut("3", modifiers: .command)

            Divider()

            // P199 — 表示フィルタ Smooth/Sharp(inline Picker=メニュー内でチェック付き 2 項目)。
            Picker(selection: $displayFilter) {
                ForEach(DisplayFilter.allCases) { f in
                    Text(f.labelKey).tag(f.rawValue)
                }
            } label: {
                Text("表示フィルタ")
            }
            .pickerStyle(.inline)

            // P665 — 走査線エフェクト(P664設定画面トグルと同一キーを共有)。
            // P667 — 真因確定: P666のButton化は無効だった(制御の種類を
            // 変えても症状が不変=原因は別にあった)。独立再現アプリでの
            // 実測(列投影法、AppKit標準メニュー項目「フルスクリーンにする」
            // (アイコン付き)がこのButtonの直後に続き、Divider無しで同一
            // セクションとなりアイコン分(+19px実測)字下げされていたことを
            // 確認。ネイティブToggleへ差し戻し、末尾にDividerを追加して
            // AppKit標準項目とセクションを分離する。
            Toggle("走査線エフェクト", isOn: $scanlineEffect)

            Divider()
        }
    }
}

/// P548 — 項目の並び順を XM6 の「View」メニュー構成(`mfc/mfc_res.rc`)に倣った
/// 論理グループ順へ整理。ラベル・開くウィンドウ・キーボードショートカットは
/// P348 以来一切変更していない。
/// P566 — グループ境界の `Divider()` をネスト(`Menu`)による階層化へ置き換えた。
/// P347/P348/P547 でサブメニューが開いた直後に自動で閉じる不具合(D-49)のため
/// 階層化は 2 度撤回されていたが、P563 で真因(`App` 構造体が `@StateObject` を
/// 直接保持していたことによる `App.body`=`.commands{}` 全体の不要な再評価 → AppKit
/// 側 NSMenu 再構築)を確定・修正済み。本サイクルでその上に階層化 UI を導入した。
struct MonitorCommands: Commands {
    @Environment(\.openWindow) private var openWindow
    var body: some Commands {
        CommandMenu("Monitor") {
            Menu("System") {
                Button("Performance Viewer") { openWindow(id: "monitor-perf") }
                    .keyboardShortcut("7", modifiers: [.command, .option])
            }

            Menu("Processor") {
                Button("CPU / Register Viewer") { openWindow(id: "monitor-cpu") }
                    .keyboardShortcut("1", modifiers: [.command, .option])
                // P600 — メモリビューア(⌘⌥2、Docs/03 で予約済みだった枠を実配線)と
                // メモリマップビューア(⌘⌥R、未使用キーを新規割当)。
                Button("Memory Viewer") { openWindow(id: "monitor-memory") }
                    .keyboardShortcut("2", modifiers: [.command, .option])
                Button("Memory Map Viewer") { openWindow(id: "monitor-memory-map") }
                    .keyboardShortcut("r", modifiers: [.command, .option])
            }

            // Device(表示制御系レジスタ/状態モニタ)
            // P696 — P692〜P694 の追加で 10 項目まで膨らんだ Device グループを
            // Device / Sound / Peripherals の 3 グループへ再分割した。
            // 各項目のラベル・開くウィンドウ・キーボードショートカットは一切変更していない。
            Menu("Device") {
                Button("CRTC Viewer") { openWindow(id: "monitor-crtc") }
                    .keyboardShortcut("4", modifiers: [.command, .option])
                Button("Video Controller Viewer") { openWindow(id: "monitor-vc") }
                    .keyboardShortcut("5", modifiers: [.command, .option])
                Button("BG Viewer") { openWindow(id: "monitor-bg") }
                    .keyboardShortcut("6", modifiers: [.command, .option])
                // P550 — パレットモニタ(⌘⌥P、未使用キーを新規割当)。
                Button("Palette Viewer") { openWindow(id: "monitor-palette") }
                    .keyboardShortcut("p", modifiers: [.command, .option])
            }

            // Sound(音源系モニタ) — P696 で Device から分離。
            Menu("Sound") {
                // P484 — サウンドモニタ(⌘⌥3、既存予約分を実配線)。当初サイクルはADPCMのみ。
                Button("Sound Viewer") { openWindow(id: "monitor-sound") }
                    .keyboardShortcut("3", modifiers: [.command, .option])
                // P631 — OPM シンセサイザーパネル(⌘⌥Y、未使用キーを新規割当)。
                Button("OPM Synthesizer Viewer") { openWindow(id: "monitor-opm-synth") }
                    .keyboardShortcut("y", modifiers: [.command, .option])
            }

            // Peripherals(入力/ストレージ/MIDI/RTC 等の周辺機器) — P696 で Device から分離。
            Menu("Peripherals") {
                // P551 — 入力モニタ(⌘⌥I、未使用キーを新規割当)。
                Button("Input Viewer") { openWindow(id: "monitor-input") }
                    .keyboardShortcut("i", modifiers: [.command, .option])
                // P692 — ストレージモニタ(⌘⌥E、未使用キーを新規割当)。
                // ★"D"(Disk の頭文字)は macOS システム標準の ⌥⌘D(Dock の自動的に
                //   非表示/表示)と衝突しアプリ側メニューが働かないため採らない。
                Button("Storage Viewer") { openWindow(id: "monitor-storage") }
                    .keyboardShortcut("e", modifiers: [.command, .option])
                // P693 — MIDIモニタ(⌘⌥N、未使用キーを新規割当)。
                // ★"M"(MIDI の頭文字)は macOS システム標準の ⌥⌘M(ウインドウを
                //   しまう)と衝突するため採らない。"K" は Input メニューの
                //   ソフトウェアキーボード(⌘⌥K)と紛らわしいため同じく採らない。
                Button("MIDI Viewer") { openWindow(id: "monitor-midi") }
                    .keyboardShortcut("n", modifiers: [.command, .option])
                // P694 — RTCモニタ(⌘⌥L、未使用キーを新規割当。L = cLock)。
                // ★"R" は Processor メニューの Memory Map Viewer(⌘⌥R)で使用済み、
                //   "T" は Text Plane Viewer(⌘⌥T)、"C" は BG+Sprite Composite
                //   Viewer(⌘⌥C)で使用済みのため、いずれも採れない。
                Button("RTC Viewer") { openWindow(id: "monitor-rtc") }
                    .keyboardShortcut("l", modifiers: [.command, .option])
            }

            // Video(生プレーン/VRAM内容)
            Menu("Video") {
                // P348 — BGページ/グラフィック面プレビュー(サブメニュー化は P566 で完了)。
                Button("Graphics Page Viewer") { openWindow(id: "monitor-grp-page") }
                    .keyboardShortcut("g", modifiers: [.command, .option])
                Button("BG Page Viewer") { openWindow(id: "monitor-bg-page") }
                    .keyboardShortcut("b", modifiers: [.command, .option])
                Button("Sprite Table Map Viewer") { openWindow(id: "monitor-sprite-table") }
                    .keyboardShortcut("9", modifiers: [.command, .option])
                Button("Sprite Viewer") { openWindow(id: "monitor-sprite") }
                    .keyboardShortcut("8", modifiers: [.command, .option])
            }

            // Renderer(派生/合成済みバッファ)
            Menu("Renderer") {
                Button("Text Plane Viewer") { openWindow(id: "monitor-text") }
                    .keyboardShortcut("t", modifiers: [.command, .option])
                Button("BG+Sprite Composite Viewer") { openWindow(id: "monitor-bgsp-composite") }
                    .keyboardShortcut("c", modifiers: [.command, .option])
                Button("Renderer Viewer") { openWindow(id: "monitor-renderer") }
                    .keyboardShortcut("0", modifiers: [.command, .option])
            }
        }
    }
}

// P228 — ソフトウェアキーボード(フルJISレプリカ)を開く入力メニュー。
// ⌘⌥2 は MonitorCommands のコメントで Memory Viewer 用に予約済みのため ⌘⌥K を採用。
struct SoftKeyboardCommands: Commands {
    // P552 — トランジェントメッセージ表示用。EmulatorCommands(P546)と同じく
    // @ObservedObject にはしない(観測すると Commands ツリー全体の再評価を招くため)。
    let emulatorViewModel: EmulatorViewModel
    @Environment(\.openWindow) private var openWindow
    // P237 — ラベルをキャプチャ状態に追従させる。InputManager 本体ではなく専用の
    // MouseCaptureState を観測する(InputManager.keyboardState はキー入力毎に変化し
    // Commands 全体の再評価を招くため — EmulatorView.swift:6-9 と同じ理由)。
    @ObservedObject private var captureState = MouseCaptureState.shared
    var body: some Commands {
        CommandMenu("Input") {
            Button("Software Keyboard") { openWindow(id: "soft-keyboard") }
                .keyboardShortcut("k", modifiers: [.command, .option])

            Divider()

            Button(captureState.isCaptured ? "Release Mouse Capture" : "Capture Mouse") {
                let nowCaptured = InputManager.shared.toggleMouseCapture()
                emulatorViewModel.showTransientMessage(
                    String(localized: nowCaptured ? "Mouse Capture: ON" : "Mouse Capture: OFF"))
            }
            .keyboardShortcut("m", modifiers: [.command, .option])
        }
    }
}
