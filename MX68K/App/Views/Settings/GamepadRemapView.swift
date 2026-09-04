//
//  GamepadRemapView.swift
//  MX68K
//
//  P585 — ゲームパッドのボタン割当設定 UI。
//  KeyRemapView(P511)と同型の構成(タイトル + 一覧 + Close)だが、ホスト側の物理
//  ボタンは有限個(A/B/X/Y/L1/R1/L2/R2)しかないため「次の入力を 1 回だけ捕捉する」
//  方式ではなく各行を直接 Picker にしている(候補が一覧でき、誤操作が少ない)。
//
//  「A・B の入れ替え」は A / B(旧表記: Trigger 1 / Trigger 2)の Picker を選び直すことで
//  実現する(専用の入れ替えボタンは設けない — 同じ機構で 4/6 ボタンの割当変更も
//  統一的に扱えるため)。
//
//  上書きは config.input.gamepadMap(String→String)に保存し、
//  InputManager.setGamepadButtonMapping(_:function:port:) でライブ適用する。
//  一覧に出す機能は表示時点のプロファイル(標準 2 / CPSF-MD 6 / マジカルパッド 4)に
//  応じて GamepadFunction.rows(for:) が決める。
//

import SwiftUI

struct GamepadRemapView: View {
    @EnvironmentObject var configManager: ConfigManager
    @Environment(\.dismiss) private var dismiss
    /// 対象ポート(0 = JOY1 / 1 = JOY2)。
    let port: Int32

    /// プロファイル(@Published)の変化を購読して一覧の行数を追随させる。
    @ObservedObject private var inputManager = InputManager.shared

    private var currentProfile: GamepadButtonProfile {
        inputManager.gamepadProfiles[port] ?? .standard2Button
    }

    private var rows: [GamepadFunction] {
        GamepadFunction.rows(for: currentProfile)
    }

    /// P587: 「その他」Section に出す機能(CPSF-MD の MODE/START のみ。
    /// それ以外のプロファイルでは空 = Section 非表示)。
    private var otherRows: [GamepadFunction] {
        GamepadFunction.otherRows(for: currentProfile)
    }

    /// 「追加ボタン」Section に出す機能(基礎バンクにも「その他」にも属さない残り)。
    private var extraRows: [GamepadFunction] {
        let other = otherRows
        return rows.filter { !GamepadFunction.baseRows.contains($0) && !other.contains($0) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 6) {
                // ポート番号は数字ラベルのため verbatim(翻訳対象外)。
                HStack(spacing: 6) {
                    Text("Remap Gamepad Buttons")
                    Text(verbatim: "— Port \(port + 1)")
                }
                .font(.headline)
                Text("Choose which controller button drives each X68000 function. The list follows the button profile selected for this port; change the profile first if you want the extra buttons.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.horizontal, 16)
            .padding(.top, 16)
            .padding(.bottom, 10)

            // P586: 基礎バンク(全プロファイル共通の A/B)と追加バンクを Section で
            // 視覚的に分ける。プロファイル名の「4 ボタン」「6 ボタン」は追加バンク側の
            // 個数を指すため、1 本のフラットな一覧では行数と名前が合わないように
            // 見えるのを解消する。
            // P587: CPSF-MD の MODE/START は 6 面ボタンの数に含まれない別枠のため
            // 「その他」Section へさらに分離する(該当機能が無いプロファイルでは
            // その Section 自体を表示しない)。
            List {
                Section(header: Text("Base (all profiles)")) {
                    ForEach(GamepadFunction.baseRows) { function in
                        pickerRow(function)
                    }
                }
                if !extraRows.isEmpty {
                    Section(header: Text("Extra buttons")) {
                        ForEach(extraRows) { function in
                            pickerRow(function)
                        }
                    }
                }
                if !otherRows.isEmpty {
                    Section(header: Text("Other (MODE / START)")) {
                        ForEach(otherRows) { function in
                            pickerRow(function)
                        }
                    }
                }
            }

            Divider()

            HStack {
                Spacer()
                Button("Close") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            .padding(16)
        }
        .frame(width: 360, height: sheetHeight)
    }

    /// 1 行分の Picker(基礎 / 追加 / その他の全 Section で共用)。
    /// 機能名(A / B / Z / MODE 等)は X68000 側のボタン刻印に相当する
    /// ラベル文字のため verbatim 表示。
    @ViewBuilder
    private func pickerRow(_ function: GamepadFunction) -> some View {
        Picker(selection: binding(for: function)) {
            ForEach(GamepadPhysicalButton.allCases) { button in
                Text(verbatim: button.displayName).tag(button)
            }
        } label: {
            Text(verbatim: function.displayName)
        }
    }

    /// 行数依存の高さ(標準 2 ボタン = 2 行 / マジカルパッド = 5 行 / CPSF-MD = 8 行)。
    /// P586: Section ヘッダの分を加算する。
    /// P587: 「その他」Section を加えた 3 段構成に合わせ、実際に表示される
    /// Section の本数(「基礎」は常時 + 追加/その他は該当時のみ)で加算する。
    private var sheetHeight: CGFloat {
        // ヘッダ + Close 行の固定分 + 1 行あたりの概算高さ + Section ヘッダ分。
        let hasExtraSection = !extraRows.isEmpty
        let hasOtherSection = !otherRows.isEmpty
        return 190 + CGFloat(rows.count) * 32
            + 30                                // 「基礎」Section ヘッダ(常時)
            + (hasExtraSection ? 30 : 0)
            + (hasOtherSection ? 30 : 0)
    }

    /// Picker のバインディング。読み出しは config を直接引く(InputManager 側の
    /// `gamepadOverrides` は同じ辞書のライブ複製であり、両者は applyConfig /
    /// setGamepadButtonMapping で常に同期する)。未設定なら既定値へフォールバック。
    private func binding(for function: GamepadFunction) -> Binding<GamepadPhysicalButton> {
        Binding<GamepadPhysicalButton>(
            get: {
                let raw = configManager.config.input.gamepadMap[key(function)]
                return raw.flatMap(GamepadPhysicalButton.init(rawValue:)) ?? function.defaultButton
            },
            set: { newButton in
                // ライブ適用(ハンドラ再登録)+ 保存。KeyRemapView.completeCapture と
                // 同じく、config への書込みは View 側の責務。
                InputManager.shared.setGamepadButtonMapping(newButton, function: function, port: port)
                configManager.config.input.gamepadMap[key(function)] = newButton.rawValue
                configManager.save()
            }
        )
    }

    /// 上書き辞書のキー("\(port):\(function.rawValue)" — InputManager と同一形式)。
    private func key(_ function: GamepadFunction) -> String {
        "\(port):\(function.rawValue)"
    }
}
