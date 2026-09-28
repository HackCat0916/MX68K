//
//  IOSGamepadInput.swift
//  MX68K-iOS
//
//  P824: iOS 版の物理 Bluetooth / MFi ゲームパッド対応。
//
//  ★規範(normative reference)は **MX68K 自身の macOS 実装**
//    `MX68K/App/Services/InputManager.swift` の GameController 配線
//    (P227 `startGamepadMonitoring`:705-731 / `registerGamepadHandler`:756-763 /
//     `installGamepadHandler`:768-810、P499 `autoAssignGamepadPort`:345-354 /
//     `clearGamepadPort`:330-332)である。ビット位置・idle 値・デッドゾーン・
//    既定ボタン割当・ポート自動割当順は 1 つも発明しておらず、既存の確立済み値の
//    転記のみ(P824 計画 §記号表)。権威ソースは `Bridge/EmulatorBridge.h` の
//    `mx68k_joy_set` コメント / upstream joystick.h。
//
//  ★P705 の `IOSKeyboardInput` / P714 の `TouchJoystickInput` と同じ事情で
//    このファイルが必要になっている: `InputManager.swift` は `import AppKit` /
//    `import Carbon` を持つ **macOS 専用**ファイルであり、iOS ターゲットの
//    Sources phase に入っていない(`ruby Scripts/add_ios_target.rb dump-sources MX68K-iOS`
//    で確認済み)。iOS 側に等価物を置く形をとる。
//
//  ★意図的なスコープ縮小(P824 計画 §1): P585 の多ボタンプロファイル / ボタン
//    リマップは持たない。プロファイルは `standard2Button` 固定 —— bank1
//    (`mx68k_joy_set1`)へは一度も書かない(= 常に既存の idle 値のまま)。
//    ボタン割当は既定値固定(TRIG1 = buttonA / TRIG2 = buttonB)。
//

import Foundation
import Combine
import GameController

/// 物理ゲームパッドの接続を追跡し、入力を X68000 のジョイスティックポートへ送出する。
///
/// ★シングルトンである理由: `MX68KiOSRootView`(仮想パッドの提示可否の判定)と
/// UIKit 側のライフサイクルフック(`EmulatorMTKView_iOS` のバックグラウンド遷移監視)の
/// **両方**から触られる。`TouchJoystickInput.shared` と同じ形をとる。
///
/// ★スレッド規約: `gamepadControllers` / `gamepadPorts` はメインスレッドからのみ触る
/// (通知観測は `queue: .main`、`startMonitoring` は `.task` から = メイン)。
/// `valueChangedHandler` は任意スレッドで発火し得るため、クロージャ内からこれらの
/// 辞書を参照しない(`InputManager.swift:765-767` と同じ規約)。
final class IOSGamepadInput: ObservableObject {

    static let shared = IOSGamepadInput()

    private init() {}

    // MARK: - ビット定数(InputManager.swift:794-806 からの逐語転写)
    //
    // 負論理: idle = 0xFF を起点に、押下されたビットを **クリア**する。
    // bit4 / bit7 は常時 1(未使用)。

    /// bit0 = 上 (`InputManager.swift:801`)
    private static let bitUp: UInt8 = 0x01
    /// bit1 = 下 (`InputManager.swift:802`)
    private static let bitDown: UInt8 = 0x02
    /// bit2 = 左 (`InputManager.swift:803`)
    private static let bitLeft: UInt8 = 0x04
    /// bit3 = 右 (`InputManager.swift:804`)
    private static let bitRight: UInt8 = 0x08
    /// bit5 = TRIG2、既定で buttonB (`InputManager.swift:806`、既定割当は `:169`)
    private static let bitTrig2: UInt8 = 0x20
    /// bit6 = TRIG1、既定で buttonA (`InputManager.swift:805`、既定割当は `:168`)
    private static let bitTrig1: UInt8 = 0x40

    /// 全ビット未押下の既定値(`InputManager.swift:332` / `:742-743` の
    /// `mx68k_joy_set(port, 0xFF)` と同一)。
    private static let idle: UInt8 = 0xFF

    /// 左スティックのデッドゾーン閾値(`InputManager.swift:795`)。
    private static let deadZone: Float = 0.5

    // MARK: - ポート割当状態

    /// port(0 = JOY1 / 1 = JOY2)→ 割当済みコントローラ。
    /// `MX68KiOSRootView.virtualPadAvailable` の判定に使う(同ビューから observe される)。
    @Published private(set) var gamepadControllers: [Int32: GCController] = [:]

    private var gamepadPorts: [ObjectIdentifier: Int32] = [:]

    /// `.task` の多重発火に対する冪等ガード(`MX68KiOSViewModel.start()` の
    /// `didStart` ガードと同型の理由 —— `.task` はビュー再出現で再発火し得る)。
    private var isMonitoring = false

    // MARK: - 監視開始

    /// アプリ起動時に `.task` から呼ぶ。接続済みコントローラへハンドラを登録し、
    /// 以後の抜き差しを NotificationCenter で追従する(`InputManager.swift:705-731`)。
    ///
    /// ★対になる停止処理(observer 解除)は持たない —— iOS のアプリライフサイクル上
    ///   `.onDisappear` 相当の確実な契機が無く(`MX68KiOSViewModel` と同じ事情)、
    ///   プロセス終了まで観測を継続して害はない(P824 計画 §2)。
    func startMonitoring() {
        guard !isMonitoring else { return }
        isMonitoring = true
        for controller in GCController.controllers() {
            registerHandler(controller)
        }
        let nc = NotificationCenter.default
        nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] note in
            guard let controller = note.object as? GCController else { return }
            self?.registerHandler(controller)
        }
        nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] note in
            guard let controller = note.object as? GCController else { return }
            // 押下中に抜かれた場合の押しっぱなし残留を避けるため、抜かれた側の
            // ポートだけ idle にする(もう一方のプレイヤーを巻き添えにしない)。
            self?.releasePort(for: controller)
        }
        mx68k_log(String(format: "[Swift][iOS][P824-GAMEPAD] monitoring started, connected=%d",
                         GCController.controllers().count))
    }

    /// extendedGamepad プロファイルを持たないコントローラはスキップ。
    /// 自動割当できるポート(0/1 の空き)が無い 3 台目以降もスキップ
    /// (`InputManager.swift:756-763`)。
    private func registerHandler(_ controller: GCController) {
        guard let pad = controller.extendedGamepad else { return }
        guard let port = autoAssignPort(for: controller) else { return }
        installHandler(pad, port: port)
        mx68k_log(String(format: "[Swift][iOS][P824-GAMEPAD] assigned port=%d vendor=%@",
                         Int(port), controller.vendorName ?? "unknown"))
    }

    /// 空いている最小ポート(0 → 1)を自動割当する。既に割当済みならそのポートを返す。
    /// 2 台を超える場合は nil(`InputManager.swift:343-354` の逐語移植)。
    private func autoAssignPort(for controller: GCController) -> Int32? {
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
    /// `InputManager.swift:330-332`(`clearGamepadPort`)と同じ手順。bank1 の idle 化
    /// (`:333`)は持たない —— `standard2Button` 固定で bank1 へ一度も書かないため。
    private func releasePort(for controller: GCController) {
        let id = ObjectIdentifier(controller)
        guard let port = gamepadPorts[id] else { return }
        gamepadPorts.removeValue(forKey: id)
        gamepadControllers[port] = nil
        controller.extendedGamepad?.valueChangedHandler = nil
        mx68k_joy_set(port, Self.idle)
        mx68k_log(String(format: "[Swift][iOS][P824-GAMEPAD] released port=%d (disconnect)", Int(port)))
    }

    /// 割当済み全ポートの送出バイトを idle へ戻す(押しっぱなし解除)。
    ///
    /// P705 の `IOSKeyboardInput.releaseAll(reason:)`・P714 の
    /// `TouchJoystickInput.releaseVirtualPad(reason:)` と同じ役割・同じ契機
    /// (`EmulatorMetalView_iOS.swift` の `didEnterBackgroundNotification` 観測)で呼ぶ。
    ///
    /// ★ポートの割当自体は維持する(`releasePort` と違い `gamepadPorts` /
    ///   `gamepadControllers` は変更しない)—— コントローラは物理的に接続されたままで
    ///   あり、フォアグラウンド復帰後も同じポート割当のまま使い続けられるようにするため。
    func releaseAllPorts(reason: String) {
        let ports = gamepadControllers.keys.sorted()
        for port in ports {
            mx68k_joy_set(port, Self.idle)
        }
        mx68k_log("[Swift][iOS][P824-GAMEPAD] releaseAllPorts reason=\(reason) ports=\(ports)")
    }

    /// 指定ポート向けの入力変化ハンドラを登録する(`InputManager.swift:768-810`)。
    /// port は不変値としてクロージャにキャプチャする —— `valueChangedHandler` は
    /// 任意スレッドで発火し得るため、クロージャ内から割当辞書を参照してはならない。
    private func installHandler(_ pad: GCExtendedGamepad, port: Int32) {
        pad.valueChangedHandler = { gamepad, _ in
            var byte: UInt8 = Self.idle
            let dz = Self.deadZone
            // d-pad と左スティックを OR 合成(どちらを倒しても方向入力として有効)。
            let up    = gamepad.dpad.up.isPressed    || gamepad.leftThumbstick.yAxis.value >  dz
            let down  = gamepad.dpad.down.isPressed  || gamepad.leftThumbstick.yAxis.value < -dz
            let left  = gamepad.dpad.left.isPressed  || gamepad.leftThumbstick.xAxis.value < -dz
            let right = gamepad.dpad.right.isPressed || gamepad.leftThumbstick.xAxis.value >  dz
            if up    { byte &= ~Self.bitUp }
            if down  { byte &= ~Self.bitDown }
            if left  { byte &= ~Self.bitLeft }
            if right { byte &= ~Self.bitRight }
            if gamepad.buttonA.isPressed { byte &= ~Self.bitTrig1 }
            if gamepad.buttonB.isPressed { byte &= ~Self.bitTrig2 }
            // GamePad_SetState(呼出先)はアトミック実装済み → メインスレッドへの
            // ディスパッチ不要(`InputManager.swift:807-808` と同じ前提)。
            mx68k_joy_set(port, byte)
        }
    }
}
