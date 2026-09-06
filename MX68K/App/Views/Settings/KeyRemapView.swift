//
//  KeyRemapView.swift
//  MX68K
//
//  P511 — キーボードリマップ設定 UI(Docs/10 D-2)。
//  ホストキー 1 つ → X68000 スキャンコード 1 つの個別上書きと、全リセットのみを扱う。
//  XEiJ/XM6 の複数ホストキー対応・プロファイル・undo 履歴・CSV 入出力は意図的にスコープ外。
//
//  上書きは config.input.keyboardMap(String→String)に保存し、
//  InputManager.setupKeyMapping() が既定の割当を組み立てた「後」に適用する。
//

import SwiftUI
import AppKit

struct KeyRemapView: View {
    @EnvironmentObject var configManager: ConfigManager
    @Environment(\.dismiss) private var dismiss

    /// 現在捕捉中の行(X68000 スキャンコード)。nil = 捕捉していない。
    /// ★同時に有効な捕捉セッションは常に 1 つまで — この 1 つの状態で全行を制御し、
    ///   捕捉中は他行の「変更」ボタンを無効化する(次の keyDown 1 回が 2 つの捕捉を
    ///   同時に解決してしまう事故を防ぐ)。
    @State private var capturingScancode: UInt8?

    /// 捕捉中に登録している NSEvent ローカルモニタのトークン。
    @State private var monitor: Any?

    /// 表示用スナップショット(スキャンコード → 現在のホストキー表示名)。
    /// `InputManager.keyMapping` は @Published ではないため、変更のたびに明示的に
    /// 作り直して SwiftUI の再描画トリガとする。
    @State private var bindings: [UInt8: String] = [:]

    // MARK: - 行データ

    private struct RemapRow: Identifiable {
        let scancode: UInt8
        let label: String
        var id: UInt8 { scancode }
    }

    /// SHIFT(0x70)・CTRL(0x71)・CAPS(0x5D)は除外する。この 3 つは
    /// `InputManager.handleFlagsChanged` が macOS の modifierFlags から直接合成して
    /// おり、`keyMapping` 辞書を経由しない別経路のため本機構では扱えない。
    private static let excludedScancodes: Set<UInt8> = [0x70, 0x71, 0x5D]

    /// P584: 一覧の並び順。`softKeyLayout` の定義順は物理キーボードの配置座標順のため、
    /// アルファベット・数字が記号キーと入り混じって探しづらい。以下のグループ順へ
    /// 再構成する(グループ 1〜5 は下記の明示スキャンコード列、グループ 6 は
    /// 「1〜5 に含まれない残り」を `softKeyLayout` 上の相対順序のまま連結)。
    ///   1. ファンクションキー F1〜F10
    ///   2. XF1〜XF5
    ///   2.5(P587 追加). カーソルキー UP→DOWN→LEFT→RIGHT
    ///   3. アルファベット A→Z 昇順
    ///   4. メイン数字列 0→9 昇順
    ///   5. テンキー(数字 0→9 昇順 → 記号 → CLR → ENTER)
    ///   6. それ以外すべて(既存の相対順序を維持)
    private static let orderedScancodes: [UInt8] = [
        // 1. ファンクションキー F1〜F10
        0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c,
        // 2. XF1〜XF5
        0x55, 0x56, 0x57, 0x58, 0x59,
        // 2.5(P587). カーソルキー UP→DOWN→LEFT→RIGHT(up=0x3c down=0x3e
        //    left=0x3b right=0x3d)。`softKeyLayout` 上では up だけ 1 行上の
        //    「J/K/L/;/:/]」列に位置しており、グループ 6(相対順序維持)のままでは
        //    残り 3 方向から離れて表示されてしまうため、専用グループへ独立させる。
        0x3c, 0x3e, 0x3b, 0x3d,
        // 3. アルファベット A→Z(A=0x1e B=0x2e C=0x2c D=0x20 E=0x13 F=0x21 G=0x22
        //    H=0x23 I=0x18 J=0x24 K=0x25 L=0x26 M=0x30 N=0x2f O=0x19 P=0x1a Q=0x11
        //    R=0x14 S=0x1f T=0x15 U=0x17 V=0x2d W=0x12 X=0x2b Y=0x16 Z=0x2a)
        0x1e, 0x2e, 0x2c, 0x20, 0x13, 0x21, 0x22, 0x23, 0x18, 0x24, 0x25, 0x26, 0x30,
        0x2f, 0x19, 0x1a, 0x11, 0x14, 0x1f, 0x15, 0x17, 0x2d, 0x12, 0x2b, 0x16, 0x2a,
        // 4. メイン数字列 0→9(0=0x0b, 1〜9=0x02〜0x0a)
        0x0b, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
        // 5. テンキー — 数字 0→9(0=0x4f 1=0x4b 2=0x4c 3=0x4d 4=0x47 5=0x48 6=0x49
        //    7=0x43 8=0x44 9=0x45)
        0x4f, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49, 0x43, 0x44, 0x45,
        //    記号 / * - + = , .(既存の相対順序のまま)
        0x40, 0x41, 0x42, 0x46, 0x4a, 0x50, 0x51,
        //    CLR・ENTER
        0x3f, 0x4e,
    ]

    /// P584: テンキー側の数字・記号 17 個。メイン数字列/記号列と同じラベル文字で
    /// 並ぶため、一覧上では「テンキー 0」のように接頭辞を付けて区別する。
    /// CLR(0x3f)・ENTER(0x4e)は単語ラベルで自明なため対象外。
    private static let numpadScancodes: Set<UInt8> = [
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
        0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4f, 0x50, 0x51,
    ]

    /// `softKeyLayout`(SoftKeyboardView.swift)を再利用し、除外分を落として
    /// スキャンコードで重複除去した一覧(SHIFT は左右 2 行あるが除外対象)を
    /// `orderedScancodes` の順に並べ替えたもの。
    private static let rows: [RemapRow] = {
        // 除外(SHIFT/CTRL/CAPS)とスペーサー(scancode == 0)のフィルタはここで
        // 一度だけ適用し、グループ 6 の差集合構築にも同じ辞書/順序リストを使う
        // — フィルタを落とすとリマップ不可能なキーが一覧へ混入するため。
        var byScancode: [UInt8: SoftKeyDef] = [:]
        var layoutOrder: [UInt8] = []
        for key in softKeyLayout
        where key.scancode != 0 && !excludedScancodes.contains(key.scancode) {
            guard byScancode[key.scancode] == nil else { continue }
            byScancode[key.scancode] = key
            layoutOrder.append(key.scancode)
        }

        var seen = Set<UInt8>()
        var result: [RemapRow] = []
        func appendRow(_ scancode: UInt8) {
            guard let key = byScancode[scancode], !seen.contains(scancode) else { return }
            seen.insert(scancode)
            result.append(RemapRow(scancode: scancode, label: x68kLabel(key)))
        }
        for scancode in orderedScancodes { appendRow(scancode) }   // グループ 1〜5(2.5 含む)
        for scancode in layoutOrder { appendRow(scancode) }        // グループ 6(残り)
        return result
    }()

    /// 一覧に出す X68000 側のラベル。改行入りラベル(ROLL\nUP 等)は 1 行へ潰し、
    /// 空ラベル(スペースキー・ろキー)はスキャンコードから補う。
    /// P584: テンキー側のキーには「テンキー 」接頭辞を付ける(この関数は
    /// `KeyRemapView` 専用のラベル変換であり、`softKeyLayout` の `label` 自体は
    /// 変更しないため、オンスクリーンソフトキーボードの見た目には影響しない)。
    /// P586: カーソルキー 4 種は `softKeyLayout` の生ラベル(小文字 1 単語、
    /// うち right は `"rigt"` という既存の誤字)がそのまま出ており記号キーの
    /// 並びに埋もれて見分けにくかったため、大文字 + 矢印記号の固定文字列を
    /// 返す(`softKeyLayout` 自体は変更しない — オンスクリーンソフトキーボードは
    /// `iconName` 優先描画のため無関係)。
    private static func x68kLabel(_ key: SoftKeyDef) -> String {
        switch key.scancode {
        case 0x3c: return "UP ↑"
        case 0x3e: return "DOWN ↓"
        case 0x3b: return "LEFT ←"
        case 0x3d: return "RIGHT →"
        default: break
        }
        var flattened = key.label
            .replacingOccurrences(of: "\n", with: " ")
            .trimmingCharacters(in: .whitespaces)
        // P739: F1〜F9 は実機キートップ印字に合わせ `softKeyLayout` 側で
        // "F 1"〜"F 9"(1桁を2桁のF10と揃えるための空白入り)になっているが、
        // オンスクリーンキーボードと違いこの一覧はキートップの見た目を
        // 再現する場ではないため、空白だけ詰める(softKeyLayout自体は無変更)。
        if flattened.count == 3, flattened.hasPrefix("F "), let last = flattened.last, last.isNumber {
            flattened = "F" + String(last)
        }
        if !flattened.isEmpty {
            return numpadScancodes.contains(key.scancode) ? "テンキー " + flattened : flattened
        }
        switch key.scancode {
        case 0x35: return "SPACE"
        case 0x34: return "_"
        default:   return String(format: "0x%02X", key.scancode)
        }
    }

    // MARK: - 本体

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 6) {
                Text("Remap Keyboard")
                    .font(.headline)
                Text("Choose which host key sends each X68000 key. SHIFT, CTRL and CAPS are not listed: macOS reports them as modifier flags, so they cannot be remapped here.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.horizontal, 16)
            .padding(.top, 16)
            .padding(.bottom, 10)

            List {
                ForEach(Self.rows) { row in
                    rowView(row)
                }
            }

            Divider()

            HStack {
                Button("Reset All") { resetAll() }
                Spacer()
                Button("Close") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            .padding(16)
        }
        .frame(width: 460, height: 520)
        .onAppear { refreshBindings() }
        // シートがスワイプ / Close / ⌘W 等で閉じられたときにモニタを取り残さない。
        .onDisappear { stopCapture() }
    }

    @ViewBuilder
    private func rowView(_ row: RemapRow) -> some View {
        HStack(spacing: 12) {
            Text(row.label)
                .font(.system(.body, design: .monospaced))
                .frame(width: 96, alignment: .leading)
                .lineLimit(1)

            if capturingScancode == row.scancode {
                Text("Press a key…")
                    .foregroundColor(.accentColor)
                Spacer()
                // ★キー入力に依存しないキャンセル手段。ESC は「捕捉対象の 1 つ」で
                //   あってキャンセルキーを兼任できないため、必ずマウス操作で戻せること。
                Button("Cancel") { stopCapture() }
            } else {
                if let name = bindings[row.scancode] {
                    Text(name)
                } else {
                    Text("Unassigned")
                        .italic()
                        .foregroundColor(.secondary)
                }
                Spacer()
                Button("Change") { startCapture(row.scancode) }
                    .disabled(capturingScancode != nil)
            }
        }
        .padding(.vertical, 1)
    }

    // MARK: - 捕捉

    /// 次の keyDown を 1 回だけ捕捉する。既存の捕捉セッションがあれば必ず先に畳む。
    private func startCapture(_ scancode: UInt8) {
        stopCapture()
        capturingScancode = scancode
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
            // ★捕捉した keyDown はここで消費する(nil を返す)。素通しにすると、
            //   ESC(スキャンコード 0x01)を割り当てようとした瞬間にシート自体が
            //   閉じたり、RET(0x1d)が Close の既定ボタンを誤発火させたりする。
            completeCapture(scancode, hostKeyCode: event.keyCode)
            return nil
        }
    }

    /// 捕捉完了 — 上書きを保存してライブ適用し、通常のイベントフローへ戻す。
    private func completeCapture(_ scancode: UInt8, hostKeyCode: UInt16) {
        stopCapture()
        configManager.config.input.keyboardMap[String(hostKeyCode)] = String(scancode)
        applyAndSave()
        refreshBindings()
    }

    /// モニタを解除して捕捉モードを抜ける(キーストロークを発行しない)。冪等。
    private func stopCapture() {
        if let monitor = monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
        capturingScancode = nil
    }

    // MARK: - 保存 / 表示更新

    private func resetAll() {
        stopCapture()
        configManager.config.input.keyboardMap = [:]
        applyAndSave()
        refreshBindings()
    }

    /// 保存 + ライブ適用(リセット不要)。`InputSettingsView.applyAndSave()` は
    /// private で別ファイルからは呼べないため、同じ 2 行をこちらでも持つ。
    private func applyAndSave() {
        configManager.save()
        InputManager.shared.applyConfig(configManager.config.input)
    }

    /// 現在の実効割当(既定 + 上書き)を読み直して表示用スナップショットを作る。
    private func refreshBindings() {
        var snapshot: [UInt8: String] = [:]
        for row in Self.rows {
            if let hostKey = InputManager.shared.currentHostKeyCode(forScancode: row.scancode) {
                snapshot[row.scancode] = hostKeyDisplayName(hostKey)
            }
        }
        bindings = snapshot
    }
}
