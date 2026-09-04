//
//  IOSKeyboardInput.swift
//  MX68K-iOS
//
//  P705: 物理 / Bluetooth キーボードの `UIPress` を **X68000 の生スキャンコード**へ
//  写し、Bridge の `mx68k_key_down` / `mx68k_key_up`
//  (`Bridge/EmulatorBridge.h:556-557`)へ送る。
//
//  ★規範(normative reference)は **MX68K 自身の macOS 実装**
//    `MX68K/App/Services/InputManager.swift:467-537`(キーマップ)および
//    `:569-585`(押下/解放)・`:587-606`(修飾キー)である。
//    本ファイルの目的は「macOS 版と同一の写像を iOS でも再現する」ことであり、
//    新しい番号体系・新しいスキャンコードは 1 つも発明していない
//    (P705 計画 §記号表「一次情報源についての明示」)。
//    スキャンコード値そのものが実機 X68000 として正しいかは本サイクルの
//    検証範囲外 —— macOS 版で長期間 hands-on 検証済みの資産の横展開である。
//
//  ★重複スキャンコードを守る機構は **2 つに分かれている**(帰属を取り違えないこと。
//    P705 計画 §変更内容 B-3 の訂正表):
//
//      0x5E (INS)   … `.keyboardInsert` / `.keyboardHelp` の 2 経路から届き得る。
//                     `keyMap` に**載る**。守るのは `handleUp` の汎用ガード
//                     `guard !heldKeys.values.contains(code)`
//                     (= `InputManager.swift:582` の逐語移植)。
//      0x70 (SHIFT) … L/R Shift の 2 キー。`keyMap` には**載せない**。
//      0x71 (CTRL)  … L/R Ctrl の 2 キー。`keyMap` には**載せない**。
//                     この 2 つを守るのは `processModifiers()` の
//                     **OR 合成 + 状態遷移エッジ送出**であり、`heldKeys` ではない。
//
//    修飾キー用の重複排除を `heldKeys` 側へ足してはならない —— 同じ保証が
//    2 箇所へ分散し、片方だけ直したときに静かに壊れる。
//
//  ★T-0(2026-08-29, ユーザー hands-on / iPad シミュレータ)の実測結果:
//    - 単独修飾キーは **自身の keyCode で届く**(Shift 単独 = `usage=0xE1`、
//      Ctrl 単独 = `usage=0xE0`)。よって経路 A は生きている。経路 B
//      (`UIKey.modifierFlags`)は冗長化のために併走させたままにする。
//    - キーリピートは began の反復として届く(B-6 の「保持中 usage の down は無視」を確定)。
//    - **`pressesCancelled` / `resignFirstResponder` はアプリのバックグラウンド遷移で
//      発火しなかった**。よって押しっぱなし解除の**主たる保証**は
//      `EmulatorMTKView_iOS` が張る `UIApplication.didEnterBackgroundNotification`
//      の観測に置く(本ファイルは `releaseAll(reason:)` を提供する側)。
//    - 英字キーボードでのテストのため、かな / ¥ / _ / F13-F15 / テンキー特殊キー等は
//      物理的に押せず未計測(ユーザー承認済み、§残留リスク R-2 / R-5)。
//    - **かな(`.keyboardLANG1`)/ ローマ字(`.keyboardLANG2`)の 2 行は
//      テーブルから外してある**(R-11)。HID 値が信頼度 L(想起値・静的裏取りも無し)の
//      まま出荷すると「別のキーを誤って『かな』として送る」実害があり得るため、
//      誤った割当を入れるより**無割当のまま**を選ぶ。
//

import Foundation
import UIKit

/// `UIPress` → X68000 スキャンコードの写像と、押下状態の保持。
///
/// スレッド規約(P705 計画 §変更内容 B-7): `UIPress` はメインスレッドで配送され、
/// `mx68k_key_down/up` は macOS 側でも同じくメインスレッド(AppKit イベント)から
/// 呼ばれている。**新しいスレッド越えは導入していない。**
/// `releaseAll(reason:)` を呼ぶ通知観測も `queue: .main` で登録すること。
final class IOSKeyboardInput {

    /// `pressesBegan` / `pressesEnded` / `pressesCancelled` のどれで受けたか。
    enum Phase {
        case began
        case ended
        case cancelled
    }

    // MARK: - スキャンコード定数(InputManager.swift からの逐語転写)

    /// SHIFT (`InputManager.swift:590`)
    private static let scShift: UInt8 = 0x70
    /// CTRL (`InputManager.swift:591`)
    private static let scCtrl: UInt8 = 0x71
    /// CAPS (`InputManager.swift:601-602`)。**保持せず down+up のパルス**。
    private static let scCaps: UInt8 = 0x5D

    // MARK: - 修飾キーの HID usage
    //
    // ★これらは `keyMap` に載せない(§B-3)。`processModifiers()` が専有する。
    // Cmd (`.keyboardLeftGUI` / `.keyboardRightGUI` / `.command`) は
    // **意図的に未割当** —— macOS 側 `InputManager.swift:605` の
    // 「COMMAND (Cmd) intentionally not mapped」を iOS でも踏襲する。

    private static let shiftUsages: Set<Int> = [
        UIKeyboardHIDUsage.keyboardLeftShift.rawValue,      // M1
        UIKeyboardHIDUsage.keyboardRightShift.rawValue,     // M2
    ]
    private static let controlUsages: Set<Int> = [
        UIKeyboardHIDUsage.keyboardLeftControl.rawValue,    // M3
        UIKeyboardHIDUsage.keyboardRightControl.rawValue,   // M4
    ]
    private static let capsLockUsage = UIKeyboardHIDUsage.keyboardCapsLock.rawValue  // M5

    private static func isModifierUsage(_ usage: Int) -> Bool {
        shiftUsages.contains(usage) || controlUsages.contains(usage) || usage == capsLockUsage
    }

    // MARK: - 非修飾キーの変換表
    //
    // ★このテーブルに載るのは P705 計画 付録表 #1-#98(非修飾キー)のみ。
    //   #99 かな / #100 ローマ字 は R-11 により**意図的に除外**している。
    //   修飾キー M1-M5 も**意図的にここへ載せない**(§B-3、上の説明)。
    //
    // ★HID 値は数値リテラルで書かず **シンボル名**で書く。名前の誤りは
    //   コンパイルエラーになり、実行時の静かな誤対応にならない
    //   (P705 計画 §記号表「この値が誤りなら出力はどうなるか」列)。
    //   キーは `Int`(= `UIKeyboardHIDUsage.RawValue`)まで落とす —— 未知の raw 値は
    //   単に「表に無い」として安全に無視される。
    //
    // ★意図的に除外した X68000 キー: 記号入力 (0x5C)・登録 (0x5F)・ひらがな・全角。
    //   これは転写漏れではなく、**macOS 版が今日そもそも割り当てていない**ことの
    //   正確な反映である(無いものを発明しない)。
    private static let keyMap: [Int: UInt8] = [
        // --- 数字段 (InputManager.swift:469-476) ---
        UIKeyboardHIDUsage.keyboard1.rawValue:                 0x02,
        UIKeyboardHIDUsage.keyboard2.rawValue:                 0x03,
        UIKeyboardHIDUsage.keyboard3.rawValue:                 0x04,
        UIKeyboardHIDUsage.keyboard4.rawValue:                 0x05,
        UIKeyboardHIDUsage.keyboard5.rawValue:                 0x06,
        UIKeyboardHIDUsage.keyboard6.rawValue:                 0x07,
        UIKeyboardHIDUsage.keyboard7.rawValue:                 0x08,
        UIKeyboardHIDUsage.keyboard8.rawValue:                 0x09,
        UIKeyboardHIDUsage.keyboard9.rawValue:                 0x0A,
        UIKeyboardHIDUsage.keyboard0.rawValue:                 0x0B,
        UIKeyboardHIDUsage.keyboardHyphen.rawValue:            0x0C,   // -
        UIKeyboardHIDUsage.keyboardEqualSign.rawValue:         0x0D,   // ^ (JIS)
        UIKeyboardHIDUsage.keyboardInternational3.rawValue:    0x0E,   // ¥   [信頼度 M]
        UIKeyboardHIDUsage.keyboardDeleteOrBackspace.rawValue: 0x0F,   // BS

        // --- 上段 (InputManager.swift:478-485) ---
        UIKeyboardHIDUsage.keyboardTab.rawValue:               0x10,
        UIKeyboardHIDUsage.keyboardQ.rawValue:                 0x11,
        UIKeyboardHIDUsage.keyboardW.rawValue:                 0x12,
        UIKeyboardHIDUsage.keyboardE.rawValue:                 0x13,
        UIKeyboardHIDUsage.keyboardR.rawValue:                 0x14,
        UIKeyboardHIDUsage.keyboardT.rawValue:                 0x15,
        UIKeyboardHIDUsage.keyboardY.rawValue:                 0x16,
        UIKeyboardHIDUsage.keyboardU.rawValue:                 0x17,
        UIKeyboardHIDUsage.keyboardI.rawValue:                 0x18,
        UIKeyboardHIDUsage.keyboardO.rawValue:                 0x19,
        UIKeyboardHIDUsage.keyboardP.rawValue:                 0x1A,
        UIKeyboardHIDUsage.keyboardOpenBracket.rawValue:       0x1B,   // @ (JIS)
        UIKeyboardHIDUsage.keyboardCloseBracket.rawValue:      0x1C,   // [ (JIS)
        UIKeyboardHIDUsage.keyboardReturnOrEnter.rawValue:     0x1D,   // CR

        // --- 中段 (InputManager.swift:487-492) ---
        UIKeyboardHIDUsage.keyboardA.rawValue:                 0x1E,
        UIKeyboardHIDUsage.keyboardS.rawValue:                 0x1F,
        UIKeyboardHIDUsage.keyboardD.rawValue:                 0x20,
        UIKeyboardHIDUsage.keyboardF.rawValue:                 0x21,
        UIKeyboardHIDUsage.keyboardG.rawValue:                 0x22,
        UIKeyboardHIDUsage.keyboardH.rawValue:                 0x23,
        UIKeyboardHIDUsage.keyboardJ.rawValue:                 0x24,
        UIKeyboardHIDUsage.keyboardK.rawValue:                 0x25,
        UIKeyboardHIDUsage.keyboardL.rawValue:                 0x26,
        UIKeyboardHIDUsage.keyboardSemicolon.rawValue:         0x27,   // ;
        UIKeyboardHIDUsage.keyboardQuote.rawValue:             0x28,   // : (JIS)
        UIKeyboardHIDUsage.keyboardBackslash.rawValue:         0x29,   // ] (JIS)

        // --- 下段 (InputManager.swift:494-499) ---
        UIKeyboardHIDUsage.keyboardZ.rawValue:                 0x2A,
        UIKeyboardHIDUsage.keyboardX.rawValue:                 0x2B,
        UIKeyboardHIDUsage.keyboardC.rawValue:                 0x2C,
        UIKeyboardHIDUsage.keyboardV.rawValue:                 0x2D,
        UIKeyboardHIDUsage.keyboardB.rawValue:                 0x2E,
        UIKeyboardHIDUsage.keyboardN.rawValue:                 0x2F,
        UIKeyboardHIDUsage.keyboardM.rawValue:                 0x30,
        UIKeyboardHIDUsage.keyboardComma.rawValue:             0x31,   // ,
        UIKeyboardHIDUsage.keyboardPeriod.rawValue:            0x32,   // .
        UIKeyboardHIDUsage.keyboardSlash.rawValue:             0x33,   // /
        UIKeyboardHIDUsage.keyboardInternational1.rawValue:    0x34,   // _   [信頼度 M]
        UIKeyboardHIDUsage.keyboardSpacebar.rawValue:          0x35,   // SPACE

        // --- 編集キー (InputManager.swift:501-506) ---
        UIKeyboardHIDUsage.keyboardHome.rawValue:              0x36,   // HOME
        UIKeyboardHIDUsage.keyboardDeleteForward.rawValue:     0x37,   // DEL
        UIKeyboardHIDUsage.keyboardPageUp.rawValue:            0x38,   // ROLL UP
        UIKeyboardHIDUsage.keyboardPageDown.rawValue:          0x39,   // ROLL DOWN
        UIKeyboardHIDUsage.keyboardEnd.rawValue:               0x3A,   // UNDO
        UIKeyboardHIDUsage.keyboardEscape.rawValue:            0x01,   // ESC

        // --- カーソル (InputManager.swift:508-509) ---
        UIKeyboardHIDUsage.keyboardLeftArrow.rawValue:         0x3B,
        UIKeyboardHIDUsage.keyboardRightArrow.rawValue:        0x3D,
        UIKeyboardHIDUsage.keyboardUpArrow.rawValue:           0x3C,
        UIKeyboardHIDUsage.keyboardDownArrow.rawValue:         0x3E,

        // --- テンキー (InputManager.swift:511-522) ---
        UIKeyboardHIDUsage.keypadNumLock.rawValue:             0x3F,   // CLR [信頼度 M]
        UIKeyboardHIDUsage.keypadSlash.rawValue:               0x40,
        UIKeyboardHIDUsage.keypadAsterisk.rawValue:            0x41,
        UIKeyboardHIDUsage.keypadHyphen.rawValue:              0x42,
        UIKeyboardHIDUsage.keypad7.rawValue:                   0x43,
        UIKeyboardHIDUsage.keypad8.rawValue:                   0x44,
        UIKeyboardHIDUsage.keypad9.rawValue:                   0x45,
        UIKeyboardHIDUsage.keypadPlus.rawValue:                0x46,
        UIKeyboardHIDUsage.keypad4.rawValue:                   0x47,
        UIKeyboardHIDUsage.keypad5.rawValue:                   0x48,
        UIKeyboardHIDUsage.keypad6.rawValue:                   0x49,
        UIKeyboardHIDUsage.keypadEqualSign.rawValue:           0x4A,   // [信頼度 M]
        UIKeyboardHIDUsage.keypad1.rawValue:                   0x4B,
        UIKeyboardHIDUsage.keypad2.rawValue:                   0x4C,
        UIKeyboardHIDUsage.keypad3.rawValue:                   0x4D,
        UIKeyboardHIDUsage.keypadEnter.rawValue:               0x4E,
        UIKeyboardHIDUsage.keypad0.rawValue:                   0x4F,
        UIKeyboardHIDUsage.keypadComma.rawValue:               0x50,   // [信頼度 M]
        UIKeyboardHIDUsage.keypadPeriod.rawValue:              0x51,

        // --- ファンクション (InputManager.swift:524-532) ---
        UIKeyboardHIDUsage.keyboardF1.rawValue:                0x63,
        UIKeyboardHIDUsage.keyboardF2.rawValue:                0x64,
        UIKeyboardHIDUsage.keyboardF3.rawValue:                0x65,
        UIKeyboardHIDUsage.keyboardF4.rawValue:                0x66,
        UIKeyboardHIDUsage.keyboardF5.rawValue:                0x67,
        UIKeyboardHIDUsage.keyboardF6.rawValue:                0x55,   // XF1
        UIKeyboardHIDUsage.keyboardF7.rawValue:                0x56,   // XF2
        UIKeyboardHIDUsage.keyboardF8.rawValue:                0x57,   // XF3
        UIKeyboardHIDUsage.keyboardF9.rawValue:                0x58,   // XF4
        UIKeyboardHIDUsage.keyboardF10.rawValue:               0x59,   // XF5
        UIKeyboardHIDUsage.keyboardF11.rawValue:               0x72,   // OPT.1
        UIKeyboardHIDUsage.keyboardF12.rawValue:               0x73,   // OPT.2
        UIKeyboardHIDUsage.keyboardF13.rawValue:               0x54,   // HELP  [信頼度 M]
        UIKeyboardHIDUsage.keyboardF14.rawValue:               0x62,   // COPY  [信頼度 M]
        UIKeyboardHIDUsage.keyboardF15.rawValue:               0x61,   // BREAK [信頼度 M]

        // --- INS (InputManager.swift:533) ---
        // ★ここだけ **意図的に 2 つの usage が同一スキャンコード 0x5E へ写る**。
        //   解放時の保護は `handleUp` の汎用ガードが担う(§B-3 / R-3b / T-4c)。
        UIKeyboardHIDUsage.keyboardInsert.rawValue:            0x5E,   // [信頼度 M]
        UIKeyboardHIDUsage.keyboardHelp.rawValue:              0x5E,   // [信頼度 M]
    ]

    // MARK: - 保持状態

    /// usage.rawValue → **押下時に実際に送出したコード**。
    /// `InputManager.heldKeys`(`:572, 580-584`)の逐語移植。
    /// ★修飾キー(M1-M5)はここへ入らない。
    private var heldKeys: [Int: UInt8] = [:]

    /// 経路 A: 現在押されている修飾キーの HID usage → **同時押下数**。
    ///
    /// ★集合ではなく**カウンタ**である理由(T-4a の実測): この環境では
    ///   物理左 Shift と物理右 Shift が **どちらも同じ usage 0xE1**
    ///   (`.keyboardLeftShift`)で届き、`0xE5` は一度も現れない。
    ///   集合で持つと 2 キー同時押下でも要素は 1 個しか入らず、
    ///   片方を離した時点で唯一の要素が消えて SHIFT が誤って上がってしまう。
    ///   カウンタなら 2 キーとも離れるまで 0 にならない。
    ///   逆に L/R が別 usage で届く環境でも、usage ごとに独立した
    ///   カウンタになるだけで従来の集合と同じ挙動になる(回帰なし)。
    private var heldModifierKeys: [Int: Int] = [:]

    /// 最後にゲストへ送った合成状態(エッジ検出の基準)。
    /// `InputManager.lastModifiers` に相当する。
    private var shiftDown = false
    private var ctrlDown = false
    /// 経路 B の CapsLock (`.alphaShift`) エッジ検出用。macOS 側 `lastModifiers` と
    /// 同じく初期値は「立っていない」。
    private var lastAlphaShift = false

    // MARK: - イベント処理

    /// 受け取った `UIPress` のうち **自分が処理しなかったもの**を返す。
    /// 呼び出し側(`EmulatorMTKView_iOS`)はそれだけを `super.pressesXxx` へ転送する
    /// ——全部飲み込むと、キーイベントに依存する OS 側の挙動まで殺してしまう。
    @discardableResult
    func handle(phase: Phase, presses: Set<UIPress>) -> Set<UIPress> {
        // ★修飾キーを**先に**処理する。macOS では `flagsChanged` が `keyDown` より先に
        //   届くため、同じ順序を保たないと「SHIFT が上がる前に次の文字が入る」
        //   といった順序依存の食い違いが出る。
        processModifiers(phase: phase, presses: presses)

        var unhandled: Set<UIPress> = []

        for press in presses {
            guard let key = press.key else {
                // ゲームコントローラ由来など、キーを伴わない press。触らず素通しする。
                unhandled.insert(press)
                continue
            }
            let usage = key.keyCode.rawValue

            // 修飾キーは processModifiers() が消費済み。ここで二重に扱わない。
            if Self.isModifierUsage(usage) { continue }

            let handled: Bool
            switch phase {
            case .began:
                handled = handleDown(usage: usage)
                if !handled {
                    /* §自己反証可能性 (4): 常時の全件ログは撤去し、**表に無い usage が
                     * 届いたときだけ**残す。将来キーを増やすときの分母として安価に
                     * 有用で、通常操作ではログを一切汚さない。
                     * Release ビルドでは P679 により debug_log() ごと抑制される。 */
                    mx68k_log(String(format:
                        "[Swift][iOS][P705-KEYUNMAPPED] usage=0x%02X mods=0x%02X",
                        usage, key.modifierFlags.rawValue))
                }
            case .ended, .cancelled:
                handleUp(usage: usage)
                // 表に載っているキーなら「処理した」。載っていなければ super へ回す
                // (down 側で 1 行ログ済みなので、up 側では重ねてログしない)。
                handled = Self.keyMap[usage] != nil
            }

            if !handled { unhandled.insert(press) }
        }

        return unhandled
    }

    /// `InputManager.handleKeyDown`(`:569-575`)の逐語移植 + キーリピート抑止。
    @discardableResult
    private func handleDown(usage: Int) -> Bool {
        guard let code = Self.keyMap[usage] else { return false }
        // B-6 / T-0d: iOS は保持中も began を反復配送する。ゲストへ重複した
        // key_down を送らないよう、既に保持中の usage は無視する。
        if heldKeys[usage] != nil { return true }
        heldKeys[usage] = code
        mx68k_key_down(code)
        return true
    }

    /// `InputManager.handleKeyUp`(`:577-585`)の逐語移植。
    @discardableResult
    private func handleUp(usage: Int) -> Bool {
        // 押下時に送出したコードで解放する(現在の割当を引き直さない)。
        guard let code = heldKeys.removeValue(forKey: usage) else { return false }
        /* ★同じコードを別の物理キーがまだ保持している場合は解放しない
         *   (`InputManager.swift:582` の逐語移植)。
         *   本テーブルでこのガードが実際に効くのは **0x5E (INS) の 1 件だけ**
         *   —— `.keyboardInsert` と `.keyboardHelp` の両方を 0x5E へ写す、
         *   本サイクル自身の設計選択が原因で必要になる(§B-3 / R-3b)。
         *   落とすと「Insert を離した瞬間、Help を押したままなのに
         *   ゲスト側で INS が上がる」誤動作になる。
         *   ★L/R Shift・L/R Ctrl の重複解放抑止をここへ足してはならない
         *     —— それは processModifiers() の OR 合成が担う(二重実装の禁止)。 */
        guard !heldKeys.values.contains(code) else { return true }
        mx68k_key_up(code)
        return true
    }

    // MARK: - 修飾キー(二重経路 + OR 合成 + 状態遷移エッジ送出)
    //
    // P705 計画 §変更内容 B-4。`InputManager.handleFlagsChanged`(`:587-606`)の
    // エッジ検出規約をそのまま写しつつ、iOS 固有の未知数
    // (「単独修飾キーが自身の keyCode で届くか」)へ依存しない形にしてある。
    //
    //   経路 A … `.keyboardLeftShift` 等が `pressesBegan/Ended` で届いたら、
    //            その**物理キー**の保持状態として `heldModifierKeys` へ記録する。
    //   経路 B … そのイベントに含まれる `UIKey.modifierFlags` の和から
    //            `.shift` / `.control` / `.alphaShift` を読む。
    //   合成  … desired = (経路 A のいずれかが保持中) || (経路 B のビットが立っている)。
    //            **desired が前回から変化したときだけ** key_down / key_up を送る。
    //
    // → 経路 A だけが動く環境でも、経路 B だけが動く環境でも、両方動く環境でも、
    //   ゲストへ届く key_down / key_up は各 1 回になる。
    //   「左 Shift を離しても右 Shift を押している間は SHIFT を上げない」保証も、
    //   `desiredShift` が OR でtrue のままエッジが立たないことで与えられる。

    private func processModifiers(phase: Phase, presses: Set<UIPress>) {
        var sawKey = false
        var mods: UIKeyModifierFlags = []
        var capsKeyInEvent = false
        var capsBeganViaKeyCode = false

        for press in presses {
            guard let key = press.key else { continue }
            sawKey = true
            let usage = key.keyCode.rawValue

            /* ★経路 B の集計から「今まさに解放されつつある修飾キー自身」の
             *   modifierFlags を除外する。UIKit が解放イベントでも当の修飾ビットを
             *   立てたまま報告する場合、素直に OR を取ると desired が false へ落ちず、
             *   SHIFT/CTRL がゲスト側に残ってしまう。ビットが既にクリアされている
             *   環境ではこの除外は何も変えない(除外して悪くなる方向が無い)。
             *   除外しても、他の修飾キーの保持は経路 A が保っているので情報は落ちない。 */
            let selfRelease = (phase != .began) && Self.isModifierUsage(usage)
            if !selfRelease { mods.formUnion(key.modifierFlags) }

            if Self.shiftUsages.contains(usage) || Self.controlUsages.contains(usage) {
                if phase == .began {
                    heldModifierKeys[usage, default: 0] += 1
                } else {
                    // ★0 で床止めする —— 重複した解放イベントが届いても
                    //   負値にして以後の状態を壊さない。
                    let n = (heldModifierKeys[usage] ?? 0) - 1
                    if n > 0 { heldModifierKeys[usage] = n }
                    else { heldModifierKeys.removeValue(forKey: usage) }
                }
            } else if usage == Self.capsLockUsage {
                capsKeyInEvent = true
                if phase == .began { capsBeganViaKeyCode = true }
            }
        }

        // キーを 1 つも伴わないイベントで状態を作り直さない(経路 B の分母が無い)。
        guard sawKey else { return }

        let desiredShift = Self.shiftUsages.contains { (heldModifierKeys[$0] ?? 0) > 0 }
                        || mods.contains(.shift)
        let desiredCtrl  = Self.controlUsages.contains { (heldModifierKeys[$0] ?? 0) > 0 }
                        || mods.contains(.control)

        if desiredShift != shiftDown {
            shiftDown = desiredShift
            if desiredShift { mx68k_key_down(Self.scShift) } else { mx68k_key_up(Self.scShift) }
        }
        if desiredCtrl != ctrlDown {
            ctrlDown = desiredCtrl
            if desiredCtrl { mx68k_key_down(Self.scCtrl) } else { mx68k_key_up(Self.scCtrl) }
        }

        /* CAPS (0x5D) は **down+up のパルス**(保持しない)。macOS `:600-603` と同一規約。
         * 1 回の物理押下に対してパルスは **ちょうど 1 回**でなければならない
         * —— X68000 の CAPS はトグルなので、2 回送ると差し引きゼロになる。
         * そのため:
         *   - このイベントに CapsLock キー自身が含まれる場合は **経路 A が権威**。
         *     began でのみパルスし、`lastAlphaShift` は同期するだけでトリガしない
         *     (began と ended で `.alphaShift` の値が変わっても二重に打たない)。
         *   - CapsLock の keyCode がそもそも届かない環境でのみ、経路 B の
         *     `.alphaShift` 変化がパルスの契機になる。 */
        let alphaShift = mods.contains(.alphaShift)
        if capsKeyInEvent {
            lastAlphaShift = alphaShift
            if capsBeganViaKeyCode { pulseCaps() }
        } else if alphaShift != lastAlphaShift {
            lastAlphaShift = alphaShift
            pulseCaps()
        }
    }

    private func pulseCaps() {
        mx68k_key_down(Self.scCaps)
        mx68k_key_up(Self.scCaps)
    }

    // MARK: - 全解放

    /// 保持中の全キー(通常キー + 修飾状態)へ `mx68k_key_up` を送り、内部状態を空にする。
    ///
    /// **iOS 固有の失敗モードへの対処**であり、macOS に対応物が無い新規機能である
    /// (P705 計画 §変更内容 B-5)。T-0e の実測により
    /// `pressesCancelled` / `resignFirstResponder` はバックグラウンド遷移で
    /// **発火しないことが確認された**ため、主たる呼び出し元は
    /// `EmulatorMTKView_iOS` が観測する `UIApplication.didEnterBackgroundNotification`
    /// である(この 2 つの UIKit 経路は補助として残してある)。
    ///
    /// ★呼び出しはメインスレッドから行うこと(§B-7)。
    func releaseAll(reason: String) {
        /* 複数の usage が同一スキャンコードへ写り得る(0x5E)ので、
         * コードの集合へ畳んでから 1 回ずつ up する。 */
        let codes = Set(heldKeys.values)
        heldKeys.removeAll()
        for code in codes { mx68k_key_up(code) }

        // ★ログには**解放前**の状態を出す(解放後は常に 0 になり、証拠にならない)。
        let hadShift = shiftDown
        let hadCtrl = ctrlDown
        heldModifierKeys.removeAll()
        if shiftDown { shiftDown = false; mx68k_key_up(Self.scShift) }
        if ctrlDown { ctrlDown = false; mx68k_key_up(Self.scCtrl) }
        /* `lastAlphaShift` は**リセットしない** —— false へ戻すと、復帰後の最初の
         * キーイベントで `.alphaShift` が立っていた場合に偽のパルスが出る。
         * CAPS は保持状態を持たないので、ここで送るものは無い。 */

        /* T-4b(R-4)の判定材料。全件ログではなくバックグラウンド遷移等でのみ出るので、
         * 通常操作でログを汚さない。released=0 でも「フックが発火した」ことの証拠に
         * なるため、解放対象が無くても 1 行出す。 */
        mx68k_log("[Swift][iOS][P705-KEYRELEASE] reason=\(reason) "
                + "keys=\(codes.count) shift=\(hadShift ? 1 : 0) ctrl=\(hadCtrl ? 1 : 0)")
    }
}
