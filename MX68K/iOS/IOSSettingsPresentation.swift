//
//  IOSSettingsPresentation.swift
//  MX68K-iOS
//
//  P706 §D-2 — iOS 専用の「設定をどう出すか」層。
//
//  macOS では設定画面の提示が 2 経路(⌘, の独立 `Window` と、BIOS 未設定時の
//  `RootView` フォールバック)に分かれている(MX68KApp.swift:39-40, :102-107)。
//  iOS も同じ 2 経路を持つが、`Window` シーンが無いのでシート提示に置き換える。
//  ここはその提示だけを担い、設定の中身は共有の `SettingsView` がそのまま担当する。
//
//  ★`.sheet` 修飾子を同一 View へ 2 つ積まない(P578/P585 の既知の罠。片方しか
//    機能しなくなる)。提示先は単一の `.sheet(item:)` に集約し、種別は下の enum で
//    表す。将来シートが増えても case を足すだけで、修飾子は 1 つのまま。
//

import SwiftUI
import UniformTypeIdentifiers   // P713: 共有 `.fileImporter` の `allowedContentTypes: [.data]`。

/// iOS で提示するシートの種別。`.sheet` を 1 本に保つための器。
enum IOSSheetKind: Int, Identifiable {
    case settings
    case loadState   // P725
    case archivePicker   // P729 — zip 内にディスクイメージが複数あったときの選択

    var id: Int { rawValue }
}

/// 状態表示帯の右端に置くボタン 6 つ(P706 改訂 2 — macOS 版と最低限同等の操作を揃える。
/// P709 で Interrupt を追加)。
///
///  * Hard Reset …… 確認ダイアログを出してから実行(文言は macOS と逐語同一)。
///  * Soft Reset …… 確認ダイアログ無しで即時実行(macOS `ToolbarView` と同じ)。
///  * Interrupt(NMI) …… 確認ダイアログ無しで即時実行(P709、macOS `ToolbarView` と同じ)。
///  * FDD0… …… FDD0 のディスクイメージを選ぶ。**設定タブではない**のは
///    macOS でも同じ(ディスク選択は File メニュー側の機能であり、
///    `Settings/` には存在しない — P706 §S-1)。
///  * FDD1… …… FDD1 のディスクイメージを選ぶ(同じ経路をドライブ番号だけ変えて再利用)。
///  * (Spacer で分離) ソフトキーボード …… P710。`canToggleSoftKeyboard` が真の
///    コンテキスト(= 幅ゲート通過時)にだけ現れる 7 つ目のボタン。
///  * (同グループ) 歯車 …… 設定シート(`SettingsView(showCloseButton: true)`)を開く。
///
/// ★並び順は macOS `ToolbarView.swift`(Hard Reset → Soft Reset → Interrupt → FDD0 → FDD1)と
///   ユーザーの指摘(2026-08-30)に合わせて確定した。歯車だけは macOS に対応物が無い
///   (macOS の設定は独立 Window/メニュー経由)ため、視覚的に分離する目的で右端へ
///   Spacer で押し出している。
///
/// P708 — `axis` を追加した。既定値 `.horizontal` により**縦向き経路(および他の
/// 既存呼び出し)はレイアウト的に完全な no-op** であり、これがスコープ封じ込めの根拠。
/// 横向きでは左のピラーボックス余白へ縦積みで置くため `.vertical` を渡す。
/// ビュー同一性を保つため `if/else` で 2 種類の Stack を書き分けず、`AnyLayout`
/// (iOS 16.0+、本ターゲットの `IPHONEOS_DEPLOYMENT_TARGET` は 16.0)で軸だけを差し替える。
struct IOSSettingsBarButtons: View {
    @ObservedObject var viewModel: MX68KiOSViewModel
    @EnvironmentObject var configManager: ConfigManager
    @Binding var activeSheet: IOSSheetKind?
    /// 取り込み失敗を無言にしないための 1 行(§B-6)。表示はルートビューが行う。
    @Binding var importError: String?
    /// P708 — ボタンの並び軸。既定 `.horizontal` = P706 までと同一の見た目。
    var axis: Axis = .horizontal
    /// P710 §C-4 — ソフトキーボードの表示状態(所有者はルートビュー)。
    @Binding var showSoftKeyboard: Bool
    /// P710 §C-4 — 現在のコンテキストでソフトキーボードを提示してよいか
    /// (§記号表 `SKBD_MIN_WIDTH`。判定式は `MX68KiOSRootView.softKeyboardAvailable`
    ///  ただ 1 本に閉じており、ここへは結果だけが渡る)。
    /// ★**既定値を与えない** —— 呼び出し側 2 箇所(topBand / sideButtonBand)に必ず
    ///   明示的な判断をさせる。`axis` が既定値を持つのはレイアウト no-op を保証する
    ///   ためだが、こちらは「うっかり既定で出てしまう/出なくなる」を避けたい。
    var canToggleSoftKeyboard: Bool
    /// P713 §方針2 — FDD の Menu から「Select…」が選ばれたドライブ番号(nil = 非表示)。
    /// 所有者はルートビュー(`showSoftKeyboard` / `importError` と同じ流儀)。
    /// FDD0/FDD1 で 1 つの状態を共有し、提示も単一の共有 `.fileImporter` 1 個に集約する。
    @Binding var pendingFDDDrive: Int?
    /// P714 — 仮想ジョイスティック(オンスクリーン D-pad)の表示状態。
    /// 所有者はルートビュー(`showSoftKeyboard` と同じ流儀)。
    @Binding var showVirtualPad: Bool
    /// P714 — タッチマウス(全画面相対トラックパッド)モードの ON/OFF。
    /// 仮想パッドとは**排他**(マウスモード中は仮想パッドを隠す)。
    @Binding var mouseModeEnabled: Bool
    /// P714 — 現在のコンテキストで仮想パッドを提示してよいか。
    /// 判定式はルートビューの `virtualPadAvailable` ただ 1 本に閉じており、
    /// ここへは結果だけが渡る(`canToggleSoftKeyboard` と同じ流儀 = 閾値・条件を
    /// 2 箇所に書かない)。**トグルボタンの無効化条件と自動非表示条件は同一の値**。
    var canToggleVirtualPad: Bool

    /// P708 §C-3 — 軸だけを差し替える。spacing は水平/垂直で共通(14)。
    private var layout: AnyLayout {
        axis == .vertical ? AnyLayout(VStackLayout(spacing: 14))
                          : AnyLayout(HStackLayout(spacing: 14))
    }

    /// P708 §C-3-1 — 歯車を視覚的に分離する区切り。
    /// 水平軸では貪欲な `Spacer` が歯車を右端へ押し出す(P706 の意図どおり)。
    /// **垂直軸で同じ `Spacer` を使うと歯車を余白の最下端まで押し出してしまう**ため、
    /// 固定間隔(20pt)へ差し替える。「視覚的に分離する」意図は保ったまま、
    /// 列が内容にフィットして上寄せに収まる。
    @ViewBuilder private var gearSeparator: some View {
        if axis == .vertical {
            Color.clear.frame(height: 20)
        } else {
            Spacer(minLength: 0)
        }
    }

    /// P710 §C-4 — トグルボタンのアイコン名。
    ///
    /// ★SF Symbol 名の誤りは**コンパイルエラーにならず、無言で空グリフになる**
    ///   (計画 §根拠種別 E-4 = `[training knowledge]`。リポジトリ内に一次情報源が無い)。
    ///   H-8 でどちらかの状態が空白だった場合の退避策は、下の
    ///   `useCompactDownGlyph` を `false` にする **1 行だけ**である:
    ///   両状態とも `keyboard`(SF Symbols 初版から存在する基本シンボル)を使い、
    ///   状態は `.accessibilityLabel` とキーボードが実際に出ているかどうかで示す。
    private static let useCompactDownGlyph = true

    private var softKeyboardGlyph: String {
        (Self.useCompactDownGlyph && showSoftKeyboard) ? "keyboard.chevron.compact.down"
                                                       : "keyboard"
    }

    var body: some View {
        // ★ユーザー指摘(2026-08-30)により、並び順をmacOS ToolbarView.swift(:9-51)と
        //   同じ(Hard Reset → Soft Reset → Interrupt → FDD0 → FDD1、Interrupt は P709 で追加)
        //   へ揃え、歯車だけを Spacer で右端へ分離した
        //   (Hard/Soft/Interrupt/FDD0/FDD1は左詰めのまま)。
        layout {
            Button {
                viewModel.showHardResetConfirm = true
            } label: {
                Image(systemName: "arrow.counterclockwise")
                    .imageScale(.large)
            }
            .accessibilityLabel("Hard Reset")
            .modifier(IOSBarTouchTarget(active: axis == .vertical))

            Button {
                viewModel.softReset()
            } label: {
                Image(systemName: "arrow.uturn.backward")
                    .imageScale(.large)
            }
            .accessibilityLabel("Soft Reset")
            .modifier(IOSBarTouchTarget(active: axis == .vertical))

            // P709 — NMI(Interrupt)。macOS `ToolbarView.swift:38` と同じ SF Symbol・
            // 同じ「確認ダイアログ無しで即時実行」。
            Button {
                viewModel.nmi()
            } label: {
                Image(systemName: "exclamationmark.triangle")
                    .imageScale(.large)
            }
            .accessibilityLabel("Interrupt")
            .modifier(IOSBarTouchTarget(active: axis == .vertical))

            // P713 — FDD0 / FDD1。macOS `ToolbarView` は Select(folder)と
            // Eject(eject)の 2 ボタン構成だが、iOS では同一行に複数 Button を
            // 並べると隣接ボタンが同時発火する不具合を P712 で経験しているため、
            // 1 つの `Menu`(タップで Select… / Eject を出す)へまとめる。
            fddMenu(drive: 0, titleKey: "FDD0…")
            fddMenu(drive: 1, titleKey: "FDD1…")

            // P725 — セーブステート。FDD0/1 と同じ `Menu` パターン(隣接 Button 同時
            // 発火回避、P712f)。Save State は即実行、Load State… は一覧シートを開く。
            // ★`isStateOperationPending` の間は Menu 自体を無効化する。ポーリング
            //   未完了のまま 2 つ目の save/load がキューされると、
            //   `stateOpTask?.cancel()`(P726 —— 旧`stateOpTimer?.invalidate()`から
            //   Task化)が 1 つ目の結果通知を無言で握り潰す
            //   レース(Bridge 側キューは 1 操作分のみ、~1 フレーム = 18ms の狭い窓)
            //   を防ぐ。
            Menu("State") {
                Button("Save State") {
                    viewModel.saveState()
                }
                Button("Load State…") {
                    activeSheet = .loadState
                }
            }
            .disabled(viewModel.isStateOperationPending)
            .modifier(IOSBarTouchTarget(active: axis == .vertical))

            gearSeparator

            // P710 §C-4 — ソフトキーボードのトグル。
            // ★区切りの**右側**(歯車と同じグループ)へ置く: 現行の並びは
            //   「左 = マシン操作(Hard/Soft Reset・Interrupt・FDD0/1、macOS
            //   ToolbarView と同順)/ 区切り / 右 = アプリ UI(歯車)」という構造を
            //   持つ。ソフトキーボードのトグルはマシン操作ではなく**表示切替**なので
            //   右グループが既存の分類に忠実である。macOS 側でも Input メニュー
            //   (`SoftKeyboardCommands`)という別グループにあり、`ToolbarView` には
            //   無い —— 左グループへ入れると macOS との並び対応が崩れる。
            if canToggleSoftKeyboard {
                Button {
                    withAnimation(.easeOut(duration: 0.2)) { showSoftKeyboard.toggle() }
                } label: {
                    Image(systemName: softKeyboardGlyph)
                        .imageScale(.large)
                }
                .accessibilityLabel(showSoftKeyboard ? "Hide Software Keyboard"
                                                     : "Show Software Keyboard")
                .modifier(IOSBarTouchTarget(active: axis == .vertical))
            }

            // P714 — 仮想パッド / タッチマウスのトグル。
            // ★ソフトキーボードのトグルと同じく区切りの**右側**(表示切替グループ)へ置く
            //   —— どちらもマシン操作ではなく「画面に何を重ねるか」の切替である。
            //
            // ★`.buttonStyle(.borderless)` は付けない: P712f の隣接ボタン同時発火は
            //   Select/Eject のような「機能的に密結合したペア」で起きたものであり、
            //   この 2 つは互いに独立したトグル(既存ツールバーの独立ボタン 5 個と同型、
            //   無症状で出荷済み)である(P714 計画 §変更内容 4)。
            Button {
                withAnimation(.easeOut(duration: 0.2)) { showVirtualPad.toggle() }
            } label: {
                Image(systemName: showVirtualPad ? "gamecontroller.fill" : "gamecontroller")
                    .imageScale(.large)
            }
            .disabled(!canToggleVirtualPad)
            .accessibilityLabel(showVirtualPad ? "Hide Virtual Pad" : "Show Virtual Pad")
            .modifier(IOSBarTouchTarget(active: axis == .vertical))

            Button {
                withAnimation(.easeOut(duration: 0.2)) { mouseModeEnabled.toggle() }
            } label: {
                Image(systemName: "cursorarrow")
                    .imageScale(.large)
                    // ★ON 状態を色で示す(SF Symbol に fill 対の無いシンボルのため)。
                    .foregroundStyle(mouseModeEnabled ? Color.yellow : Color.green)
            }
            .accessibilityLabel(mouseModeEnabled ? "Disable Touch Mouse" : "Enable Touch Mouse")
            .modifier(IOSBarTouchTarget(active: axis == .vertical))

            Button {
                activeSheet = .settings
            } label: {
                Image(systemName: "gearshape.fill")
                    .imageScale(.large)
            }
            .accessibilityLabel("Settings")
            .modifier(IOSBarTouchTarget(active: axis == .vertical))
        }
        // P713 — 単一の共有 `.fileImporter`(FDD0 / FDD1 で 1 個)。
        // ★行ごと(ドライブごと)に `.fileImporter` を持たせる旧方式は、同一 View に
        //   複数の提示器が同居することによるシート誤発火の疑いで P712c が撤回済み。
        .fileImporter(isPresented: Binding(
            get: { pendingFDDDrive != nil },
            set: { newValue in
                guard !newValue else { return }
                // P712e/f の設計を踏襲する。キャンセル時は completion ハンドラが
                // 一度も呼ばれない(実機ログで確認済み)。成功時は completion が同期的に
                // `pendingFDDDrive` をクリアするので、次の RunLoop の時点ではこの set を
                // 引き起こした値と一致しなくなっている(= 処理済み)。一致したままなら
                // completion が呼ばれなかった = キャンセルとみなしてここでクリアする。
                // ★set の中で即座にクリアしない(completion より先にクリアすると
                //   成功時の処理が空振りする — P712c/P712d の教訓)。
                let dismissedDrive = pendingFDDDrive
                DispatchQueue.main.async {
                    if pendingFDDDrive == dismissedDrive {
                        pendingFDDDrive = nil
                    }
                }
            }
        ), allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            guard let drive = pendingFDDDrive else { return }
            pendingFDDDrive = nil
            switch result {
            case .success(let urls):
                guard let url = urls.first else {
                    ImportedFileStore.logCancel(destination: .disksDir)
                    return
                }
                // P729 — zip はコピーインせず、直接 ZIPFoundation で展開する
                // (通常ファイルの `ImportedFileStore.importFile` copy-in 経路は
                //  zip ファイル自体を disks/ へコピーする意味を持たないため通らない)。
                // ★`configManager.config.fdd.setLastPath` も呼ばない —— 展開先は
                //   一時ディレクトリであり、次回起動時には既にスイープ済みで無効な
                //   パスになる。macOS 版は展開先の一時パスもそのまま永続化している
                //   (`onFDDMounted`、:36-39)が、それは次回起動時に無効パスを参照する
                //   既存の粗さであり、新規移植で複製しない(P729 計画 §変更内容5c)。
                if url.pathExtension.lowercased() == "zip" {
                    importError = nil
                    viewModel.handleArchiveSelection(drive: drive, zipURL: url)
                    return
                }
                do {
                    let path = try ImportedFileStore.importFile(from: url, to: .disksDir)
                    importError = nil
                    guard viewModel.mountDisk(drive: drive, path: path) else { return }
                    // §D-3: config への保存は ConfigManager を所有する側(= ルートビューが
                    // 環境へ入れたこの 1 インスタンス)で行う。VM は ConfigManager を持たない。
                    // ★ドライブ分岐は既存の集約点 `setLastPath(_:_:)`(P684)を再利用し、
                    //   `lastFDD1Path` 等を新規に直接参照しない。
                    configManager.config.fdd.setLastPath(drive, path)
                    configManager.save()
                } catch {
                    importError = ImportedFileStore.message(for: error)
                }
            case .failure(let error):
                if (error as NSError).code == NSUserCancelledError {
                    ImportedFileStore.logCancel(destination: .disksDir)
                } else {
                    ImportedFileStore.logPickerFailure(destination: .disksDir, error: error)
                    importError = ImportedFileStore.message(for: error)
                }
            }
        }
        .font(.system(size: 14))
        .tint(.green)
    }

    /// P713 §方針2 — FDD 1 台分の Menu(Select… / Eject)。
    ///
    /// ラベル文言は P706 以来の "FDD0…" / "FDD1…" を維持する —— トップバーの幅制約
    /// (P707/P708)を踏まえ、マウント中ファイル名の表示などの拡張はスコープ外。
    /// Select… は `pendingFDDDrive` を立てるだけで、実際の提示は `body` 側の
    /// 単一の共有 `.fileImporter` が行う(P712c と同じ形)。
    /// P730 — Eject の無効化判定は Core 側の実マウント状態(`mx68k_fdd_is_inserted`)を
    /// 参照する、config.json の lastPath には依存しない(P729 の ZIP 経由マウントが
    /// lastPath を永続化しない設計と整合させるため)。
    private func fddMenu(drive: Int, titleKey: LocalizedStringKey) -> some View {
        Menu(titleKey) {
            Button("Select…") {
                ImportedFileStore.logOpen(destination: .disksDir)
                pendingFDDDrive = drive
            }
            Button("Eject") {
                viewModel.ejectFDD(drive: drive)
                configManager.config.fdd.setLastPath(drive, "")
                configManager.save()
            }
            .disabled(!mx68k_fdd_is_inserted(Int32(drive)))
        }
        .modifier(IOSBarTouchTarget(active: axis == .vertical))
    }
}

/// P708 §C-3-2 — 縦積み時だけ各ボタンへ HIG 最小タップ領域(44×44pt)を与える。
///
/// ★`.contentShape(Rectangle())` は必須 —— これが無いと拡大した frame は
///   ヒットテストされず、ラベルの実寸(`imageScale(.large)` で 20-24pt 程度)だけが
///   タップ可能なまま残る。
///
/// 水平軸(縦向き)側は `active == false` で**素通し**にしてある: 現行の縦向きも
/// 44pt 未満だが、P708 は横向き限定スコープであり、縦向きのタップ領域拡大は
/// **意図的に対象外**(見落としではない)。
struct IOSBarTouchTarget: ViewModifier {
    let active: Bool

    @ViewBuilder func body(content: Content) -> some View {
        if active {
            content
                .frame(minWidth: 44, minHeight: 44)
                .contentShape(Rectangle())
        } else {
            content
        }
    }
}

/// 単一の `.sheet(item:)` を張るだけのモディファイア(CR-8: 二重 `.sheet` の禁止)。
///
/// 環境オブジェクトはシート側へ**明示的に**渡す。macOS の
/// `Window("Settings")`(MX68KApp.swift:102-107)が 3 つすべてを明示的に付けているのと
/// 同じ形にして、提示経路による注入差を作らない。
struct IOSSettingsSheet: ViewModifier {
    @Binding var activeSheet: IOSSheetKind?
    let configManager: ConfigManager
    let settingsViewModel: SettingsViewModel
    let viewModel: MX68KiOSViewModel

    func body(content: Content) -> some View {
        content.sheet(item: $activeSheet) { kind in
            switch kind {
            case .settings:
                SettingsView(showCloseButton: true)
                    .environmentObject(configManager)
                    .environmentObject(settingsViewModel)
                    .environmentObject(viewModel)
            case .loadState:   // P725
                StateListView()
                    .environmentObject(viewModel)
            case .archivePicker:   // P729
                // ★他の 2 ケースと違い、このシートは `activeSheet` を直接立てる
                //   操作(ボタン)からではなく、zip 展開の**結果**として出る。
                //   VM 側の `showArchivePicker` を唯一の起点とし、ルートビューの
                //   `.onChange` がそれを `activeSheet` へ転写する
                //   (MX68KiOSApp.swift の該当箇所を参照)。
                ArchiveEntryPickerView(
                    images: viewModel.archivePickerImages,
                    onSelect: { viewModel.completeArchiveSelection($0) },
                    onCancel: { viewModel.cancelArchiveSelection() }
                )
            }
        }
    }
}

extension View {
    func iosSettingsSheet(activeSheet: Binding<IOSSheetKind?>,
                          configManager: ConfigManager,
                          settingsViewModel: SettingsViewModel,
                          viewModel: MX68KiOSViewModel) -> some View {
        modifier(IOSSettingsSheet(activeSheet: activeSheet,
                                  configManager: configManager,
                                  settingsViewModel: settingsViewModel,
                                  viewModel: viewModel))
    }
}
