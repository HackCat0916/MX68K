import Foundation
import Carbon
import AppKit
import GameController
import CoreGraphics

/// P500: 多ボタンパッドのプロファイル。
/// 実機の X68000 は 8番ピン(PPI PortC bit4/5)のストローブ L/H による静的 2 バンク
/// 多重で多ボタン化しており(セレクタ 74HC157A)、L 側 = 方向+TRIG1/2(既存)、
/// H 側 = 追加ボタン。H 側のビット位置は XM6 `vm/ppi.cpp` の `JoyCpsfMd::MakeData`
/// (2547-2624)/ `JoyMagical::MakeData`(2650-2760)の実コード直読による。
/// 既定は `standard2Button` で、この場合 H 側へは一切書き込まない
/// (Bridge 側 `pad_btn1` は 0xFF 初期値のまま = P227/P499 の挙動と完全に同一)。
enum GamepadButtonProfile: String, CaseIterable, Identifiable {
    case standard2Button     // 標準 2 ボタン(既存動作・回帰なし)
    case cpsfMD6Button       // ストII 等(CPSF-PC + メガドライブ 6 ボタンパッド相当)
    case magicalPad4Button   // 餓狼伝説2 等(マジカルパッド相当)

    var id: String { rawValue }

    /// 設定画面の表示名(英語キー。日本語訳は Localizable.xcstrings)。
    /// `LocalizedStringKey` を返さないのは、このファイルが SwiftUI に依存しないため
    /// (View 側で `LocalizedStringKey(profile.displayNameKey)` として使う)。
    var displayNameKey: String {
        switch self {
        case .standard2Button:   return "Standard (2 buttons)"
        case .cpsfMD6Button:     return "CPSF-MD (6 buttons)"
        case .magicalPad4Button: return "Magical Pad (4 buttons)"
        }
    }

    /// H 側バンクの idle 値(プロファイル依存)。
    /// マジカルパッドのみ bit0/bit1 が常時 0 の固定シグネチャのため 0xFC
    /// (0xFF ではない — XM6 `JoyMagical::MakeData` の `data[1]` 初期値)。
    var bank1Idle: UInt8 {
        switch self {
        case .standard2Button, .cpsfMD6Button: return 0xFF
        case .magicalPad4Button:               return 0xFC
        }
    }
}

/// P585: リマップ可能なホスト側の物理ボタン(`GCExtendedGamepad` の押しボタン群)。
/// 方向入力(d-pad / 左スティック)は対象外 — 本機構が扱うのはボタンのみ。
/// 表示名の「L1/R1/L2/R2」はメーカー中立な一般的呼称であり、`GCExtendedGamepad` の
/// `leftShoulder`/`rightShoulder`/`leftTrigger`/`rightTrigger` に対応する。
/// A/B/X/Y 共々ラベル文字のため翻訳対象外(Localizable.xcstrings では扱わない)。
enum GamepadPhysicalButton: String, CaseIterable, Identifiable, Codable {
    case a, b, x, y, leftShoulder, rightShoulder, leftTrigger, rightTrigger

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .a: return "A"
        case .b: return "B"
        case .x: return "X"
        case .y: return "Y"
        case .leftShoulder:  return "L1"
        case .rightShoulder: return "R1"
        case .leftTrigger:   return "L2"
        case .rightTrigger:  return "R2"
        }
    }

    func isPressed(_ pad: GCExtendedGamepad) -> Bool {
        switch self {
        case .a: return pad.buttonA.isPressed
        case .b: return pad.buttonB.isPressed
        case .x: return pad.buttonX.isPressed
        case .y: return pad.buttonY.isPressed
        case .leftShoulder:  return pad.leftShoulder.isPressed
        case .rightShoulder: return pad.rightShoulder.isPressed
        case .leftTrigger:   return pad.leftTrigger.isPressed
        case .rightTrigger:  return pad.rightTrigger.isPressed
        }
    }
}

/// P585: X68000 側の論理機能(どのビットを駆動するか)。ビット位置自体は
/// `installGamepadHandler` 内の既存定数(XM6 `vm/ppi.cpp` 由来・P500 で確認済み)を
/// そのまま使い、本 enum は「どの物理ボタンがその機能を駆動するか」の識別子のみを担う。
enum GamepadFunction: String, CaseIterable, Identifiable {
    // 基礎バンク(全プロファイル共通)
    case trig1, trig2
    // cpsfMD6Button 追加バンク(bank1)
    case z, y6, x6, mode, c, start
    // magicalPad4Button 追加バンク(bank1)
    case l4, r4, b4

    var id: String { rawValue }

    /// `y6`/`x6` は Swift の enum case 名の衝突回避用の内部識別子であり、
    /// 表示名は実機どおりの "Y"/"X"。
    /// P587: 基礎バンクの 2 件は実機のボタン刻印に合わせて "A"/"B" 表記へ
    /// 変更(旧: "Trigger 1"/"Trigger 2" — ユーザーの hands-on 指摘)。
    /// 表示ラベルのみの変更で、`rawValue`(上書き辞書のキー)は無変更。
    var displayName: String {
        switch self {
        case .trig1: return "A"
        case .trig2: return "B"
        case .z:     return "Z"
        case .y6:    return "Y"
        case .x6:    return "X"
        case .mode:  return "MODE"
        case .c:     return "C"
        case .start: return "START"
        case .l4:    return "L"
        case .r4:    return "R"
        case .b4:    return "B"
        }
    }

    /// P593: この機能が駆動する bank1(ストローブ High 側)のビットマスク。
    /// 値は `installGamepadHandler` 内の既存定数(XM6 `vm/ppi.cpp` 由来・P500 で
    /// 確認済み)をそのまま転記したもので、新しいビット位置は導出していない。
    /// 基礎バンク(trig1/trig2)は bank0 側のため nil。
    /// 入力モニタ(`InputMonitorView`)の bank1 表示にのみ使う。
    var bank1Mask: UInt8? {
        switch self {
        case .trig1, .trig2: return nil
        case .z:     return 0x01
        case .y6:    return 0x02
        case .x6:    return 0x04
        case .mode:  return 0x08
        case .c:     return 0x20
        case .start: return 0x40
        case .l4:    return 0x04
        case .r4:    return 0x08
        case .b4:    return 0x40
        }
    }

    /// P586: 全プロファイル共通で常時有効な基礎バンク(`rows(for:)` が返す
    /// 一覧の先頭 2 件と一致)。リマップ画面で「基礎」と「追加ボタン」を
    /// Section で分けて見せるために使う(プロファイル名の数字は追加バンク側の
    /// ボタン数を指すため、そのままでは行数と一致せず分かりにくい)。
    static let baseRows: [GamepadFunction] = [.trig1, .trig2]

    /// P587: CPSF-MD(6 ボタン)の MODE/START は、メガドライブ 6 ボタンパッド実機の
    /// 「6 面ボタン」のカウントに含まれない別枠の特殊ボタン(Mode 切替・Start)である
    /// ため、「追加ボタン」Section とは別の「その他」Section へ分離して見せる。
    /// ユーザーの hands-on 指摘への対応(P587)。ビット位置・`rows(for:)` が返す
    /// 機能の集合は無変更で、GamepadRemapView 側の表示グルーピングにのみ使う。
    static func otherRows(for profile: GamepadButtonProfile) -> [GamepadFunction] {
        switch profile {
        case .cpsfMD6Button: return [.mode, .start]
        default:             return []
        }
    }

    /// このプロファイルで表示すべき機能一覧(基礎 2 + プロファイル別追加分)。
    /// マジカルパッドの D ボタンは含めない(XM6 側で B と同一ビットを操作しており
    /// ビット位置が未確定 — P500 でスコープ外とした判断を踏襲する。UI レイヤーの
    /// 本サイクルでは解決できない実機再現性の問題)。
    static func rows(for profile: GamepadButtonProfile) -> [GamepadFunction] {
        switch profile {
        case .standard2Button:   return [.trig1, .trig2]
        case .cpsfMD6Button:     return [.trig1, .trig2, .z, .y6, .x6, .mode, .c, .start]
        case .magicalPad4Button: return [.trig1, .trig2, .l4, .r4, .b4]
        }
    }

    /// 既定の割当先ボタン(P500 までのハードコード動作と完全一致 — 未設定時の
    /// フォールバック。`gamepadMap` が空なら常にこちらが使われるため回帰しない)。
    var defaultButton: GamepadPhysicalButton {
        switch self {
        case .trig1: return .a
        case .trig2: return .b
        case .z:     return .y
        case .y6:    return .x
        case .x6:    return .leftShoulder
        case .mode:  return .rightShoulder
        case .c:     return .leftTrigger
        case .start: return .rightTrigger
        case .l4:    return .leftShoulder
        case .r4:    return .rightShoulder
        case .b4:    return .leftTrigger
        }
    }
}

class InputManager: ObservableObject {
    // P194: 共有インスタンス。EmulatorView / EmulatorMetalView / InputSettingsView が
    // 同一インスタンスを参照する(EnvironmentObject 注入漏れによるクラッシュを原理的に排除)。
    static let shared = InputManager()

    @Published var keyboardState: [UInt32: Bool] = [:]
    @Published var mousePosition: CGPoint = .zero
    @Published var mouseButtonLeft: Bool = false
    @Published var mouseButtonRight: Bool = false
    @Published var joyState: [Int: UInt8] = [:]
    /// P593: 多ボタンプロファイル(4/6 ボタン)でゲストへ送出した bank1 の生バイト。
    /// 表示専用(入力モニタ)。標準 2 ボタンでは bank1 を一切送出しないため
    /// エントリが作られない = 「未送出」の判別に使う。
    @Published var joyState1: [Int: UInt8] = [:]

    private var keyMapping: [UInt16: UInt32] = [:]

    private var lastModifiers: NSEvent.ModifierFlags = []

    // P194: 実際に押下時に送出したスキャンコードを物理キー単位で記録する。
    // カーソルキーとテンキーが同じコードを出しうるため、キー解放は「押下時のコード」で行う。
    private var heldKeys: [UInt16: UInt32] = [:]

    // P194: 入力設定(ライブ適用)
    var arrowKeysAsNumpad = false
    var mouseEnabled      = true
    var mouseSensitivity  = 1.0

    // P511: ユーザーによる個別キー上書き(config.input.keyboardMap のライブ複製)。
    // キー = macOS 仮想キーコード(NSEvent.keyCode)の 10 進文字列、
    // 値 = 送出先の X68000 スキャンコード(0-255)の 10 進文字列。
    private var keyboardOverrides: [String: String] = [:]

    // P585: ゲームパッドのボタン割当上書き(config.input.gamepadMap のライブ複製)。
    // キー = "\(port):\(function.rawValue)"(例 "0:trig1")、
    // 値 = GamepadPhysicalButton.rawValue(例 "a")。未設定の機能は
    // `GamepadFunction.defaultButton`(= P500 までのハードコード値)へフォールバックする。
    // ★この辞書は valueChangedHandler クロージャ内から参照しない — ハンドラ登録時に
    //   解決済みの `GamepadPhysicalButton` を不変値としてキャプチャする(port/profile と同方針)。
    private var gamepadOverrides: [String: String] = [:]

    // P227: ゲームパッド(GameController framework)。負論理
    // (idle=0xFF・押下でビットをクリア)。bit0=Up bit1=Down bit2=Left
    // bit3=Right bit5=TRIG2(0x20) bit6=TRIG1(0x40)、bit4/bit7 は常時1。
    // 権威ソース: EmulatorBridge.h の mx68k_joy_set コメント / upstream joystick.h。
    // P499: port0(JOY1)/port1(JOY2)の 2 台に対応(Bridge/Core は元から 2 ポート対応)。
    var gamepadEnabled = false
    private var gamepadObservers: [NSObjectProtocol] = []

    // MARK: - P499/P499b: ゲームパッドのポート割当(2P 対応)

    // P499: コントローラ識別子 → 割当済み port(0/1)。新規接続時は空いている最小ポートを
    // 自動割当する(1 台だけ使う一般的なケースで設定画面を開かずに済ませるため)。
    // P499b: `GCController.controllers()` の配列順は OS 内部の管理順であり物理的な接続順
    // とは限らないため、自動割当の結果を設定画面の Picker から常に手動で上書きできる。
    // ★以下 3 つのプロパティは全てメインスレッドからのみ読み書きする(GCControllerDidConnect/
    // Disconnect 通知は queue: .main で登録済み、assignGamepadPort(_:toPort:) も SwiftUI の
    // Picker バインディング経由でメインスレッドから呼ばれる)。valueChangedHandler クロージャ
    // 内からは一切参照しない(スレッド安全性は port を不変値としてキャプチャすることで担保する)。
    private var gamepadPorts: [ObjectIdentifier: Int32] = [:]                 // controller 識別子 → port
    @Published private(set) var gamepadControllers: [Int32: GCController] = [:]  // port → 割当済み controller
    // P499b: 現在接続中で extendedGamepad プロファイルを持つ全コントローラ(設定画面の
    // Picker の選択肢。割当済みかどうかとは独立に、接続されていれば必ず列挙される)。
    @Published private(set) var connectedGamepads: [GCController] = []

    // P500: ポートごとの多ボタンパッドプロファイル(既定 = 標準 2 ボタン)。
    // ★このプロパティも上の 3 つと同様、メインスレッドからのみ読み書きする。
    // valueChangedHandler クロージャからは参照せず、ハンドラ登録時に不変値として
    // キャプチャする(port と同じ方針)。プロファイル変更時はハンドラを再登録する。
    @Published private(set) var gamepadProfiles: [Int32: GamepadButtonProfile] = [
        0: .standard2Button, 1: .standard2Button
    ]

    /// P500: 指定ポートのボタンプロファイルを切り替える(設定画面の Picker から呼ぶ)。
    /// 切替の瞬間に H 側バンクへプロファイル別の idle 値を 1 回送出し、直前の
    /// 押下状態が残らないようにする(`standard2Button` へ戻す場合は 0xFF =
    /// Bridge 側の初期値と同一に戻す)。
    func setGamepadProfile(_ profile: GamepadButtonProfile, forPort port: Int32) {
        guard port == 0 || port == 1 else { return }
        guard gamepadProfiles[port] != profile else { return }
        gamepadProfiles[port] = profile
        mx68k_joy_set1(port, profile.bank1Idle)
        // ハンドラは profile を不変キャプチャしているため、割当中のコントローラが
        // あれば再登録して新プロファイルを反映する。
        if let pad = gamepadControllers[port]?.extendedGamepad {
            installGamepadHandler(pad, port: port)
        }
    }

    /// P585: 指定ポート・機能に対する実効の物理ボタンを解決する。
    /// 上書きが無い / 値が不正な場合は既定(P500 までのハードコード値)へフォールバック。
    /// ★メインスレッドからのみ呼ぶ(呼出元は installGamepadHandler の登録処理)。
    private func resolvedButton(port: Int32, function: GamepadFunction) -> GamepadPhysicalButton {
        guard let raw = gamepadOverrides["\(port):\(function.rawValue)"],
              let button = GamepadPhysicalButton(rawValue: raw) else {
            return function.defaultButton
        }
        return button
    }

    /// P585: 指定ポート・機能の割当ボタンを変更する(リマップ画面から呼ぶ)。
    /// ハンドラは割当を不変キャプチャしているため、割当中のコントローラがあれば
    /// 再登録して即時反映する(`setGamepadProfile(_:forPort:)` と同型)。
    /// config.json への保存は View 側の責務(P511 の KeyRemapView と同じ分担)。
    func setGamepadButtonMapping(_ button: GamepadPhysicalButton, function: GamepadFunction, port: Int32) {
        guard port == 0 || port == 1 else { return }
        gamepadOverrides["\(port):\(function.rawValue)"] = button.rawValue
        if let pad = gamepadControllers[port]?.extendedGamepad {
            installGamepadHandler(pad, port: port)
        }
    }

    /// P499b: 表示名(vendorName が無いコントローラ向けのフォールバック付き)。
    func gamepadDisplayName(_ controller: GCController) -> String {
        controller.vendorName ?? "Controller"
    }

    /// P499b: 指定ポートへコントローラを明示的に割り当てる(設定画面の Picker から呼ぶ)。
    /// controller = nil は「未接続」の選択(そのポートの割当解除)。
    /// 「指定 port の既存割当を解除 → 指定 controller が別 port に割当済みならそちらも解除
    ///  → 指定 port へ割当」を 1 操作で行うため、Port1/Port2 を順に選び直すことで入れ替えも
    /// 表現できる。ハンドラは port を不変キャプチャしているので、割当変更は該当コントローラの
    /// ハンドラ再登録で反映する(メインスレッドからの書込)。
    func assignGamepadPort(_ controller: GCController?, toPort port: Int32) {
        guard port == 0 || port == 1 else { return }
        // 0. 割当先が指定されている場合は、先に extendedGamepad を取得できるか確認する
        //    (取得できないコントローラを選んだために既存の割当まで失われるのを防ぐ)。
        let pad = controller?.extendedGamepad
        if controller != nil && pad == nil { return }
        // 1. 指定ポートの既存割当を解除(押下中の入力が残らないよう idle 送出込み)
        clearGamepadPort(port)
        guard let controller = controller, let pad = pad else { return }   // nil = 「未接続」を選択
        // 2. 指定コントローラが別ポートに割当済みなら、そちらも解除(同一機の二重割当を防ぐ)
        if let previous = gamepadPorts[ObjectIdentifier(controller)] {
            clearGamepadPort(previous)
        }
        // 3. 指定ポートへ割当
        gamepadPorts[ObjectIdentifier(controller)] = port
        gamepadControllers[port] = controller
        installGamepadHandler(pad, port: port)
    }

    /// 指定ポートの割当を解除し、そのポートのみ idle 化する
    /// (もう一方のポートの入力状態は巻き添えにしない)。割当が無ければ何もしない。
    private func clearGamepadPort(_ port: Int32) {
        guard let controller = gamepadControllers[port] else { return }
        gamepadPorts.removeValue(forKey: ObjectIdentifier(controller))
        gamepadControllers[port] = nil
        controller.extendedGamepad?.valueChangedHandler = nil
        mx68k_joy_set(port, 0xFF)
        idleGamepadBank1(port)   // P500: H 側バンクも押しっぱなしを残さない
    }

    /// P500: 指定ポートの H 側バンクを idle 化する。標準 2 ボタンプロファイルでは
    /// 何もしない(pad_btn1 へ一度も書かない = 既存動作の完全維持)。
    private func idleGamepadBank1(_ port: Int32) {
        guard let profile = gamepadProfiles[port], profile != .standard2Button else { return }
        mx68k_joy_set1(port, profile.bank1Idle)
    }

    /// 空いている最小ポート(0 → 1)を自動割当する。既に割当済みならそのポートを返す。
    /// 2 台を超える場合は nil(実機のポート数に合わせ、サイレントに無視する)。
    private func autoAssignGamepadPort(for controller: GCController) -> Int32? {
        let id = ObjectIdentifier(controller)
        if let existing = gamepadPorts[id] { return existing }
        for candidate: Int32 in [0, 1] where gamepadControllers[candidate] == nil {
            gamepadPorts[id] = candidate
            gamepadControllers[candidate] = controller
            return candidate
        }
        return nil
    }

    /// 切断されたコントローラのポートを解放する(割当が無い場合は何もしない)。
    private func releaseGamepadPort(for controller: GCController) {
        if let port = gamepadPorts[ObjectIdentifier(controller)] { clearGamepadPort(port) }
    }

    // P194: マウス感度適用後の端数の持ち越し(1.0 未満の感度が無効化されないように)
    private var accX = 0.0
    private var accY = 0.0

    // P196: 絶対座標追従(ホストカーソルの「位置」を目標にゲストポインタを追わせる)。
    // ホスト側のポインタ加速は無関係(位置しか見ない)ため乖離しない。false で従来の相対方式。
    @Published var mouseAbsolute = true
    private var guestX: Double = 0     // ゲストポインタの推定位置(framebuffer px)
    private var guestY: Double = 0
    private var needsHoming = true     // 次のマウス移動時に原点合わせ(homing)する

    /// P196: 原点合わせを要求(モード切替 / 再有効化 / 起動 / reset / fb サイズ変化)。
    /// ホストカーソル位置には依存しない。
    func requestMouseHoming() { needsHoming = true }

    /// P196: ゲストポインタを (0,0) に張り付ける。ゲスト(IOCS)は画面境界で
    /// ポインタをクランプするので、十分大きな負方向移動を送れば必ず原点に着く。
    /// Bridge が残余を持ち越すため確実に届く。ホストカーソル位置に依存しない。
    private func homeGuestPointer(_ fb: CGSize) {
        let bx = Int32(clamping: Int(-(fb.width + 64)))
        let by = Int32(clamping: Int(-(fb.height + 64)))
        mx68k_mouse_move(bx, by)
        guestX = 0; guestY = 0
        mousePosition = CGPoint(x: guestX, y: guestY)   // P551: 入力モニタ表示用(呼び出し元は main thread)
        needsHoming = false
    }

    // MARK: - P237: マウスキャプチャ(ホストカーソルを隠して無制限相対移動)

    /// ⌥⌘M から呼ぶトグル。true 化でカーソル位置とマウス移動を切り離し
    /// (画面境界にホストカーソルが張り付いて delta が出なくなるのを回避)
    /// カーソルを隠す。false 化で拘束を解き、絶対モード復帰時のズレを防ぐため
    /// homing を要求する。戻り値は反転後の状態。
    @discardableResult
    func toggleMouseCapture() -> Bool {
        if MouseCaptureState.shared.isCaptured {
            releaseMouseCapture()
        } else {
            MouseCaptureState.shared.isCaptured = true
            CGAssociateMouseAndMouseCursorPosition(0)
            NSCursor.hide()
        }
        return MouseCaptureState.shared.isCaptured
    }

    /// 安全弁(フォーカス喪失 / アプリ非アクティブ / マウス無効化 / 停止)から呼ぶ。
    /// キャプチャ中のときだけ解除する冪等な入口。
    func forceReleaseMouseCaptureIfNeeded() {
        guard MouseCaptureState.shared.isCaptured else { return }
        releaseMouseCapture()
    }

    /// CGAssociateMouseAndMouseCursorPosition(0) はシステム全体に効くため、
    /// 解除漏れは実 OS カーソルが画面全体で不可視・固着する重大な失敗になる。
    /// 解除処理は 1 箇所に集約する。
    private func releaseMouseCapture() {
        MouseCaptureState.shared.isCaptured = false
        CGAssociateMouseAndMouseCursorPosition(1)
        NSCursor.unhide()
        needsHoming = true   // 絶対モード復帰時にゲストポインタ原点を合わせ直す
    }

    /// P194: 設定を反映(ライブ適用・リセット不要)。起動時も設定 UI からもこれ 1 本を呼ぶ。
    func applyConfig(_ input: InputConfig) {
        arrowKeysAsNumpad = input.arrowKeysAsNumpad
        // P511: 末尾の setupKeyMapping() が上書きを適用するため、その前に取り込む。
        keyboardOverrides = input.keyboardMap
        // P585: ゲームパッドのボタン割当上書きも同じタイミングで取り込む。
        gamepadOverrides = input.gamepadMap
        let wasEnabled  = mouseEnabled
        let wasAbsolute = mouseAbsolute
        mouseEnabled      = input.mouseEnabled
        mouseSensitivity  = min(max(input.mouseSensitivity, 0.25), 3.0)
        mouseAbsolute     = input.mouseAbsolute
        // マウス無効化時にボタンが押しっぱなしになるのを防ぐ
        if !mouseEnabled {
            if mouseButtonLeft  { mouseButtonLeft  = false; mx68k_mouse_button(0, false) }
            if mouseButtonRight { mouseButtonRight = false; mx68k_mouse_button(1, false) }
            accX = 0; accY = 0
            forceReleaseMouseCaptureIfNeeded()   // P237: マウス自体を無効化したらキャプチャも解除
        }
        // P196: 絶対モードへの切替 / マウス再有効化 で原点合わせを要求
        // (ホストカーソル位置での初期化は逆効果 — homing で (0,0) に合わせる)。
        if mouseEnabled && mouseAbsolute && (!wasAbsolute || !wasEnabled) {
            needsHoming = true
        }
        // P227: ゲームパッドを true→false に無効化した瞬間、押しっぱなしの入力が
        // ゲスト側に残留しないよう idle(0xFF)を送る(上の mouseEnabled 解放と同じ考え方)。
        let wasGamepad = gamepadEnabled
        gamepadEnabled = input.gamepadEnabled
        if wasGamepad && !gamepadEnabled {
            mx68k_joy_set(0, 0xFF)
            mx68k_joy_set(1, 0xFF)   // P499: port1(2P)も同時に idle 化
            idleGamepadBank1(0)      // P500: H 側バンク(多ボタンプロファイル時のみ)
            idleGamepadBank1(1)
        }
        setupKeyMapping()   // 内部で保持中キーを解放する
    }

    func setupKeyMapping() {
        // P194: 割当を変更する前に保持中のキーを全て解放する
        // (変更後は別のスキャンコードになり、古いコードが押されたままになるため)。
        for (_, mapped) in heldKeys { mx68k_key_up(UInt8(mapped)) }
        heldKeys.removeAll()
        keyboardState.removeAll()

        keyMapping = [
            // 数字・記号 行 (row1)
            UInt16(kVK_ANSI_1): 0x02, UInt16(kVK_ANSI_2): 0x03, UInt16(kVK_ANSI_3): 0x04,
            UInt16(kVK_ANSI_4): 0x05, UInt16(kVK_ANSI_5): 0x06, UInt16(kVK_ANSI_6): 0x07,
            UInt16(kVK_ANSI_7): 0x08, UInt16(kVK_ANSI_8): 0x09, UInt16(kVK_ANSI_9): 0x0A,
            UInt16(kVK_ANSI_0): 0x0B,
            UInt16(kVK_ANSI_Minus): 0x0C,        // '-'
            UInt16(kVK_ANSI_Equal): 0x0D,        // JIS 凡例 '^'
            UInt16(kVK_JIS_Yen): 0x0E,           // '¥'
            UInt16(kVK_Delete): 0x0F,            // macOS Backspace -> X68000 BS
            // QWERTY 行 (row2)
            UInt16(kVK_Tab): 0x10,
            UInt16(kVK_ANSI_Q): 0x11, UInt16(kVK_ANSI_W): 0x12, UInt16(kVK_ANSI_E): 0x13,
            UInt16(kVK_ANSI_R): 0x14, UInt16(kVK_ANSI_T): 0x15, UInt16(kVK_ANSI_Y): 0x16,
            UInt16(kVK_ANSI_U): 0x17, UInt16(kVK_ANSI_I): 0x18, UInt16(kVK_ANSI_O): 0x19,
            UInt16(kVK_ANSI_P): 0x1A,
            UInt16(kVK_ANSI_LeftBracket): 0x1B,  // JIS '@'
            UInt16(kVK_ANSI_RightBracket): 0x1C, // JIS '['
            UInt16(kVK_Return): 0x1D,            // CR
            // ASDF 行 (row3)
            UInt16(kVK_ANSI_A): 0x1E, UInt16(kVK_ANSI_S): 0x1F, UInt16(kVK_ANSI_D): 0x20,
            UInt16(kVK_ANSI_F): 0x21, UInt16(kVK_ANSI_G): 0x22, UInt16(kVK_ANSI_H): 0x23,
            UInt16(kVK_ANSI_J): 0x24, UInt16(kVK_ANSI_K): 0x25, UInt16(kVK_ANSI_L): 0x26,
            UInt16(kVK_ANSI_Semicolon): 0x27,    // ';'
            UInt16(kVK_ANSI_Quote): 0x28,        // JIS ':'
            UInt16(kVK_ANSI_Backslash): 0x29,    // JIS ']'
            // ZXCV 行 (row4)
            UInt16(kVK_ANSI_Z): 0x2A, UInt16(kVK_ANSI_X): 0x2B, UInt16(kVK_ANSI_C): 0x2C,
            UInt16(kVK_ANSI_V): 0x2D, UInt16(kVK_ANSI_B): 0x2E, UInt16(kVK_ANSI_N): 0x2F,
            UInt16(kVK_ANSI_M): 0x30,
            UInt16(kVK_ANSI_Comma): 0x31, UInt16(kVK_ANSI_Period): 0x32, UInt16(kVK_ANSI_Slash): 0x33,
            UInt16(kVK_JIS_Underscore): 0x34,    // '_'
            UInt16(kVK_Space): 0x35,
            // 編集・制御
            UInt16(kVK_Home): 0x36,              // HOME
            UInt16(kVK_ForwardDelete): 0x37,     // DEL
            UInt16(kVK_PageUp): 0x38,            // ROLL UP
            UInt16(kVK_PageDown): 0x39,          // ROLL DOWN
            UInt16(kVK_End): 0x3A,               // UNDO
            UInt16(kVK_Escape): 0x01,
            // 矢印（訂正済: ←0x3B →0x3C ↑0x3D ↓0x3E）
            UInt16(kVK_LeftArrow): 0x3B, UInt16(kVK_RightArrow): 0x3D,
            UInt16(kVK_UpArrow): 0x3C, UInt16(kVK_DownArrow): 0x3E,
            // テンキー
            UInt16(kVK_ANSI_KeypadClear): 0x3F,  // CLR
            UInt16(kVK_ANSI_KeypadDivide): 0x40, UInt16(kVK_ANSI_KeypadMultiply): 0x41,
            UInt16(kVK_ANSI_KeypadMinus): 0x42,
            UInt16(kVK_ANSI_Keypad7): 0x43, UInt16(kVK_ANSI_Keypad8): 0x44, UInt16(kVK_ANSI_Keypad9): 0x45,
            UInt16(kVK_ANSI_KeypadPlus): 0x46,
            UInt16(kVK_ANSI_Keypad4): 0x47, UInt16(kVK_ANSI_Keypad5): 0x48, UInt16(kVK_ANSI_Keypad6): 0x49,
            UInt16(kVK_ANSI_KeypadEquals): 0x4A,
            UInt16(kVK_ANSI_Keypad1): 0x4B, UInt16(kVK_ANSI_Keypad2): 0x4C, UInt16(kVK_ANSI_Keypad3): 0x4D,
            UInt16(kVK_ANSI_KeypadEnter): 0x4E,
            UInt16(kVK_ANSI_Keypad0): 0x4F,
            UInt16(kVK_JIS_KeypadComma): 0x50,   // テンキー ','
            UInt16(kVK_ANSI_KeypadDecimal): 0x51,// テンキー '.'
            // ファンクション・特殊
            UInt16(kVK_F1): 0x63, UInt16(kVK_F2): 0x64, UInt16(kVK_F3): 0x65,
            UInt16(kVK_F4): 0x66, UInt16(kVK_F5): 0x67,
            UInt16(kVK_F6): 0x55, UInt16(kVK_F7): 0x56, UInt16(kVK_F8): 0x57,   // XF1-XF3
            UInt16(kVK_F9): 0x58, UInt16(kVK_F10): 0x59,                        // XF4-XF5
            UInt16(kVK_F11): 0x72,               // OPT.1
            UInt16(kVK_F12): 0x73,               // OPT.2
            UInt16(kVK_F13): 0x54,               // HELP
            UInt16(kVK_F14): 0x62,               // COPY
            UInt16(kVK_F15): 0x61,               // BREAK
            UInt16(kVK_Help): 0x5E,              // INS (Mac Insert/Help 位置)
            // 日本語入力キー
            UInt16(kVK_JIS_Kana): 0x5A,          // かな
            UInt16(kVK_JIS_Eisu): 0x5B,          // ローマ字(英数)
        ]

        // P194: カーソルキーをテンキー(8=上 / 2=下 / 4=左 / 6=右)として送る。
        // テンキー自身の割当は据え置き(両方から同じスキャンコードが出るため
        // heldKeys による押下コードの記録が必要)。
        if arrowKeysAsNumpad {
            keyMapping[UInt16(kVK_UpArrow)]    = 0x44   // テンキー 8
            keyMapping[UInt16(kVK_DownArrow)]  = 0x4C   // テンキー 2
            keyMapping[UInt16(kVK_LeftArrow)]  = 0x47   // テンキー 4
            keyMapping[UInt16(kVK_RightArrow)] = 0x49   // テンキー 6
        }

        // P511: ユーザーによる個別キー上書き(config.input.keyboardMap)を最後に適用。
        // 上書きはホストキー単位で keyMapping を直接書き換えるだけで、置き換えられた
        // 側の「元のホストキー」の割当は解除しない(複数ホストキーが同じ X68000 キーへ
        // 送出できる状態を許容する — undo/差し替え管理を持たない意図的な最小スコープ)。
        // arrowKeysAsNumpad の調整より後に適用するため、明示的な上書きが常に最終的に勝つ。
        for (hostKeyStr, scancodeStr) in keyboardOverrides {
            guard let hostKey = UInt16(hostKeyStr), let scancode = UInt8(scancodeStr) else { continue }
            keyMapping[hostKey] = UInt32(scancode)
        }
    }

    /// P511: 指定の X68000 スキャンコードへ現在実際に送出しているホストキーコードを返す
    /// (デフォルト割当・ユーザー上書きの両方を区別せず、有効な結果を返す)。
    /// 複数のホストキーが同じスキャンコードへ割り当てられている場合はどれか 1 件を返す
    /// (表示専用・決定論的な優先順位は持たせない)。`keyMapping` が private のため、
    /// 設定 UI 向けの読み取り専用の橋渡しとして用意している。
    func currentHostKeyCode(forScancode scancode: UInt8) -> UInt16? {
        keyMapping.first(where: { $0.value == UInt32(scancode) })?.key
    }

    func handleKeyDown(_ event: NSEvent) {
        let raw = UInt16(event.keyCode)
        guard let mapped = keyMapping[raw], mapped <= 0xFF else { return }
        heldKeys[raw] = mapped          // P194: 送出したコードを物理キー単位で記録
        keyboardState[mapped] = true
        mx68k_key_down(UInt8(mapped))
    }

    func handleKeyUp(_ event: NSEvent) {
        let raw = UInt16(event.keyCode)
        // P194: 押下時に送出したコードで解放する(現在の割当を引き直さない)。
        guard let mapped = heldKeys.removeValue(forKey: raw) else { return }
        // 同じコードを別の物理キーがまだ保持している場合は解放しない。
        guard !heldKeys.values.contains(mapped) else { return }
        keyboardState[mapped] = false
        mx68k_key_up(UInt8(mapped))
    }

    func handleFlagsChanged(_ event: NSEvent) {
        let now = event.modifierFlags
        let pairs: [(NSEvent.ModifierFlags, UInt8)] = [
            (.shift,   0x70),   // SHIFT
            (.control, 0x71),   // CTRL
        ]
        for (flag, code) in pairs {
            let wasDown = lastModifiers.contains(flag)
            let isDown  = now.contains(flag)
            if isDown && !wasDown { mx68k_key_down(code) }
            else if !isDown && wasDown { mx68k_key_up(code) }
        }
        // CAPSLOCK: toggle -> down+up pulse on any change
        if now.contains(.capsLock) != lastModifiers.contains(.capsLock) {
            mx68k_key_down(0x5D)
            mx68k_key_up(0x5D)
        }
        lastModifiers = now
        // COMMAND (Cmd) intentionally not mapped (preserve macOS shortcuts)
    }

    // P196: 絶対座標追従。ホストカーソルの Metal ビュー内座標を framebuffer 座標へ
    // 写像し、ゲストポインタ推定位置との差分を送る。ホスト側の加速は無関係(位置のみ)。
    /// P595 (D-55): `geom` は Bridge が公開する表示ジオメトリ(`EmulatorViewModel.
    /// displayGeometry` と同一値をメインスレッドで受け取ったもの)。標準ラスタでは
    /// 恒等なので従来と完全に同じ写像になる。
    func handleMouseMoved(_ event: NSEvent, viewPoint p: CGPoint, viewSize: CGSize,
                          framebuffer fb: CGSize, geom: DisplayGeometry = .identity) {
        guard mouseEnabled else { return }
        // P237: キャプチャ中は絶対/相対設定に関わらず常に相対移動(ホスト
        // カーソル位置に依存しない無制限移動)。
        if MouseCaptureState.shared.isCaptured { return relativeMove(event) }
        guard mouseAbsolute else { return relativeMove(event) }   // 従来方式(感度スライダ)
        guard fb.width > 0, fb.height > 0, viewSize.width > 0, viewSize.height > 0 else { return }
        guard p.x.isFinite, p.y.isFinite else { return }          // ★S5: NaN/inf ガード

        // ★M3: 原点合わせ(ホストカーソル位置に依存しない)。
        if needsHoming { homeGuestPointer(fb) }

        // P212: draw() と同一の 4:3 viewport を共有し、2軸独立スケールで framebuffer
        // 座標へ写像する。新 draw() は tw:th ≠ 4:3 で非一様ストレッチゆえ、単一スケール
        // s=min(...) では横方向がズレる(P196 回帰)。viewSize(pt)は drawable(px)と
        // backingScale の比例関係なので、同じ式が幾何的に一致する。
        // P595 (D-55): draw() は上基準の fitMetal、こちらは AppKit の下基準座標系なので
        // fitAppKit を使う(同じ矩形を y 反転して表す)。標準ラスタでは geom は恒等。
        let vp = DisplayViewport.fitAppKit(container: viewSize, geom: geom)
        let scaleX = vp.w / fb.width
        let scaleY = vp.h / fb.height
        guard scaleX > 0, scaleY > 0, scaleX.isFinite, scaleY.isFinite else { return }
        let fx = min(max((p.x - vp.x) / scaleX, 0), fb.width)
        let fy = min(max(fb.height - (p.y - vp.y) / scaleY, 0), fb.height)   // AppKit は左下原点 → 反転

        // ★M1: Bridge が残余を持ち越すので ±127 クランプ不要 — 全量を送りモデルを目標に一致させる。
        var sx = Double(fx) - guestX
        var sy = Double(fy) - guestY

        // ★M2: 端に張り付いたら余分に押し込んで再同期(ゲストは画面境界でクランプする)。
        var anchoredX = false, anchoredY = false
        if fx <= 0                  { sx = -(guestX) - 64;                 anchoredX = true }
        else if fx >= fb.width  - 1 { sx = (fb.width  - 1 - guestX) + 64;  anchoredX = true }
        if fy <= 0                  { sy = -(guestY) - 64;                 anchoredY = true }
        else if fy >= fb.height - 1 { sy = (fb.height - 1 - guestY) + 64;  anchoredY = true }

        // ★S3: 感度スライダは絶対モードでも「ポインタ追従の補正(既定 1.0)」として有効。
        // 送出とモデルの前進に同じ係数を掛ける(k=1 のとき target に一致)。
        // 端の押し込みには掛けない(端へ確実に到達させるため)。
        let sentX = anchoredX ? sx : sx * mouseSensitivity
        let sentY = anchoredY ? sy : sy * mouseSensitivity

        guard sentX != 0 || sentY != 0 else { return }
        guard sentX.isFinite, sentY.isFinite else { return }
        let ix = Int32(clamping: Int(sentX.rounded(FloatingPointRoundingRule.towardZero)))     // ★S5
        let iy = Int32(clamping: Int(sentY.rounded(FloatingPointRoundingRule.towardZero)))
        mx68k_mouse_move(ix, iy)

        // モデル前進: 端では観測値を断定、それ以外は送出分だけ進める(k=1 で target に一致)。
        guestX = anchoredX ? (fx <= 0 ? 0 : fb.width  - 1) : guestX + sentX
        guestY = anchoredY ? (fy <= 0 ? 0 : fb.height - 1) : guestY + sentY
        mousePosition = CGPoint(x: guestX, y: guestY)   // P551: 入力モニタ表示用(NSEvent 経路 = main thread)
    }

    /// P196: 従来の相対方式(感度スライダ + 端数持ち越し)。モード「相対」で使う。
    private func relativeMove(_ event: NSEvent) {
        // P194: 感度を適用し、端数を次イベントへ持ち越す。
        // (非ゼロ delta に ±1 を強制すると 1.0 未満の感度が実質無効になるため)
        accX += event.deltaX * mouseSensitivity
        accY += event.deltaY * mouseSensitivity
        let dx = accX.rounded(.towardZero)
        let dy = accY.rounded(.towardZero)
        accX -= dx
        accY -= dy
        guard dx != 0 || dy != 0 else { return }
        // P195: ±127 クランプは Bridge 側(Mouse_SetData)がパケット単位で行う。
        // ここで 1 イベント毎にクランプすると、素早い動きの移動量が失われる。
        // ここでは Int32 変換のトラップ回避のガードのみ。
        mx68k_mouse_move(Int32(min(max(dx, -10000), 10000)),
                         Int32(min(max(dy, -10000), 10000)))
    }

    func handleMouseDown(button: Int) {
        guard mouseEnabled else { return }
        if button == 0 { mouseButtonLeft = true }
        if button == 1 { mouseButtonRight = true }
        mx68k_mouse_button(Int32(button), true)
    }

    func handleMouseUp(button: Int) {
        guard mouseEnabled else { return }
        if button == 0 { mouseButtonLeft = false }
        if button == 1 { mouseButtonRight = false }
        mx68k_mouse_button(Int32(button), false)
    }

    // MARK: - P227: ゲームパッド(GameController framework・P499 で port0/port1 の 2 台対応)

    /// EmulatorView の .onAppear から呼ぶ。接続済みコントローラにハンドラを登録し、
    /// 以後の抜き差しを NotificationCenter で追従する。ポーリングでなくイベント駆動
    /// (valueChangedHandler)ゆえフレームループとは独立。
    func startGamepadMonitoring() {
        // 既に接続済みのコントローラにハンドラを登録
        for controller in GCController.controllers() {
            registerGamepadHandler(controller)
        }
        // 以後の抜き差しに追従(observer トークンを保持し stop で解除する)
        let nc = NotificationCenter.default
        let connectToken = nc.addObserver(
            forName: .GCControllerDidConnect, object: nil, queue: .main
        ) { [weak self] note in
            guard let controller = note.object as? GCController else { return }
            self?.registerGamepadHandler(controller)
        }
        let disconnectToken = nc.addObserver(
            forName: .GCControllerDidDisconnect, object: nil, queue: .main
        ) { [weak self] note in
            guard let controller = note.object as? GCController else { return }
            // ハンドラは自然に発火しなくなる。方向/ボタン押下中に抜かれた場合の
            // 押しっぱなし残留を避けるため、P499 では「抜かれた側のポートだけ」idle
            // にする(もう一方のプレイヤーの入力状態を巻き添えにしない)。
            self?.releaseGamepadPort(for: controller)
            // P499b: 設定画面の選択肢からも取り除く(オブジェクト同一性で除去し、
            // GCController.controllers() の更新タイミングに依存しない)。
            self?.connectedGamepads.removeAll { $0 === controller }
        }
        gamepadObservers = [connectToken, disconnectToken]
    }

    /// EmulatorView の .onDisappear から呼ぶ。observer とハンドラを解除し、
    /// スティック/ボタンが押しっぱなしのまま残らないよう idle を最後に1回送る。
    func stopGamepadMonitoring() {
        let nc = NotificationCenter.default
        for token in gamepadObservers { nc.removeObserver(token) }
        gamepadObservers.removeAll()
        for controller in GCController.controllers() {
            controller.extendedGamepad?.valueChangedHandler = nil
        }
        mx68k_joy_set(0, 0xFF)   // idle(押しっぱなし解放)
        mx68k_joy_set(1, 0xFF)   // P499: port1(2P)も idle 化
        idleGamepadBank1(0)      // P500: H 側バンク(多ボタンプロファイル時のみ)
        idleGamepadBank1(1)
        gamepadPorts.removeAll()
        gamepadControllers.removeAll()
        connectedGamepads.removeAll()
    }

    /// 1 台のコントローラの extendedGamepad に入力変化ハンドラを登録する。
    /// extendedGamepad プロファイルを持たないコントローラはスキップ。
    /// P499b: 接続中一覧(設定画面 Picker の選択肢)には、ポートを割り当てられない
    /// 3 台目以降も含める(手動でポートへ差し替えられるようにするため)。
    /// P499: 自動割当できるポート(0/1 の空き)が無い場合はハンドラを登録しない。
    private func registerGamepadHandler(_ controller: GCController) {
        guard let pad = controller.extendedGamepad else { return }
        if !connectedGamepads.contains(where: { $0 === controller }) {
            connectedGamepads.append(controller)
        }
        guard let port = autoAssignGamepadPort(for: controller) else { return }
        installGamepadHandler(pad, port: port)
    }

    /// P499: 指定ポート向けの入力変化ハンドラを登録する。port は不変値として
    /// クロージャにキャプチャされる(ポート割当辞書をクロージャ内から参照しては
    /// ならない — valueChangedHandler は任意スレッドで発火しうるため)。
    private func installGamepadHandler(_ pad: GCExtendedGamepad, port: Int32) {
        // P500: プロファイルも port と同じく「登録時点の不変値」としてキャプチャする
        // (クロージャ内から @Published の辞書を触らない — 任意スレッド発火のため)。
        // 変更時は setGamepadProfile(_:forPort:) がハンドラを再登録する。
        let profile = gamepadProfiles[port] ?? .standard2Button
        // P585: ボタン割当も profile と同じく登録時点で解決し、不変値としてキャプチャする
        // (クロージャ内から gamepadOverrides を参照しない)。割当変更時は
        // setGamepadButtonMapping(_:function:port:) がハンドラを再登録する。
        // ★ここで決まるのは「どの物理ボタンの押下を読むか」だけで、
        //   下のビット位置(0x40/0x20/0x01/0x02/0x04/0x08/0x20/0x40)は無変更。
        let trig1Button = resolvedButton(port: port, function: .trig1)
        let trig2Button = resolvedButton(port: port, function: .trig2)
        // cpsfMD6Button 用(bank1)
        let zButton     = resolvedButton(port: port, function: .z)
        let y6Button    = resolvedButton(port: port, function: .y6)
        let x6Button    = resolvedButton(port: port, function: .x6)
        let modeButton  = resolvedButton(port: port, function: .mode)
        let cButton     = resolvedButton(port: port, function: .c)
        let startButton = resolvedButton(port: port, function: .start)
        // magicalPad4Button 用(bank1)
        let l4Button    = resolvedButton(port: port, function: .l4)
        let r4Button    = resolvedButton(port: port, function: .r4)
        let b4Button    = resolvedButton(port: port, function: .b4)
        pad.valueChangedHandler = { [weak self] gamepad, _ in
            guard let self = self, self.gamepadEnabled else { return }
            // 負論理: idle=0xFF を起点に、押下されたビットをクリアする。
            var byte: UInt8 = 0xFF
            let dz: Float = 0.5   // 左スティックのデッドゾーン閾値
            // d-pad と左スティックを OR 合成(どちらを倒しても方向入力として有効)。
            let up    = gamepad.dpad.up.isPressed    || gamepad.leftThumbstick.yAxis.value >  dz
            let down  = gamepad.dpad.down.isPressed  || gamepad.leftThumbstick.yAxis.value < -dz
            let left  = gamepad.dpad.left.isPressed  || gamepad.leftThumbstick.xAxis.value < -dz
            let right = gamepad.dpad.right.isPressed || gamepad.leftThumbstick.xAxis.value >  dz
            if up    { byte &= ~UInt8(0x01) }   // bit0 = Up
            if down  { byte &= ~UInt8(0x02) }   // bit1 = Down
            if left  { byte &= ~UInt8(0x04) }   // bit2 = Left
            if right { byte &= ~UInt8(0x08) }   // bit3 = Right
            if trig1Button.isPressed(gamepad) { byte &= ~UInt8(0x40) }   // bit6 = TRIG1
            if trig2Button.isPressed(gamepad) { byte &= ~UInt8(0x20) }   // bit5 = TRIG2
            // GamePad_SetState(呼出先)はアトミック実装済み → メインスレッドへの
            // ディスパッチ不要。valueChangedHandler は任意スレッドで呼ばれてよい。
            // P499: 送出先はハンドラ登録時にキャプチャした不変の port(0=JOY1 / 1=JOY2)。
            mx68k_joy_set(port, byte)

            // P551: 入力モニタ用に、ゲストへ送出した生バイトを控える(表示専用)。
            // `joyState` は @Published のため SwiftUI 更新を伴う → 必ずメインスレッド
            // へディスパッチする(このクロージャは任意スレッドで発火しうる。上の
            // mx68k_joy_set の呼出しタイミング・値は一切変えていない)。
            DispatchQueue.main.async { [weak self] in
                self?.joyState[Int(port)] = byte
            }

            // P500: 多ボタンパッドの第2バンク(ストローブ High 側)。
            // 標準 2 ボタンでは一切送出しない(pad_btn1 は 0xFF 初期値のまま =
            // 既存動作と完全に同一・回帰防止)。
            // ★ビット位置は XM6 vm/ppi.cpp の MakeData 実コード由来(固定)。
            //   どのホストボタンを割り当てるかは UX 上の設計選択(調整可)。
            switch profile {
            case .standard2Button:
                break
            case .cpsfMD6Button:
                var b1: UInt8 = 0xFF
                if zButton.isPressed(gamepad)     { b1 &= ~UInt8(0x01) }   // bit0 = Z
                if y6Button.isPressed(gamepad)    { b1 &= ~UInt8(0x02) }   // bit1 = Y
                if x6Button.isPressed(gamepad)    { b1 &= ~UInt8(0x04) }   // bit2 = X
                if modeButton.isPressed(gamepad)  { b1 &= ~UInt8(0x08) }   // bit3 = MODE
                if cButton.isPressed(gamepad)     { b1 &= ~UInt8(0x20) }   // bit5 = C
                if startButton.isPressed(gamepad) { b1 &= ~UInt8(0x40) }   // bit6 = START
                mx68k_joy_set1(port, b1)
                // P593: 入力モニタ用に bank1 の生バイトを控える(表示専用)。
                // `joyState`(bank0)と同じく @Published のためメインスレッドへ
                // ディスパッチする。上の mx68k_joy_set1 の呼出しタイミング・値は無変更。
                DispatchQueue.main.async { [weak self] in
                    self?.joyState1[Int(port)] = b1
                }
            case .magicalPad4Button:
                // 起点は 0xFC — bit0/bit1 は常時 0 の固定シグネチャ(XM6 の
                // JoyMagical では data[1] の初期値が 0xfc であり、0xff 起点の
                // 「未使用ビット」ではなく能動的に出している信号)。
                // D ボタンは実装しない(XM6 側で B と同一ビットを操作しており
                // ビット位置が未確定 — P500 ではスコープ外)。
                var b1: UInt8 = 0xFC
                if l4Button.isPressed(gamepad) { b1 &= ~UInt8(0x04) }   // bit2 = L
                if r4Button.isPressed(gamepad) { b1 &= ~UInt8(0x08) }   // bit3 = R
                if b4Button.isPressed(gamepad) { b1 &= ~UInt8(0x40) }   // bit6 = B
                mx68k_joy_set1(port, b1)
                // P593: 入力モニタ用に bank1 の生バイトを控える(表示専用)。
                // `joyState`(bank0)と同じく @Published のためメインスレッドへ
                // ディスパッチする。上の mx68k_joy_set1 の呼出しタイミング・値は無変更。
                DispatchQueue.main.async { [weak self] in
                    self?.joyState1[Int(port)] = b1
                }
            }
        }
    }
}

/// P511: ホスト(macOS)キーコードの短い表示名。キーリマップ設定画面が
/// 「今どのホストキーが割り当たっているか」を表示するためだけに使う。
/// ★対応表は `InputManager.setupKeyMapping()` の辞書リテラルで使われている
///   `kVK_*` 定数の集合をそのまま機械的に転記したものであり、新しい対応関係は
///   発明していない(キーキャップ名なので Localizable.xcstrings では扱わない)。
///   末尾の数件のみ、既定の割当を持たないがユーザーが捕捉しうるキーとして追記した。
/// 未知のキーコードは `"Key <code>"` へフォールバックする(クラッシュさせない)。
func hostKeyDisplayName(_ code: UInt16) -> String {
    switch code {
    // 数字・記号 行
    case UInt16(kVK_ANSI_1): return "1"
    case UInt16(kVK_ANSI_2): return "2"
    case UInt16(kVK_ANSI_3): return "3"
    case UInt16(kVK_ANSI_4): return "4"
    case UInt16(kVK_ANSI_5): return "5"
    case UInt16(kVK_ANSI_6): return "6"
    case UInt16(kVK_ANSI_7): return "7"
    case UInt16(kVK_ANSI_8): return "8"
    case UInt16(kVK_ANSI_9): return "9"
    case UInt16(kVK_ANSI_0): return "0"
    case UInt16(kVK_ANSI_Minus): return "-"
    case UInt16(kVK_ANSI_Equal): return "="
    case UInt16(kVK_JIS_Yen): return "¥"
    case UInt16(kVK_Delete): return "Delete"
    // QWERTY 行
    case UInt16(kVK_Tab): return "Tab"
    case UInt16(kVK_ANSI_Q): return "Q"
    case UInt16(kVK_ANSI_W): return "W"
    case UInt16(kVK_ANSI_E): return "E"
    case UInt16(kVK_ANSI_R): return "R"
    case UInt16(kVK_ANSI_T): return "T"
    case UInt16(kVK_ANSI_Y): return "Y"
    case UInt16(kVK_ANSI_U): return "U"
    case UInt16(kVK_ANSI_I): return "I"
    case UInt16(kVK_ANSI_O): return "O"
    case UInt16(kVK_ANSI_P): return "P"
    case UInt16(kVK_ANSI_LeftBracket): return "["
    case UInt16(kVK_ANSI_RightBracket): return "]"
    case UInt16(kVK_Return): return "Return"
    // ASDF 行
    case UInt16(kVK_ANSI_A): return "A"
    case UInt16(kVK_ANSI_S): return "S"
    case UInt16(kVK_ANSI_D): return "D"
    case UInt16(kVK_ANSI_F): return "F"
    case UInt16(kVK_ANSI_G): return "G"
    case UInt16(kVK_ANSI_H): return "H"
    case UInt16(kVK_ANSI_J): return "J"
    case UInt16(kVK_ANSI_K): return "K"
    case UInt16(kVK_ANSI_L): return "L"
    case UInt16(kVK_ANSI_Semicolon): return ";"
    case UInt16(kVK_ANSI_Quote): return "'"
    case UInt16(kVK_ANSI_Backslash): return "\\"
    // ZXCV 行
    case UInt16(kVK_ANSI_Z): return "Z"
    case UInt16(kVK_ANSI_X): return "X"
    case UInt16(kVK_ANSI_C): return "C"
    case UInt16(kVK_ANSI_V): return "V"
    case UInt16(kVK_ANSI_B): return "B"
    case UInt16(kVK_ANSI_N): return "N"
    case UInt16(kVK_ANSI_M): return "M"
    case UInt16(kVK_ANSI_Comma): return ","
    case UInt16(kVK_ANSI_Period): return "."
    case UInt16(kVK_ANSI_Slash): return "/"
    case UInt16(kVK_JIS_Underscore): return "_"
    case UInt16(kVK_Space): return "Space"
    // 編集・制御
    case UInt16(kVK_Home): return "Home"
    case UInt16(kVK_ForwardDelete): return "Forward Delete"
    case UInt16(kVK_PageUp): return "Page Up"
    case UInt16(kVK_PageDown): return "Page Down"
    case UInt16(kVK_End): return "End"
    case UInt16(kVK_Escape): return "Esc"
    // 矢印
    case UInt16(kVK_LeftArrow): return "←"
    case UInt16(kVK_RightArrow): return "→"
    case UInt16(kVK_UpArrow): return "↑"
    case UInt16(kVK_DownArrow): return "↓"
    // テンキー
    case UInt16(kVK_ANSI_KeypadClear): return "Keypad Clear"
    case UInt16(kVK_ANSI_KeypadDivide): return "Keypad /"
    case UInt16(kVK_ANSI_KeypadMultiply): return "Keypad *"
    case UInt16(kVK_ANSI_KeypadMinus): return "Keypad -"
    case UInt16(kVK_ANSI_Keypad7): return "Keypad 7"
    case UInt16(kVK_ANSI_Keypad8): return "Keypad 8"
    case UInt16(kVK_ANSI_Keypad9): return "Keypad 9"
    case UInt16(kVK_ANSI_KeypadPlus): return "Keypad +"
    case UInt16(kVK_ANSI_Keypad4): return "Keypad 4"
    case UInt16(kVK_ANSI_Keypad5): return "Keypad 5"
    case UInt16(kVK_ANSI_Keypad6): return "Keypad 6"
    case UInt16(kVK_ANSI_KeypadEquals): return "Keypad ="
    case UInt16(kVK_ANSI_Keypad1): return "Keypad 1"
    case UInt16(kVK_ANSI_Keypad2): return "Keypad 2"
    case UInt16(kVK_ANSI_Keypad3): return "Keypad 3"
    case UInt16(kVK_ANSI_KeypadEnter): return "Keypad Enter"
    case UInt16(kVK_ANSI_Keypad0): return "Keypad 0"
    case UInt16(kVK_JIS_KeypadComma): return "Keypad ,"
    case UInt16(kVK_ANSI_KeypadDecimal): return "Keypad ."
    // ファンクション・特殊
    case UInt16(kVK_F1): return "F1"
    case UInt16(kVK_F2): return "F2"
    case UInt16(kVK_F3): return "F3"
    case UInt16(kVK_F4): return "F4"
    case UInt16(kVK_F5): return "F5"
    case UInt16(kVK_F6): return "F6"
    case UInt16(kVK_F7): return "F7"
    case UInt16(kVK_F8): return "F8"
    case UInt16(kVK_F9): return "F9"
    case UInt16(kVK_F10): return "F10"
    case UInt16(kVK_F11): return "F11"
    case UInt16(kVK_F12): return "F12"
    case UInt16(kVK_F13): return "F13"
    case UInt16(kVK_F14): return "F14"
    case UInt16(kVK_F15): return "F15"
    case UInt16(kVK_Help): return "Help"
    // 日本語入力キー
    case UInt16(kVK_JIS_Kana): return "かな"
    case UInt16(kVK_JIS_Eisu): return "英数"
    // ★以下は既定の割当を持たないが、ユーザーが捕捉しうるキー(表示のためだけの追記)
    case UInt16(kVK_ANSI_Grave): return "`"
    case UInt16(kVK_F16): return "F16"
    case UInt16(kVK_F17): return "F17"
    case UInt16(kVK_F18): return "F18"
    case UInt16(kVK_F19): return "F19"
    case UInt16(kVK_F20): return "F20"
    default: return "Key \(code)"
    }
}
