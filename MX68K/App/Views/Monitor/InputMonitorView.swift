import SwiftUI
import GameController

/// P551 — 入力モニタ。ゲームパッド(ポート0/1)の割当・プロファイル・
/// ゲストへ送出中の生バイト、マウスの状態(キャプチャ/座標/ボタン/設定値)を
/// 可視化する。
///
/// ★このパネルは 2 系統の更新経路が同居する(P695 で後者を追加):
///   (1) ゲームパッド / マウスの各セクション — 表示する状態は全て
///       `InputManager.shared`(ホスト側)に存在し、Bridge/Core からの取得が
///       一切不要。`@ObservedObject` の Combine 購読だけで更新する。
///   (2) キーボードロック LED セクション(P695)— このパネル唯一の Bridge 由来の
///       値(`mx68k_get_key_led()`)。CRTC/VC/BG 等の他モニタと全く同じ
///       「表示中だけ毎フレーム更新」機構に相乗りする
///       (`EmulatorEngine.inputMonitorVisible` + `fetchMonitorsAndPerfStats()`)。
///       ★`SoftKeyboardState` の 0.2 秒 `Timer` ポーリング(P228、この規律が
///        確立する以前の実装)は踏襲しない —— Bridge 読み取りを
///        `mx68k_run_frame()` と同一スレッドに縛るという設計規律に従うため。
///   どちらの経路も専用の `Timer` は作らない。
///
/// ★ゲームパッドのビット配置(負論理)は `InputManager.swift` 冒頭のコメントと
///   `valueChangedHandler` 内の実コードをそのまま転記したもの:
///   bit0=Up / bit1=Down / bit2=Left / bit3=Right / bit5=TRIG2 / bit6=TRIG1、
///   idle=0xFF で「ビットが 0 = 押下」。bit4/bit7 は常時 1。
struct InputMonitorView: View {
    @ObservedObject private var inputManager = InputManager.shared
    // マウスキャプチャ状態は InputManager ではなく専用オブジェクトが持つ
    // (SoftKeyboardCommands と同じ理由) — 別途観測する。
    @ObservedObject private var captureState = MouseCaptureState.shared
    /// P695 — キーボードロック LED セクションのみが使う(他のセクションは
    /// `InputManager.shared` 直読みで、この ViewModel を必要としない)。
    @EnvironmentObject private var emulatorViewModel: EmulatorViewModel

    /// 表示するボタンとそのビットマスク(負論理)。
    private static let buttons: [(name: String, mask: UInt8)] = [
        ("Up",    0x01),
        ("Down",  0x02),
        ("Left",  0x04),
        ("Right", 0x08),
        ("TRIG1", 0x40),
        ("TRIG2", 0x20),
    ]

    /// P695 — キーボードの 7 個のロック LED とそのビットマスク。
    /// ★ビット配置は新規解釈ではなく `Bridge/EmulatorBridge.h` の
    ///   `mx68k_get_key_led()` 宣言コメント(P228 で確定)をそのまま転記:
    ///   bit0=かな bit1=ローマ字 bit2=コード入力 bit3=CAPS bit4=INS
    ///   bit5=ひらがな bit6=全角(D7 はコマンド識別ビットで LED ではない)。
    ///   **負論理 — ビットが 0 のとき点灯**。ラベル文字列は
    ///   `SoftKeyboardView.swift` のキートップ表記と同一表記に揃えてある。
    private static let lockLEDs: [(name: String, mask: UInt8)] = [
        ("かな",     0x01),
        ("ローマ字", 0x02),
        ("コード入力", 0x04),
        ("CAPS",    0x08),
        ("INS",     0x10),
        ("ひらがな", 0x20),
        ("全角",     0x40),
    ]

    /// ★`title` は `String` ではなく `LocalizedStringKey`: `Label(String, ...)` は
    ///   非ローカライズ版オーバーロードが選ばれてしまい、`Localizable.xcstrings` の
    ///   訳語が反映されない(P553)。実行時 `String` 値から呼ぶ箇所
    ///   (`Self.buttons` の `button.name`)は呼出し側で明示的にラップする。
    private func boolLabel(_ title: LocalizedStringKey, _ on: Bool) -> some View {
        Label(title, systemImage: on ? "checkmark.circle.fill" : "xmark.circle")
            .foregroundColor(on ? .green : .secondary)
    }

    /// P695 — キーボードのロック LED セクション。このパネル唯一の Bridge 由来の値。
    /// ★`body` 直下ではなく独立した計算プロパティに切り出してある: 既存の `body` は
    ///   すでに 12 個の子ビューを持っており、これ以上直に積むと ViewBuilder の
    ///   型チェック時間だけが伸びるため(表示結果は同一)。
    @ViewBuilder
    private var keyboardLockLEDSection: some View {
        Divider()

        VStack(alignment: .leading, spacing: 6) {
            Text("Keyboard Lock LEDs").font(.subheadline).bold()
            let led = emulatorViewModel.keyLED
            // 派生表示(点灯/消灯)の元になった生バイトを必ず併記する。
            Text("Raw: 0x" + String(format: "%02X", Int(led)))
            // 7 個を 1 行に並べるとパネル幅(minWidth 460)を超えるため 4+3 に折る。
            lockLEDRow(Array(Self.lockLEDs.prefix(4)), led)
            lockLEDRow(Array(Self.lockLEDs.suffix(3)), led)
            Text("負論理: ビットが 0 のとき点灯(緑)。ソフトウェアキーボードの LED 表示と同じ生値を見ている")
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }

    /// P695 — ロック LED を 1 行分だけ並べる。負論理(ビットが 0 = 点灯)。
    @ViewBuilder
    private func lockLEDRow(_ leds: [(name: String, mask: UInt8)], _ led: UInt8) -> some View {
        HStack(spacing: 16) {
            ForEach(leds, id: \.name) { lockLED in
                boolLabel(LocalizedStringKey(lockLED.name), (led & lockLED.mask) == 0)
            }
        }
    }

    @ViewBuilder
    private func portSection(_ port: Int) -> some View {
        let key = Int32(port)
        let controller = inputManager.gamepadControllers[key]
        let profile = inputManager.gamepadProfiles[key]
        let state = inputManager.joyState[port]

        VStack(alignment: .leading, spacing: 6) {
            Text("Port \(port) (JOY\(port + 1))").font(.subheadline).bold()
            Text("Controller: \(controller.map { inputManager.gamepadDisplayName($0) } ?? "未接続")")
            Text("Profile: \(profile?.displayNameKey ?? "-")")
            if let state = state {
                Text("Raw: 0x" + String(format: "%02X", Int(state)))
                HStack(spacing: 16) {
                    ForEach(Self.buttons, id: \.name) { button in
                        // 負論理: ビットが 0 のとき押下。
                        boolLabel(LocalizedStringKey(button.name), (state & button.mask) == 0)
                    }
                }
            } else {
                Text("Raw: -- (未送出)")
                Text("このポートからはまだ 1 度も送出されていない")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            // P593: bank1(4/6 ボタンプロファイルの追加ボタン)。標準 2 ボタンでは
            // bank1 を一切送出しない(`installGamepadHandler` の switch が break)ため
            // そのプロファイルでは表示しない — 既存表示は無変更。
            // ビットマスクは `GamepadFunction.bank1Mask`(= ハンドラ内の既存定数)を使い、
            // 押下判定は bank0 と同じ負論理(ビットが 0 = 押下)。
            if let profile = profile, profile != .standard2Button {
                if let bank1 = inputManager.joyState1[port] {
                    Text("Raw (bank1): 0x" + String(format: "%02X", Int(bank1)))
                    HStack(spacing: 16) {
                        ForEach(GamepadFunction.rows(for: profile)
                            .filter { $0.bank1Mask != nil }) { function in
                            boolLabel(LocalizedStringKey(function.displayName),
                                      (bank1 & function.bank1Mask!) == 0)
                        }
                    }
                } else {
                    Text("Raw (bank1): -- (未送出)")
                    Text("追加ボタンはまだ 1 度も送出されていない")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Input").font(.headline)
            Divider()
            Text("Gamepad: ") + Text(inputManager.gamepadEnabled ? "有効" : "無効")
            Divider()
            portSection(0)
            Divider()
            portSection(1)
            Divider()

            VStack(alignment: .leading, spacing: 6) {
                Text("Mouse").font(.subheadline).bold()
                boolLabel("Capture", captureState.isCaptured)
                Text("Mode: ") + Text(inputManager.mouseAbsolute ? "Absolute" : "Relative")
                if inputManager.mouseAbsolute {
                    Text(String(format: "Position: (%.1f, %.1f)",
                                Double(inputManager.mousePosition.x),
                                Double(inputManager.mousePosition.y)))
                } else {
                    Text("Position: N/A (相対モード)")
                    Text("相対モードは移動量のみを送出するため、絶対座標は追跡されない(仕様)")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                HStack(spacing: 16) {
                    boolLabel("Left", inputManager.mouseButtonLeft)
                    boolLabel("Right", inputManager.mouseButtonRight)
                }
                boolLabel("Enabled", inputManager.mouseEnabled)
                Text("Sensitivity: \(inputManager.mouseSensitivity, specifier: "%.2f")")
            }

            Divider()

            VStack(alignment: .leading, spacing: 6) {
                Text("Connected Controllers (\(inputManager.connectedGamepads.count))")
                    .font(.subheadline).bold()
                if inputManager.connectedGamepads.isEmpty {
                    Text("なし").foregroundColor(.secondary)
                } else {
                    // GCController は Identifiable でないため、添字を id に使う
                    // (この一覧は表示専用で、並びは接続順に追記されるだけ)。
                    ForEach(inputManager.connectedGamepads.indices, id: \.self) { index in
                        let controller = inputManager.connectedGamepads[index]
                        let assigned = inputManager.gamepadControllers
                            .first(where: { $0.value === controller })?.key
                        Text("\(inputManager.gamepadDisplayName(controller)) — "
                             + (assigned.map { "Port \($0)" } ?? "未割当"))
                    }
                }
            }

            keyboardLockLEDSection

            Spacer()
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 460, minHeight: 480, alignment: .topLeading)
        // P695 — キーボードロック LED セクションだけが Bridge 読み取りを必要とする。
        // ゲームパッド/マウスの各セクションはこのフラグと無関係に更新される。
        .onAppear { emulatorViewModel.engine.inputMonitorVisible = true }
        .onDisappear { emulatorViewModel.engine.inputMonitorVisible = false }
    }
}
