import SwiftUI
import Foundation
import GameController   // P499b: 接続中コントローラ(GCController)の一覧を Picker に出す

// P194: 入力設定タブ (Docs/03 §3.5)
// キーボード(カーソルキー↔テンキー代替)とマウス(有効/感度)。
// P227: ゲームパッド(GameController framework)を有効/無効で切替。
// Bridge の mx68k_joy_set 経由でゲストへ配送する(実装済み)。
// P499: 2P 対応。port0(JOY1)/port1(JOY2)への割当を本画面で扱う。
// P499b: 接続順の自動割当だけでは利用者の意図と一致しないため(GCController.controllers()
// の順序は OS 内部の管理順)、どのコントローラをどちらのポートへ繋ぐかを Picker で明示選択する。
struct InputSettingsView: View {
    @EnvironmentObject var configManager: ConfigManager
    // P499b: ポート割当状況(@Published gamepadControllers / connectedGamepads)を
    // 購読して表示を更新する。
    @ObservedObject private var inputManager = InputManager.shared
    // P585: リマップシートの表示状態。P511 の `showKeyRemap`(Bool + .sheet(isPresented:))
    // から enum 1 本化へ移行した — 同一 View へ `.sheet` 修飾子を 2 つ付けると片方しか
    // 機能しない(P578 で確認済みの SwiftUI の制約)ため、キーボード用とゲームパッド用を
    // 1 つの `.sheet(item:)` に集約する。
    private enum InputRemapSheet: Identifiable {
        case keyboard
        case gamepad(port: Int32)
        var id: String {
            switch self {
            case .keyboard: return "keyboard"
            case .gamepad(let p): return "gamepad-\(p)"
            }
        }
    }
    @State private var activeSheet: InputRemapSheet?

    var body: some View {
        Form {
            Section(header: Text("Keyboard")) {
                Toggle("Send arrow keys as numeric keypad", isOn: Binding(
                    get: { configManager.config.input.arrowKeysAsNumpad },
                    set: { newValue in
                        configManager.config.input.arrowKeysAsNumpad = newValue
                        applyAndSave()
                    }
                ))
                // P581: grouped Form のラベル列(最長ラベル幅で決まる)がラベル無し行の
                // 左に空白を作り、説明文が右へ押し出されて見えていた。行そのものを
                // 幅いっぱいに広げ、左揃えを明示することでラベル列の影響を外す。
                Text("Some games use the numeric keypad for movement. Enable this to use the arrow keys instead.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)

                // P511: キーの個別リマップ(ホストキー → X68000 スキャンコード)。
                Divider()
                Button("Remap Keys…") { activeSheet = .keyboard }
                Text("Assign a different host key to any X68000 key.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Section(header: Text("Mouse")) {
                Toggle("Enable mouse emulation", isOn: Binding(
                    get: { configManager.config.input.mouseEnabled },
                    set: { newValue in
                        configManager.config.input.mouseEnabled = newValue
                        applyAndSave()
                    }
                ))
                // P196: マウスモード(絶対 = ホストカーソルに追従 / 相対 = 移動量を送る)。
                Picker("Mouse Mode", selection: Binding(
                    get: { configManager.config.input.mouseAbsolute },
                    set: { newValue in
                        configManager.config.input.mouseAbsolute = newValue
                        applyAndSave()
                    }
                )) {
                    Text("Absolute (follow host cursor)").tag(true)
                    Text("Relative (send movement)").tag(false)
                }
                Text("Absolute follows the host cursor position (recommended). Relative sends movement deltas (sensitivity slider active).")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                HStack {
                    Text("Sensitivity")
                    Slider(
                        value: Binding(
                            get: { configManager.config.input.mouseSensitivity },
                            set: { newValue in
                                configManager.config.input.mouseSensitivity = newValue
                                applyAndSave()
                            }
                        ),
                        in: 0.25...3.0,
                        step: 0.25
                    )
                    Text(String(format: "%.2fx", configManager.config.input.mouseSensitivity))
                        .font(.subheadline)
                        .fixedSize(horizontal: false, vertical: true)
                        .monospacedDigit()
                }
            }

            Section(header: Text("Gamepad")) {
                Toggle("Enable gamepad", isOn: Binding(
                    get: { configManager.config.input.gamepadEnabled },
                    set: { newValue in
                        configManager.config.input.gamepadEnabled = newValue
                        applyAndSave()
                    }
                ))
                // P581: 上の「カーソルキーをテンキーとして送る」の説明文と同じ是正。
                Text("Use a connected controller as the X68000 joystick (port 1). D-pad and left stick move; A = trigger 1, B = trigger 2.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)

                // P499b: 2P 対応。どのコントローラをどちらのポートへ繋ぐかを明示的に選ぶ。
                Divider()
                Text("Choose which connected controller is assigned to each port. A newly connected controller is auto-assigned to the first free port; you can always override it here.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                gamepadPortPicker(title: "Port 1 (JOY1)", port: 0)
                gamepadPortPicker(title: "Port 2 (JOY2)", port: 1)

                // P500: 多ボタンパッド(ストローブ線 L/H の 2 バンク多重)プロファイル。
                Divider()
                Text("Some titles shipped with a multi-button pad (Street Fighter II' with CPSF-MD, Fatal Fury 2 with the Magical Pad). These read extra buttons through the strobe line. Leave this on Standard unless the game supports one of them.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                gamepadProfilePicker(title: "Button profile (Port 1)", port: 0)
                gamepadProfilePicker(title: "Button profile (Port 2)", port: 1)

                // P585: どのコントローラボタンをどの X68000 側機能へ割り当てるか
                // (A・B の入れ替えを含む)。一覧は上のプロファイル選択に追随する。
                Divider()
                Text("Assign a different controller button to each X68000 function. The list of functions follows the button profile selected above.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Button("Remap Buttons… (Port 1)") { activeSheet = .gamepad(port: 0) }
                Button("Remap Buttons… (Port 2)") { activeSheet = .gamepad(port: 1) }
            }
        }
        // P580 — macOS 標準のグループスタイル(Form 自体がスクロール可能)。
        .formStyle(.grouped)
        // P511/P585: シートは環境を継承するが、ConfigManager の注入は明示しておく。
        // ★`.sheet` はこの 1 箇所のみ(2 つ付けると片方しか機能しない — P578)。
        .sheet(item: $activeSheet) { sheet in
            switch sheet {
            case .keyboard:
                KeyRemapView().environmentObject(configManager)
            case .gamepad(let port):
                GamepadRemapView(port: port).environmentObject(configManager)
            }
        }
    }

    // MARK: - P499b: ポート割当 Picker

    /// Picker の選択肢 1 件。`GCController` を直接 tag にせず `ObjectIdentifier` を
    /// 介する(型安全・値としての同一性が明確で、選択値がコントローラを保持しない)。
    private struct GamepadEntry: Identifiable, Hashable {
        let id: ObjectIdentifier
        let name: String
    }

    /// 接続中コントローラの一覧。同名(同型番)の 2 台を区別できるよう、
    /// 名前が重複する場合のみ通し番号を付ける。
    private var gamepadEntries: [GamepadEntry] {
        let controllers = inputManager.connectedGamepads
        var counts: [String: Int] = [:]
        for controller in controllers {
            counts[inputManager.gamepadDisplayName(controller), default: 0] += 1
        }
        var used: [String: Int] = [:]
        return controllers.map { controller in
            let base = inputManager.gamepadDisplayName(controller)
            guard counts[base, default: 0] > 1 else {
                return GamepadEntry(id: ObjectIdentifier(controller), name: base)
            }
            let index = (used[base] ?? 0) + 1
            used[base] = index
            return GamepadEntry(id: ObjectIdentifier(controller), name: "\(base) #\(index)")
        }
    }

    /// 指定ポート用の Picker(選択値 = 割当済みコントローラの `ObjectIdentifier`、
    /// nil = 未接続)。選択された ID から `connectedGamepads` を引き直して
    /// `assignGamepadPort(_:toPort:)` へ渡す。
    private func gamepadPortPicker(title: LocalizedStringKey, port: Int32) -> some View {
        Picker(title, selection: Binding<ObjectIdentifier?>(
            get: { inputManager.gamepadControllers[port].map(ObjectIdentifier.init) },
            set: { newID in
                let controller = newID.flatMap { id in
                    inputManager.connectedGamepads.first { ObjectIdentifier($0) == id }
                }
                inputManager.assignGamepadPort(controller, toPort: port)
            }
        )) {
            Text("Not connected").tag(ObjectIdentifier?.none)
            ForEach(gamepadEntries) { entry in
                Text(entry.name).tag(ObjectIdentifier?.some(entry.id))
            }
        }
    }

    /// P500: 指定ポートのボタンプロファイル Picker(既定 = 標準 2 ボタン)。
    /// 選択はセッション内のライブ設定(`InputManager`)で、config.json には保存しない
    /// (既定に戻る = 常に既存動作から始まる、という安全側の挙動)。
    private func gamepadProfilePicker(title: LocalizedStringKey, port: Int32) -> some View {
        Picker(title, selection: Binding<GamepadButtonProfile>(
            get: { inputManager.gamepadProfiles[port] ?? .standard2Button },
            set: { inputManager.setGamepadProfile($0, forPort: port) }
        )) {
            ForEach(GamepadButtonProfile.allCases) { profile in
                Text(LocalizedStringKey(profile.displayNameKey)).tag(profile)
            }
        }
    }

    /// 保存 + ライブ適用(リセット不要)。
    private func applyAndSave() {
        configManager.save()
        InputManager.shared.applyConfig(configManager.config.input)
    }
}
