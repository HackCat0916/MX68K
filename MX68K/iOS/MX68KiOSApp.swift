//
//  MX68KiOSApp.swift
//  MX68K-iOS
//
//  P702: iOS/iPadOS target scaffold.
//  P703: PlaceholderView を廃し、Metal 表示 + CADisplayLink フレーム駆動の
//        実アプリ本体へ差し替えた。
//  P705: 物理/Bluetooth キーボード入力を配線。
//  P706: 設定画面(BIOS / Hardware)とブートディスク選択を配線し、
//        `xcrun simctl` による手動プロビジョニング手順を廃止した。
//  P708: 横向き(上下余白 0pt)で帯が映像へ重なる問題を解消するため、
//        余白のある軸を `DisplayViewport.face()` で判定し、余白が十分広ければ
//        左右のピラーボックス余白へ帯を再配置する。縦向きは同じ式が構造的に
//        従来の上下配置を選ぶため無変更。
//

import SwiftUI
import UIKit   // BIOS 未設定オーバーレイの下地に UIColor.systemBackground を使う。

/// P710改訂2 — `SoftKeyboardView`の実描画サイズを子から親へ伝搬するための
/// `PreferenceKey`。固定定数(静的読解による見積もり)がiPhone横向きで実測と
/// 食い違っていた反省(§改訂2)から、実サイズを都度測定する設計へ切り替えた。
private struct SoftKeyboardSizeKey: PreferenceKey {
    static var defaultValue: CGSize = .zero
    static func reduce(value: inout CGSize, nextValue: () -> CGSize) {
        value = nextValue()
    }
}

@main
struct MX68KiOSApp: App {
    var body: some Scene {
        WindowGroup {
            MX68KiOSRootView()
        }
    }
}

/// P703 — iOS のルートビュー。
///
/// ★`@StateObject` は App 構造体ではなくこのビューが持つ。App 構造体へ
/// `@StateObject` を置く設計は Monitor メニュー階層化(P566-P569)で
/// 実際に不具合の真因になった箇所であり、同じ形を再現しない。
///
/// P706 §D-1 — `ConfigManager` / `SettingsViewModel` も**このビューが所有する**。
/// ★`ConfigManager()` の生成は iOS 側でここ **1 箇所だけ**でなければならない
/// (2 インスタンスが同じ config.json へ書くと片方の変更が消える — §R-11 / CR-7)。
/// `MX68KiOSViewModel` には `ConfigManager` を持たせず、値(`EmulatorConfig`)を
/// 引数として渡す。
struct MX68KiOSRootView: View {
    @StateObject private var viewModel = MX68KiOSViewModel()
    @StateObject private var configManager = ConfigManager()
    @StateObject private var settingsViewModel = SettingsViewModel()

    @State private var activeSheet: IOSSheetKind?
    /// ブートディスク取り込みの失敗文言(§B-6: 無言で何も起きない状態を作らない)。
    @State private var importError: String?
    /// P710 — 画面上ソフトキーボードの表示状態。所有者はこのルートビュー
    /// (`IOSSettingsBarButtons` へは `@Binding` で渡す)。
    @State private var showSoftKeyboard = false
    /// P713 — FDD の Menu から「Select…」が選ばれたドライブ番号(nil = ピッカー非表示)。
    /// 所有者はこのルートビュー(`importError` / `showSoftKeyboard` と同じ流儀で
    /// `IOSSettingsBarButtons` へ `@Binding` で渡す)。FDD0/FDD1 で **1 つの状態を
    /// 共有**し、提示側も単一の共有 `.fileImporter` 1 個へ集約する(P712c の教訓)。
    @State private var pendingFDDDrive: Int?
    /// P714 — 仮想ジョイスティック(P715 でアナログスティック風パッドへ、P717 でボタン表記を A/B へ変更)の表示状態。
    /// P718 — 既定 false(ユーザーの明示的要望、2026-09-01)。起動時は非表示とし、
    /// ツールバーの「Pad」ボタンから手動で ON にする。`mouseModeEnabled` と揃う。
    /// 所有者はこのルートビュー(`showSoftKeyboard` と同じ流儀)。
    @State private var showVirtualPad = false
    /// P714 — タッチマウス(全画面相対トラックパッド)モード。既定 false。
    /// 仮想パッドとは排他 —— ON の間は仮想パッドを表示しない。
    @State private var mouseModeEnabled = false
    /// P716 — トリガーボタンのサイズ段階(0=Small / 1=Medium / 2=Large)。
    /// ★`TouchJoystickView` が同名キーを**独自に**読んでいるのと同じキーを、
    ///   ここでも読む。二重宣言だが、幅ゲート
    ///   (`TouchJoystickView.requiredWidth(forTriggerLevel:)`)の計算は
    ///   `geo.size` を持つこのビュー側でしか行えないため必要な最小限の重複である
    ///   (`virtualPadOpacity` は描画側だけで完結するのでこの重複を持たない)。
    ///   閾値そのものは `TouchJoystickView` の static 関数 1 本に閉じており、
    ///   ここで再計算はしない。
    @AppStorage("triggerButtonSizeLevel") private var triggerButtonSizeLevel: Int = 0
    /// P720 — ソフトキーボード帯の不透明度。既定 1.0(= 現状の完全不透明な見た目を維持)、
    /// 範囲 0.3〜1.0。`virtualPadOpacity` と同じ理由で `@AppStorage`(ホスト側の純粋な
    /// 表示設定であり `config.json` / ハードリセット同期の対象外)。
    /// ★`SoftKeyboardView` 自体(macOS と共有)は無変更のまま、iOS 専用の呼び出し側
    ///   `softKeyboardBand(containerSize:)` が帯全体へ 1 回だけ `.opacity()` を掛ける
    ///   —— P714 の `TouchJoystickView` がコンテナ全体へ 1 回だけ適用した設計と同原則で、
    ///   個々のキーへ個別に適用はしない(macOS 側の別ウィンドウ表示への影響はゼロ)。
    @AppStorage("softKeyboardOpacity") private var softKeyboardOpacity: Double = 1.0
    /// P710改訂2 — `SoftKeyboardView`の実測サイズ。初期値は§記号表の定数(未計測時の
    /// 目安)、実際に描画されると`GeometryReader`+`PreferenceKey`で更新される。
    @State private var measuredKeyboardSize = CGSize(
        width: MX68KiOSRootView.softKeyboardIntrinsicWidth,
        height: MX68KiOSRootView.softKeyboardIntrinsicHeight)

    var body: some View {
        ZStack(alignment: .top) {
            Color.black
                .ignoresSafeArea()

            EmulatorMetalView_iOS(viewModel: viewModel)

            if viewModel.needsConfiguration {
                // P706 §D-1 — BIOS 未設定時は設定画面を**全画面**で出す
                // (macOS の RootView フォールバック MX68KApp.swift:39-40 と同じ形)。
                // ★Metal ビューは裏で生かしたまま重ねる。ビュー階層から外して
                //   再マウントさせると、Apply 直後にレンダラを作り直すことになる。
                ZStack {
                    Color(uiColor: .systemBackground)
                        .ignoresSafeArea()
                    SettingsView(showCloseButton: false)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                // ★P706追記(2026-08-30、ユーザー指摘): macOSは
                //   ToolbarView(上部・Hard/Soft Reset/Interrupt/FDD)と
                //   StatusBarView(下部・状態表示)の2段構成——同一ユーザーが
                //   macOS版とiOS版を行き来する際の違和感を減らすため、iOSも
                //   同じ上下配置に揃える(帯のスタイル自体は既存の半透明オーバーレイを
                //   踏襲し、描画を覆う面積は増やさない)。
                //
                // ★P708: 横向きでは 4:3 面が画面高いっぱいまで拡がり**上下余白が 0pt**に
                //   なるため、上端/下端に貼り付く帯は幾何的に必ず映像へ重なる。そこで
                //   「向き」ではなく「**どちらの軸に余白があるか**」で配置を決める
                //   (§C-1)。判定には新しい幾何計算を作らず、レンダラが実際に使う
                //   `DisplayViewport.face()` と同一の純関数を呼ぶ(単一情報源)。
                //   縦向きは `face().x == 0` により構造的に従来の上下配置を選ぶ ——
                //   縦向き用の特別扱いは 1 行も書かない。
                GeometryReader { geo in
                    let sideW = DisplayViewport.face(container: geo.size).x
                    // P710 §C-2 — 幅ゲート。判定式は `softKeyboardAvailable(width:)`
                    // ただ 1 本であり、ボタンの表示可否とオーバーレイの表示可否は
                    // **同じ値**を使う(閾値リテラルを 2 箇所に書かない = CR-7)。
                    let canShowKeyboard = Self.softKeyboardAvailable(width: geo.size.width)
                    // P716 §変更内容 4 — 仮想パッド帯の幅ゲート。ソフトキーボードの
                    // `canShowKeyboard` と**同型**(オーバーレイの表示可否とトグル
                    // ボタンの有効/無効が同じ 1 つの値を共有する)だが、必要幅が
                    // トリガーサイズ設定に依存するため閾値は定数ではなく
                    // `requiredWidth(forTriggerLevel:)` の戻り値である。
                    // 判定式そのものは `TouchJoystickView` 側 1 本に閉じている。
                    let canShowVirtualPad =
                        geo.size.width >= TouchJoystickView.requiredWidth(forTriggerLevel: triggerButtonSizeLevel)
                    // P710 §C-2 — キーボード帯は `ZStack` の後段に重ねるだけで
                    // **レイアウトに参加しない**(Metal ビューの寸法も `geo.size` も
                    // 変えない)。これにより `DisplayViewport.face()` の入力が不変となり、
                    // P708 の帯配置はキーボードの表示/非表示にかかわらずビット同一になる。
                    ZStack(alignment: .bottom) {
                        // P714 — タッチ入力のオーバーレイ。
                        //
                        // ★帯(`Group` 以下)よりも **先**に置く = 帯の下へ重ねる。
                        //   タッチマウスは画面全域を覆うため、帯より後に置くと
                        //   ツールバーのボタン(マウスモードを解除する唯一の手段)まで
                        //   飲み込んでしまい、モードから抜けられなくなる。
                        //   帯の実描画部は `.background(.black.opacity(0.6))` を持つ
                        //   細い領域だけなので、そこ以外のタッチは下のレイヤーへ届く。
                        //
                        // ★どちらもレイアウトに参加しない(Metal ビューの寸法も
                        //   `geo.size` も変えない)—— P710 のソフトキーボード帯と同じ原則。
                        if mouseModeEnabled {
                            TouchMouseView()
                                .frame(maxWidth: .infinity, maxHeight: .infinity)
                        } else if showVirtualPad && Self.virtualPadAvailable && canShowVirtualPad {
                            TouchJoystickView(sideInset: sideW)
                                .frame(maxWidth: .infinity, maxHeight: .infinity)
                        }

                        Group {
                            if sideW >= Self.minSideBandWidth {
                                // 側方配置: 左=操作 / 右=状態(macOS の上=Toolbar / 下=StatusBar を
                                // 90° 回した先へそのまま写す。読み順も操作→状態で保たれる)。
                                HStack(spacing: 0) {
                                    // ★`width:` と `maxHeight:` は同一オーバーロードに無いため、
                                    //   可変長 frame で幅を上下限同値に固定する。
                                    //   `maxHeight: .infinity` が無いと alignment が効かず
                                    //   (高さ=内容高になり)HStack 既定の中央寄せになってしまう。
                                    sideButtonBand(canShowKeyboard: canShowKeyboard,
                                                   canShowVirtualPad: canShowVirtualPad)
                                        .frame(minWidth: sideW, maxWidth: sideW,
                                               maxHeight: .infinity, alignment: .top)
                                    Spacer(minLength: 0)
                                    sideStatusBand
                                        .frame(minWidth: sideW, maxWidth: sideW,
                                               maxHeight: .infinity, alignment: .bottom)
                                }
                            } else {
                                VStack(spacing: 0) {
                                    topBand(canShowKeyboard: canShowKeyboard,
                                            canShowVirtualPad: canShowVirtualPad)
                                    Spacer(minLength: 0)
                                    bottomBand
                                }
                            }
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)

                        if showSoftKeyboard && canShowKeyboard {
                            softKeyboardBand(containerSize: geo.size)
                                .transition(.move(edge: .bottom))
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    // ★ログは `body` の中では出さない(SwiftUI の body 計算は副作用を
                    //   置く場所ではない)。回転 / Stage Manager のリサイズ時だけ発火する。
                    //   ★`.onChange` は iOS 16 互換の**単一引数クロージャ形式**を使う
                    //     (iOS 17 の 2 引数形式は本ターゲットの deployment target 16.0 で使えない)。
                    .onAppear { logBandGeometry(container: geo.size) }
                    .onChange(of: geo.size) { newSize in
                        logBandGeometry(container: newSize)
                    }
                    // P710 — トグル操作の 1 行。★ここで `showSoftKeyboard` を
                    // **書き換えない**(読むだけ)。表示可否は上の `if` の
                    // `canShowKeyboard` が担っているので、縦へ回せばキーボードは消え、
                    // 横へ戻せば元の状態のまま復帰する(§C-2)。
                    .onChange(of: showSoftKeyboard) { _ in
                        logSoftKeyboardGeometry(container: geo.size, tag: "P710-SKBD-TOGGLE")
                    }
                    // P714 §変更内容 5(Code Review R-2)—— 仮想パッドが画面から
                    // 消える契機では、指を離す前でも必ず押しっぱなしを解除する。
                    // これを怠ると方向ビットが送出されたままになり、ゲスト側の
                    // キャラクターが勝手に動き続ける。
                    .onChange(of: mouseModeEnabled) { enabled in
                        if enabled {
                            // マウスモード ON = 仮想パッドは隠れる(排他)。
                            TouchJoystickInput.shared.releaseVirtualPad(reason: "mouseModeOn")
                        }
                    }
                    .onChange(of: showVirtualPad) { shown in
                        if !shown {
                            TouchJoystickInput.shared.releaseVirtualPad(reason: "padHidden")
                        }
                    }
                    // P716 — 幅ゲートは P714 §変更内容 5 が列挙した「仮想パッドが画面から
                    // 消える契機」の **3 本目**である(1: マウスモード ON / 2: トグル OFF)。
                    // 指で TRIG1 を押したまま横→縦へ回す、あるいは設定で Small→Large へ
                    // 変えて必要幅を超えると、パッドは消えるのに押下ビットは送出されたまま
                    // 残り、ゲスト側のキャラクターが勝手に動き続ける。上の 2 本と同じ
                    // 解除処理をこのゲートにも掛ける。
                    .onChange(of: canShowVirtualPad) { canShow in
                        if !canShow {
                            TouchJoystickInput.shared.releaseVirtualPad(reason: "padWidthGate")
                        }
                    }
                }
            }
        }
        // 環境オブジェクトの付与はここ 1 箇所に集約する(macOS の MX68KApp.swift:82-86
        // と同じ方針)。シート経路では IOSSettingsSheet が明示的に付け直す。
        .environmentObject(configManager)
        .environmentObject(settingsViewModel)
        .environmentObject(viewModel)
        .iosSettingsSheet(activeSheet: $activeSheet,
                          configManager: configManager,
                          settingsViewModel: settingsViewModel,
                          viewModel: viewModel)
        // P729 — zip 展開の結果として出る選択シートの配線。
        //
        // 他のシート(`.settings` / `.loadState`)はボタンが `activeSheet` を直接
        // 立てるが、これだけは VM 側の処理結果(zip に複数イメージがあった)から
        // 出るため、起点が `viewModel.showArchivePicker` になる。提示器は
        // P578/P585 以来の規約どおり単一の `.sheet(item:)` のままにしたいので、
        // ここで**片方向に転写**する。
        // ★`.onChange` は iOS 16 互換の単一引数クロージャ形式(既存の 5 本と同じ)。
        .onChange(of: viewModel.showArchivePicker) { shown in
            // true 方向のみ駆動する。false 方向をここで `activeSheet = nil` に
            // するとシート自身の dismiss と二重に競合するため、閉じる側は
            // `.sheet(item:)` の標準的な dismiss 経路へ任せる。
            if shown { activeSheet = .archivePicker }
        }
        // ★スワイプで閉じられた場合の後始末。行タップ / Cancel ボタンの経路は
        //   `completeArchiveSelection` / `cancelArchiveSelection` が
        //   `showArchivePicker` を false へ戻すが、**スワイプ dismiss ではその
        //   どちらも呼ばれない**。その場合 `activeSheet` だけが nil に戻り、
        //   `showArchivePicker` は true のまま取り残される —— 展開先の一時
        //   ディレクトリがリークするうえ、次に zip を選んでも true→true で
        //   上の `.onChange` が発火せずシートが二度と出なくなる。
        //   ここで取りこぼしを拾い、キャンセル扱い(一時ディレクトリの破棄)にする。
        .onChange(of: activeSheet) { sheet in
            if sheet == nil && viewModel.showArchivePicker {
                viewModel.cancelArchiveSelection()
            }
        }
        // P706 改訂 1 — 帯の Hard Reset ボタンの確認ダイアログ。
        // 文言・ボタン・ロールは macOS `ToolbarView.swift:20-28` を逐語再利用し、
        // 新しい文言を発明しない。★`.keyboardShortcut` は付けない(タッチ操作専用 UI で
        // あり、Bluetooth キーボード接続時のみ意味を持つ副次機能は本改訂のスコープ外)。
        .alert("Hard Reset", isPresented: $viewModel.showHardResetConfirm) {
            Button("Cancel", role: .cancel) { }
            Button("Reset", role: .destructive) {
                viewModel.hardReset()
            }
        } message: {
            Text("The current state will be lost. Execute a hard reset?")
        }
        // ★`.task` はビュー ID ごとに 1 回発火するが、再出現で再発火し得る。
        //   MX68KiOSViewModel.start() 自身が `didStart` で一度きりに落とし、
        //   さらに EmulatorEngine.start() も `guard !isRunning` を持つため、
        //   多重呼び出しは無害。BIOS 未設定で保留した場合の再試行は Apply 経由
        //   (MX68KiOSViewModel.applySettings)であり、ここではない。
        .task {
            viewModel.start(config: configManager.config)
        }
    }

    /// P708 §C-1 — 側方配置を選ぶ最小余白幅。
    /// 導出: Apple HIG の最小タップ領域 44pt + 既存の `.padding(6)` の左右分 6×2 = 56pt。
    /// これ未満なら従来の上下配置へフォールバックする(= 現行挙動と同一 / 回帰ではない)。
    private static let minSideBandWidth: CGFloat = 56.0

    /// P710 §記号表 — ソフトキーボードを**提示してよい**最小 container 幅(pt)。
    ///
    /// 一次情報源は無く、本サイクルが新規に定める規約である。導出: 許容帯 (440, 667]
    /// —— 下限 440 = 現行 iPhone 縦向きの最大幅(iPhone 16 Pro Max)、
    /// 上限 667 = 現行 iPhone 横向きの最小幅(iPhone SE 3rd、安全領域インセット 0)。
    /// 512 はその内側にあり、かつ `766 × 2/3 ≈ 510.67` に近い丸めた近似値なので
    /// 「キャンバスの約 2/3 以上が一度に見える」という機能的な意味も持つ。
    ///
    /// ★デバイス種別(`userInterfaceIdiom`)も向き(`orientation`)も使わない ——
    ///   P708 が確立した「向きではなく、実際に効く幾何量で判定する」方針の踏襲。
    ///   幅ゲート 1 本で iPad 全向き=提示 / iPhone 横=提示 / iPhone 縦=非提示 を満たし、
    ///   さらに iPad の Slide Over・1/3 split(幅 320〜375pt)という**デバイス種別では
    ///   表現できないコンテキスト**まで正しく非提示にできる。
    private static let softKeyboardMinWidth: CGFloat = 512.0
    /// `SoftKeyboardView` の固有幅: キャンバス 766 + `.padding(12)` の左右分 12×2。
    private static let softKeyboardIntrinsicWidth: CGFloat = 790.0
    /// 同・固有高: キャンバス 218 + `.padding(12)` の上下分 12×2。
    private static let softKeyboardIntrinsicHeight: CGFloat = 242.0

    /// P710 §C-5 — 提示可否の判定は**この純関数 1 本**に閉じる。
    /// 閾値リテラルを 2 箇所に書くと「片方だけ直す」事故が必ず起きる(CR-7)。
    static func softKeyboardAvailable(width: CGFloat) -> Bool {
        width >= softKeyboardMinWidth
    }

    /// P714 §変更内容 3/4 — 仮想パッドを提示してよいか。**オーバーレイの自動非表示と
    /// トグルボタンの無効化はこの 1 つの値を共有する**(条件を 2 箇所に書かない)。
    ///
    /// 計画の原文は `inputManager.gamepadControllers[0] == nil`
    /// (= 「port0(JOY1)に物理コントローラが割り当てられていないか」)である。
    /// 意味論はそのままだが、iOS ではその値が **恒真**になる:
    ///
    ///   `MX68K/App/Services/InputManager.swift` は `import AppKit` / `import Carbon`
    ///   を持つ macOS 専用ファイルで、iOS ターゲットの Sources phase に入っていない
    ///   (`ruby Scripts/add_ios_target.rb dump-sources MX68K-iOS` で確認)。
    ///   したがって iOS 版には GameController の配線そのものが存在せず、
    ///   **物理ゲームパッドから X68000 側へ入力が届く経路が 1 本も無い**。
    ///   port0 へ割り当てられたコントローラは常に存在しない = 常に `nil` 相当。
    ///
    /// ★ここで `GCController.controllers()` を見て自動非表示を実装しては **ならない**:
    ///   物理パッドを繋いだ瞬間に、実際には何も入力できない物理パッドのために
    ///   唯一機能している入力手段(仮想パッド)を消してしまう。計画が自動非表示に
    ///   期待していた「port0 の取り合いの回避」は、取り合う相手が存在しない iOS では
    ///   守るべき不変条件そのものが無い(§実装差分 D-1)。
    ///   iOS が物理ゲームパッドに対応した時点で、この 1 行を実際の port0 割当状態へ
    ///   差し替えれば、オーバーレイ側・ボタン側の双方が同時に追随する。
    static var virtualPadAvailable: Bool { true }

    /// macOS `ToolbarView`(上部)に対応する帯——Hard/Soft Reset・FDD0/FDD1・歯車。
    /// 描画を隠さないよう上端の細い帯だけを使う(P703以来の設計方針を継続)。
    /// ★歯車の右寄せは`IOSSettingsBarButtons`自身のHStack内Spacerが担うため、
    ///   ここでは単に全幅を与えるだけでよい(二重にSpacerを持たない)。
    /// P710 — `canShowKeyboard` を受け取るだけの関数化。帯のスタイル
    /// (`.padding(6)` / `.frame` / `.background`)は P708 から**不変**。
    /// P716 — `canShowVirtualPad`(幅ゲート)も受け取る。オーバーレイの表示条件と
    /// 「Pad」ボタンの有効/無効が**同じ値**を共有する(`canShowKeyboard` と同型)。
    private func topBand(canShowKeyboard: Bool, canShowVirtualPad: Bool) -> some View {
        IOSSettingsBarButtons(viewModel: viewModel,
                              activeSheet: $activeSheet,
                              importError: $importError,
                              showSoftKeyboard: $showSoftKeyboard,
                              canToggleSoftKeyboard: canShowKeyboard,
                              pendingFDDDrive: $pendingFDDDrive,
                              showVirtualPad: $showVirtualPad,
                              mouseModeEnabled: $mouseModeEnabled,
                              canToggleVirtualPad: Self.virtualPadAvailable && canShowVirtualPad)
            .padding(6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.black.opacity(0.6))
    }

    /// P708 — `topBand` の側方版。左のピラーボックス余白へ縦積みで置く。
    /// 帯のスタイル(`.padding(6)` / `.black.opacity(0.6)`)は上下配置と**共通**にする
    /// ——見た目の情報源を 2 つに割らない(§C-2)。
    /// P710 — `topBand` と同じく `canShowKeyboard` を受け取るだけの関数化。
    /// `axis: .vertical` を含め帯のスタイルは P708 から**不変**。
    /// P716 — `topBand` と同じく `canShowVirtualPad`(幅ゲート)も受け取る。
    private func sideButtonBand(canShowKeyboard: Bool, canShowVirtualPad: Bool) -> some View {
        IOSSettingsBarButtons(viewModel: viewModel,
                              activeSheet: $activeSheet,
                              importError: $importError,
                              axis: .vertical,
                              showSoftKeyboard: $showSoftKeyboard,
                              canToggleSoftKeyboard: canShowKeyboard,
                              pendingFDDDrive: $pendingFDDDrive,
                              showVirtualPad: $showVirtualPad,
                              mouseModeEnabled: $mouseModeEnabled,
                              canToggleVirtualPad: Self.virtualPadAvailable && canShowVirtualPad)
            .padding(6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.black.opacity(0.6))
    }

    /// 状態表示の中身。上下配置(`bottomBand`)と側方配置(`sideStatusBand`)で
    /// **同じものを共有**する —— 文言・書式を 2 箇所に複製しない。
    private var statusLines: some View {
        VStack(alignment: .leading, spacing: 2) {
            if let error = viewModel.metalError {
                Text(error)
                    .foregroundStyle(.red)
            }
            Text(viewModel.statusText)
                .foregroundStyle(.green)
            // P725 — セーブ/ロード結果(成功/失敗どちらも 1 行で表示)。
            // ★P727改訂——`.secondary`(セマンティック色)は、このファイル内の他の
            //   帯要素(`statusText`=`.green`・`importError`=`.red`、いずれも明示色)
            //   という既存パターンから外れており、固定の`.black.opacity(0.6)`背景
            //   (システムのライト/ダーク設定に追従しない)との組み合わせで低コントラスト
            //   になり視認できていなかったと判断(P726適用後もメッセージが一度も
            //   表示されないというユーザーの再確認、および`awaitStateOp completed`
            //   ログにより代入自体は確実に実行されていることの独立確認による)。
            //   `.green`(実行中)・`.red`(エラー)と区別できる明示色として`.yellow`を採用。
            if let transientMessage = viewModel.transientMessage {
                Text(transientMessage)
                    .foregroundStyle(.yellow)
            }
            if let importError {
                Text(importError)
                    .foregroundStyle(.red)
            }
        }
        .font(.system(size: 10, design: .monospaced))
    }

    /// macOS `StatusBarView`(下部)に対応する帯——状態表示テキスト
    /// (スクリーンショット判定用、P703以来)。
    private var bottomBand: some View {
        statusLines
            .padding(6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.black.opacity(0.6))
    }

    /// P708 §C-4 — `bottomBand` の側方版。右のピラーボックス余白へ置く。
    /// 幅が狭い(内寸 66pt 前後)ため、`statusText`(約 32 文字)は 1 行に収まらない。
    /// 半角スペース区切りで複数行へ折り返させ、最長トークン `PC=00FF0048` が
    /// それでも溢れる場合(残留リスク R-3)の保険として縮小も許す。
    private var sideStatusBand: some View {
        statusLines
            .lineLimit(nil)
            .fixedSize(horizontal: false, vertical: true)
            .minimumScaleFactor(0.7)
            .padding(6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.black.opacity(0.6))
    }

    /// P710改訂2(2026-08-30、ユーザーhands-on指摘・実測ログで確定) — 最初の改訂
    /// (縦方向`needsScroll`追加)は効果が無かった。実機ログ実測: `container=750x382`
    /// (iPhone横向き)は`softKeyboardIntrinsicHeight=242`より十分大きく
    /// (`needsVScroll=0`と正しく判定されていた)、にもかかわらず最下段(XF1-5・スペース等)
    /// が押せなかった。★真因はここ——`softKeyboardIntrinsicHeight`(218キャンバス+
    /// パディング24=242という**静的読解による見積もり**)自体が、`SoftKeyboardView`の
    /// 実際の描画高さより小さかった(実測していなかった)。
    ///
    /// 固定定数に賭けるのをやめ、`GeometryReader`+`PreferenceKey`で`SoftKeyboardView`の
    /// **実際の描画サイズ**を毎回測定し、`measuredKeyboardSize`(初期値は§記号表の
    /// 定数、実測後は実測値へ更新)を根拠に`needsScroll`を決める。初回トグル直後の
    /// 1フレームだけ旧定数のままになりうるが、SwiftUIのpreference伝搬は同一レイアウト
    /// パス内で解決されるため体感的な遅延は無い。
    ///
    /// ★`.scrollDisabled(!needsScroll)`はR-1(`SoftKeyButton`の
    ///   `DragGesture(minimumDistance: 0)`とスクロールのパンの競合)への一次防壁として
    ///   維持(実測サイズが収まっていればスクロールジェスチャそのものが無効化される)。
    private func softKeyboardBand(containerSize: CGSize) -> some View {
        let needsHScroll = containerSize.width < measuredKeyboardSize.width
        let needsVScroll = containerSize.height < measuredKeyboardSize.height
        let needsScroll = needsHScroll || needsVScroll
        let bandHeight = min(measuredKeyboardSize.height, containerSize.height)
        return ScrollView(needsScroll ? [.horizontal, .vertical] : [], showsIndicators: needsScroll) {
            SoftKeyboardView()
                .background(
                    GeometryReader { contentGeo in
                        Color.clear.preference(key: SoftKeyboardSizeKey.self, value: contentGeo.size)
                    }
                )
                // 収まる幅では中央寄せ、収まらない幅では固有幅のままスクロールさせる。
                .frame(minWidth: containerSize.width, alignment: .center)
        }
        .onPreferenceChange(SoftKeyboardSizeKey.self) { size in
            // ★ゼロサイズ(未計測/レイアウト前)は既定値のまま保持し、上書きしない。
            guard size.width > 0, size.height > 0 else { return }
            measuredKeyboardSize = size
        }
        .scrollDisabled(!needsScroll)
        .frame(height: bandHeight)
        // P720 — 帯全体へ 1 回だけ適用する(`SoftKeyboardView` 内部の個々のキーには
        // 掛けない)。既定 1.0 のため、スライダーを動かすまで見た目は現状のまま。
        .opacity(softKeyboardOpacity)
    }

    /// P708 自己反証可能性 —— 分岐結果 `branch=` という**派生値だけ**でなく、
    /// その計算の**生入力(container)・中間値(face)・閾値**を同じ 1 行に併記する。
    /// これにより「横向きなのに帯が上下に出た」という観測に対し、
    /// (a) container が想定と違う / (b) `face()` の結果が想定と違う /
    /// (c) 閾値で弾かれた / (d) そもそもログが出ていない(分岐コードに未到達)
    /// を 1 行で区別できる。
    private func logBandGeometry(container: CGSize) {
        let f = DisplayViewport.face(container: container)
        let aspect = container.height > 0 ? container.width / container.height : 0
        let branch = f.x >= Self.minSideBandWidth ? "side" : "stacked"
        let numbers = String(format:
            "container=%.1fx%.1f aspect=%.3f face=(x=%.1f y=%.1f w=%.1f h=%.1f) sideW=%.1f threshold=%.1f",
            container.width, container.height, aspect,
            f.x, f.y, f.w, f.h, f.x, Self.minSideBandWidth)
        mx68k_log("[Swift][iOS][P708-BAND] \(numbers) branch=\(branch)")

        // P710 自己反証可能性 —— P708 の既存行の書式は **1 文字も変えず**、
        // 同じ呼び出し点・同じ `container` から 2 行目として出す
        // (情報源を 2 つに割らない)。
        logSoftKeyboardGeometry(container: container, tag: "P710-SKBD")
    }

    /// P710 自己反証可能性 —— 幅ゲートの判定を出す 1 行。
    ///
    /// 「キーボードボタンが出ない」という観測に対しては
    /// (a) 幅がしきい値未満だった / (b) `geo` が想定と違う寸法を返した /
    /// (c) 分岐コードに到達していない / (d) 出ているが視認できていない
    /// の 4 通りの解釈があるため、**`gate=off` のときも必ず 1 行出す**(分母を出す設計)。
    /// 行が 1 本も無ければ (c) が確定し、行があって `width=` が想定外なら (b)、
    /// 想定どおりで `gate=off` なら (a) である。
    ///
    /// 派生値(`gate=` / `needsScroll=`)には、その計算元である生値
    /// (`container=` / `width=`)・判定に使った定数そのもの(`threshold=`)・
    /// 比較相手(`intrinsic=`)を**同じ行に併記**する —— 読者が自分で再計算して
    /// 照合できる。`shown=` は `@State` の生値。
    private func logSoftKeyboardGeometry(container: CGSize, tag: String) {
        let gate = Self.softKeyboardAvailable(width: container.width)
        // ★改訂(2026-08-30、iPhone横向き最下段クリップ修正): 縦方向のneedsScrollも
        //   横方向と同じ生値ベースでログへ出す(片方だけ計測して片方を見落とす事故を防ぐ)。
        let needsHScroll = container.width < Self.softKeyboardIntrinsicWidth
        let needsVScroll = container.height < Self.softKeyboardIntrinsicHeight
        let numbers = String(format:
            "container=%.1fx%.1f width=%.1f threshold=%.1f intrinsic=%.0fx%.0f",
            container.width, container.height, container.width,
            Self.softKeyboardMinWidth,
            Self.softKeyboardIntrinsicWidth, Self.softKeyboardIntrinsicHeight)
        mx68k_log("[Swift][iOS][\(tag)] \(numbers) gate=\(gate ? "on" : "off") "
                  + "needsHScroll=\(needsHScroll ? 1 : 0) needsVScroll=\(needsVScroll ? 1 : 0) "
                  + "shown=\(showSoftKeyboard ? 1 : 0)")
    }
}
