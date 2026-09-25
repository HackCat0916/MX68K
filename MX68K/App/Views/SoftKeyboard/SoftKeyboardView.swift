//
//  SoftKeyboardView.swift
//  MX68K
//
//  P228 — 画面上のソフトウェアキーボード(JIS配列の完全再現、独立ウィンドウ、
//  ゲストLEDの反映)。Phase 2 完了条件(6)の最終項目。
//
//  設計(.mx68k_cycles/P228_plan.md 参照):
//  このビューは InputManager の heldKeys/keyboardState による状態管理とは
//  完全に分離されている。ゲストとのやり取りは C ブリッジ呼び出し
//  mx68k_key_down / mx68k_key_up / mx68k_get_key_led のみを通じて行うため、
//  物理キーボード経路とソフトキーボード経路が Swift 側の状態を共有することはない。
//
//  キーは次の3種類に分類される:
//   (A) スティッキー・モーメンタリ — SHIFT/CTRL/OPT.1/OPT.2。ホスト側の利便性トグル:
//       1回目のクリック = key_down(押下保持)、2回目のクリック = key_up。
//       ローカルの bool 状態で管理する。
//       (実機ではこれらのキーに LED は無い——これは意図的かつユーザー承認済みの、
//       実機に忠実ではない利便機能である。Docs/05 §2.2 参照。)
//   (B) パルス + ゲストLED反映 — かな/ローマ字/コード入力/CAPS/INS/ひらがな/全角。
//       クリックのたびに down+up のパルスを送る。ローカルのトグル状態は持たない——
//       LED 点灯表示は、mx68k_get_key_led() のポーリング結果のみによって決まる。
//   通常キー — DragGesture によるモーメンタリな押下/解放(最初の変化で down、
//       終了時に up)。
//

import SwiftUI

#if os(iOS)
import UIKit   // P710: Color(uiColor:) が参照する UIColor のため。macOS の NSColor の
               // 使い方のように SwiftUI の暗黙の再エクスポートに頼らないこと。
#endif

// MARK: - レイアウトデータモデル

/// JIS配列再現キーボード上の1キー。座標/ラベル/スキャンコードは px68k の
/// 正式な `x11/keytbl.inc` からそのまま転記したもの——変更しないこと。
struct SoftKeyDef {
    let x: CGFloat
    let y: CGFloat
    let w: CGFloat
    let h: CGFloat
    let label: String
    let scancode: UInt8
    /// `label` の代わりに描画する SF Symbol 名(RET + カーソルキー)。既定値は nil。
    /// (Swift が合成するメンバーワイズイニシャライザはこのオプショナルの既定値を
    ///  保持するため、これを省略している既存の109エントリはそのままコンパイルできる。)
    var iconName: String? = nil
    /// 1行ラベルのフォントサイズ(pt)の明示的な上書き指定。nil でない場合は
    /// 幅/高さから算出される自動サイズより優先され、ラベル長の異なる隣接キー
    /// (HOME/INS、BREAK/COPY)の見た目を揃えるのに使う。既定値は nil。
    var fixedFontSize: CGFloat? = nil
}

enum SoftKeyKind {
    case sticky      // SHIFT / CTRL / OPT.1 / OPT.2 — ローカルの押下保持トグル
    case ledPulse    // 7つのロックキー — パルス + ゲストLED反映
    case normal      // それ以外すべて — モーメンタリ
}

extension SoftKeyDef {
    var kind: SoftKeyKind {
        switch scancode {
        case 0x70, 0x71, 0x72, 0x73:
            return .sticky
        case 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60:
            return .ledPulse
        default:
            return .normal
        }
    }

    /// 7つのロックキーに対応する LED のビット番号(keyLED バイトは負論理)。それ以外は nil。
    /// bit0=かな bit1=ローマ字 bit2=コード入力 bit3=CAPS bit4=INS bit5=ひらがな bit6=全角
    var ledBit: Int? {
        switch scancode {
        case 0x5a: return 0
        case 0x5b: return 1
        case 0x5c: return 2
        case 0x5d: return 3
        case 0x5e: return 4
        case 0x5f: return 5
        case 0x60: return 6
        default:   return nil
        }
    }

    /// 実機のロックLEDの色(spec-confirm ゲート #4、ユーザーによる実機写真
    /// 2026-07-18): かな/ローマ字/コード入力/CAPS/INS は赤、ひらがな/全角は緑に
    /// 点灯する。`.ledPulse` キーでのみ意味を持ち、それ以外は nil。
    var ledColor: Color? {
        switch scancode {
        case 0x5a, 0x5b, 0x5c, 0x5d, 0x5e: return .red
        case 0x5f, 0x60:                   return .green
        default:                           return nil
        }
    }
}

/// キャンバスは約 766 × 218 pt。原点は左上で、各キーは (x + w/2, y + h/2) に
/// `.position` で配置する。値は `x11/keytbl.inc` からそのまま転記したもの。
/// (唯一の `scancode == 0` のスペーサーエントリはキーではなくレイアウト上の隙間で、
///  この表には含めていない——念のため、防御的フィルタでもそのようなエントリを除外する。)
let softKeyLayout: [SoftKeyDef] = [
    SoftKeyDef(x:   2, y:   2, w: 32, h: 32, label: "BREAK", scancode: 0x61, fixedFontSize: 9),
    SoftKeyDef(x:  40, y:   2, w: 32, h: 32, label: "COPY",  scancode: 0x62, fixedFontSize: 9),
    SoftKeyDef(x:  86, y:  16, w: 40, h: 18, label: "F 1",   scancode: 0x63),
    SoftKeyDef(x: 128, y:  16, w: 40, h: 18, label: "F 2",   scancode: 0x64),
    SoftKeyDef(x: 170, y:  16, w: 40, h: 18, label: "F 3",   scancode: 0x65),
    SoftKeyDef(x: 212, y:  16, w: 40, h: 18, label: "F 4",   scancode: 0x66),
    SoftKeyDef(x: 254, y:  16, w: 40, h: 18, label: "F 5",   scancode: 0x67),
    SoftKeyDef(x: 302, y:  16, w: 40, h: 18, label: "F 6",   scancode: 0x68),
    SoftKeyDef(x: 344, y:  16, w: 40, h: 18, label: "F 7",   scancode: 0x69),
    SoftKeyDef(x: 386, y:  16, w: 40, h: 18, label: "F 8",   scancode: 0x6a),
    SoftKeyDef(x: 428, y:  16, w: 40, h: 18, label: "F 9",   scancode: 0x6b),
    SoftKeyDef(x: 470, y:  16, w: 40, h: 18, label: "F10",   scancode: 0x6c),
    SoftKeyDef(x: 520, y:   2, w: 32, h: 32, label: "かな",   scancode: 0x5a),
    SoftKeyDef(x: 554, y:   2, w: 32, h: 32, label: "ローマ字", scancode: 0x5b),
    SoftKeyDef(x: 588, y:   2, w: 32, h: 32, label: "コード入力", scancode: 0x5c),
    SoftKeyDef(x: 630, y:   2, w: 32, h: 32, label: "CAPS",  scancode: 0x5d),
    SoftKeyDef(x: 664, y:   2, w: 32, h: 32, label: "記号\n入力", scancode: 0x52),
    SoftKeyDef(x: 698, y:   2, w: 32, h: 32, label: "登録",   scancode: 0x53),
    SoftKeyDef(x: 732, y:   2, w: 32, h: 32, label: "HELP",  scancode: 0x54),
    SoftKeyDef(x:   2, y:  48, w: 32, h: 32, label: "ESC",   scancode: 0x01),
    SoftKeyDef(x:  36, y:  48, w: 32, h: 32, label: "1",     scancode: 0x02),
    SoftKeyDef(x:  70, y:  48, w: 32, h: 32, label: "2",     scancode: 0x03),
    SoftKeyDef(x: 104, y:  48, w: 32, h: 32, label: "3",     scancode: 0x04),
    SoftKeyDef(x: 138, y:  48, w: 32, h: 32, label: "4",     scancode: 0x05),
    SoftKeyDef(x: 172, y:  48, w: 32, h: 32, label: "5",     scancode: 0x06),
    SoftKeyDef(x: 206, y:  48, w: 32, h: 32, label: "6",     scancode: 0x07),
    SoftKeyDef(x: 240, y:  48, w: 32, h: 32, label: "7",     scancode: 0x08),
    SoftKeyDef(x: 274, y:  48, w: 32, h: 32, label: "8",     scancode: 0x09),
    SoftKeyDef(x: 308, y:  48, w: 32, h: 32, label: "9",     scancode: 0x0a),
    SoftKeyDef(x: 342, y:  48, w: 32, h: 32, label: "0",     scancode: 0x0b),
    SoftKeyDef(x: 376, y:  48, w: 32, h: 32, label: "-",     scancode: 0x0c),
    SoftKeyDef(x: 410, y:  48, w: 32, h: 32, label: "^",     scancode: 0x0d),
    SoftKeyDef(x: 444, y:  48, w: 32, h: 32, label: "¥",     scancode: 0x0e),  // ¥ キー
    SoftKeyDef(x: 478, y:  48, w: 32, h: 32, label: "BS",    scancode: 0x0f),
    SoftKeyDef(x: 520, y:  48, w: 32, h: 32, label: "HOME",  scancode: 0x36, fixedFontSize: 10),
    SoftKeyDef(x: 554, y:  48, w: 32, h: 32, label: "INS",   scancode: 0x5e, fixedFontSize: 10),
    SoftKeyDef(x: 588, y:  48, w: 32, h: 32, label: "DEL",   scancode: 0x37, fixedFontSize: 10),
    SoftKeyDef(x: 630, y:  48, w: 32, h: 32, label: "CLR",   scancode: 0x3f),
    SoftKeyDef(x: 664, y:  48, w: 32, h: 32, label: "/",     scancode: 0x40),
    SoftKeyDef(x: 698, y:  48, w: 32, h: 32, label: "*",     scancode: 0x41),
    SoftKeyDef(x: 732, y:  48, w: 32, h: 32, label: "-",     scancode: 0x42),
    SoftKeyDef(x:   2, y:  82, w: 44, h: 32, label: "TAB",   scancode: 0x10),
    SoftKeyDef(x:  48, y:  82, w: 32, h: 32, label: "Q",     scancode: 0x11),
    SoftKeyDef(x:  82, y:  82, w: 32, h: 32, label: "W",     scancode: 0x12),
    SoftKeyDef(x: 116, y:  82, w: 32, h: 32, label: "E",     scancode: 0x13),
    SoftKeyDef(x: 150, y:  82, w: 32, h: 32, label: "R",     scancode: 0x14),
    SoftKeyDef(x: 184, y:  82, w: 32, h: 32, label: "T",     scancode: 0x15),
    SoftKeyDef(x: 218, y:  82, w: 32, h: 32, label: "Y",     scancode: 0x16),
    SoftKeyDef(x: 252, y:  82, w: 32, h: 32, label: "U",     scancode: 0x17),
    SoftKeyDef(x: 286, y:  82, w: 32, h: 32, label: "I",     scancode: 0x18),
    SoftKeyDef(x: 320, y:  82, w: 32, h: 32, label: "O",     scancode: 0x19),
    SoftKeyDef(x: 354, y:  82, w: 32, h: 32, label: "P",     scancode: 0x1a),
    SoftKeyDef(x: 388, y:  82, w: 32, h: 32, label: "@",     scancode: 0x1b),
    SoftKeyDef(x: 422, y:  82, w: 32, h: 32, label: "[",     scancode: 0x1c),
    SoftKeyDef(x: 468, y:  82, w: 42, h: 66, label: "RET",   scancode: 0x1d, iconName: "arrow.turn.down.left"),
    SoftKeyDef(x: 520, y:  82, w: 32, h: 32, label: "ROLL\nUP", scancode: 0x38),
    SoftKeyDef(x: 554, y:  82, w: 32, h: 32, label: "ROLL\nDOWN", scancode: 0x39),
    SoftKeyDef(x: 588, y:  82, w: 32, h: 32, label: "UNDO",  scancode: 0x3a),
    SoftKeyDef(x: 630, y:  82, w: 32, h: 32, label: "7",     scancode: 0x43),
    SoftKeyDef(x: 664, y:  82, w: 32, h: 32, label: "8",     scancode: 0x44),
    SoftKeyDef(x: 698, y:  82, w: 32, h: 32, label: "9",     scancode: 0x45),
    SoftKeyDef(x: 732, y:  82, w: 32, h: 32, label: "+",     scancode: 0x46),
    SoftKeyDef(x:   2, y: 116, w: 56, h: 32, label: "CTRL",  scancode: 0x71),
    SoftKeyDef(x:  60, y: 116, w: 32, h: 32, label: "A",     scancode: 0x1e),
    SoftKeyDef(x:  94, y: 116, w: 32, h: 32, label: "S",     scancode: 0x1f),
    SoftKeyDef(x: 128, y: 116, w: 32, h: 32, label: "D",     scancode: 0x20),
    SoftKeyDef(x: 162, y: 116, w: 32, h: 32, label: "F",     scancode: 0x21),
    SoftKeyDef(x: 196, y: 116, w: 32, h: 32, label: "G",     scancode: 0x22),
    SoftKeyDef(x: 230, y: 116, w: 32, h: 32, label: "H",     scancode: 0x23),
    SoftKeyDef(x: 264, y: 116, w: 32, h: 32, label: "J",     scancode: 0x24),
    SoftKeyDef(x: 298, y: 116, w: 32, h: 32, label: "K",     scancode: 0x25),
    SoftKeyDef(x: 332, y: 116, w: 32, h: 32, label: "L",     scancode: 0x26),
    SoftKeyDef(x: 366, y: 116, w: 32, h: 32, label: ";",     scancode: 0x27),
    SoftKeyDef(x: 400, y: 116, w: 32, h: 32, label: ":",     scancode: 0x28),
    SoftKeyDef(x: 434, y: 116, w: 32, h: 32, label: "]",     scancode: 0x29),
    SoftKeyDef(x: 554, y: 116, w: 32, h: 32, label: "up",    scancode: 0x3c, iconName: "arrow.up"),
    SoftKeyDef(x: 630, y: 116, w: 32, h: 32, label: "4",     scancode: 0x47),
    SoftKeyDef(x: 664, y: 116, w: 32, h: 32, label: "5",     scancode: 0x48),
    SoftKeyDef(x: 698, y: 116, w: 32, h: 32, label: "6",     scancode: 0x49),
    SoftKeyDef(x: 732, y: 116, w: 32, h: 32, label: "=",     scancode: 0x4a),
    SoftKeyDef(x:   2, y: 150, w: 68, h: 32, label: "SHIFT", scancode: 0x70),
    SoftKeyDef(x:  72, y: 150, w: 32, h: 32, label: "Z",     scancode: 0x2a),
    SoftKeyDef(x: 106, y: 150, w: 32, h: 32, label: "X",     scancode: 0x2b),
    SoftKeyDef(x: 140, y: 150, w: 32, h: 32, label: "C",     scancode: 0x2c),
    SoftKeyDef(x: 174, y: 150, w: 32, h: 32, label: "V",     scancode: 0x2d),
    SoftKeyDef(x: 208, y: 150, w: 32, h: 32, label: "B",     scancode: 0x2e),
    SoftKeyDef(x: 242, y: 150, w: 32, h: 32, label: "N",     scancode: 0x2f),
    SoftKeyDef(x: 276, y: 150, w: 32, h: 32, label: "M",     scancode: 0x30),
    SoftKeyDef(x: 310, y: 150, w: 32, h: 32, label: ",",     scancode: 0x31),
    SoftKeyDef(x: 344, y: 150, w: 32, h: 32, label: ".",     scancode: 0x32),
    SoftKeyDef(x: 378, y: 150, w: 32, h: 32, label: "/",     scancode: 0x33),
    SoftKeyDef(x: 412, y: 150, w: 32, h: 32, label: " ",     scancode: 0x34),  // ろ / アンダースコアキー
    SoftKeyDef(x: 446, y: 150, w: 64, h: 32, label: "SHIFT", scancode: 0x70),
    SoftKeyDef(x: 520, y: 132, w: 32, h: 32, label: "left",  scancode: 0x3b, iconName: "arrow.left"),
    SoftKeyDef(x: 554, y: 150, w: 32, h: 32, label: "down",  scancode: 0x3e, iconName: "arrow.down"),
    SoftKeyDef(x: 588, y: 132, w: 32, h: 32, label: "rigt",  scancode: 0x3d, iconName: "arrow.right"),
    SoftKeyDef(x: 630, y: 150, w: 32, h: 32, label: "1",     scancode: 0x4b),
    SoftKeyDef(x: 664, y: 150, w: 32, h: 32, label: "2",     scancode: 0x4c),
    SoftKeyDef(x: 698, y: 150, w: 32, h: 32, label: "3",     scancode: 0x4d),
    SoftKeyDef(x: 732, y: 150, w: 32, h: 66, label: "ENTER", scancode: 0x4e),
    SoftKeyDef(x:  60, y: 184, w: 32, h: 32, label: "ひらがな", scancode: 0x5f),
    SoftKeyDef(x:  94, y: 184, w: 38, h: 32, label: "XF1",   scancode: 0x55),
    SoftKeyDef(x: 134, y: 184, w: 38, h: 32, label: "XF2",   scancode: 0x56),
    SoftKeyDef(x: 174, y: 184, w: 128, h: 32, label: "",     scancode: 0x35),  // スペースキー(ラベルは空白だが実在するキー)
    SoftKeyDef(x: 304, y: 184, w: 38, h: 32, label: "XF3",   scancode: 0x57),
    SoftKeyDef(x: 344, y: 184, w: 42, h: 32, label: "XF4",   scancode: 0x58),
    SoftKeyDef(x: 388, y: 184, w: 42, h: 32, label: "XF5",   scancode: 0x59),
    SoftKeyDef(x: 432, y: 184, w: 32, h: 32, label: "全角",   scancode: 0x60),
    SoftKeyDef(x: 520, y: 184, w: 48, h: 32, label: "OPT.1", scancode: 0x72),
    SoftKeyDef(x: 572, y: 184, w: 48, h: 32, label: "OPT.2", scancode: 0x73),
    SoftKeyDef(x: 630, y: 184, w: 32, h: 32, label: "0",     scancode: 0x4f),
    SoftKeyDef(x: 664, y: 184, w: 32, h: 32, label: ",",     scancode: 0x50),
    SoftKeyDef(x: 698, y: 184, w: 32, h: 32, label: ".",     scancode: 0x51),
]

/// SHIFT ON 時の表示文字の上書き。計画書の SHIFT 文字表(21エントリ)から
/// そのまま転記したもの。この表に無いキー(英字、ファンクションキー、編集キー群など)は
/// 固定ラベルのまま——JIS キートップは見た目上、英字の大文字/小文字を変えない。
/// 0x0b (0) は意図的に含めていない(SHIFT 時も変化しない)。
let shiftGlyphs: [UInt8: String] = [
    0x02: "!",
    0x03: "\"",
    0x04: "#",
    0x05: "$",
    0x06: "%",
    0x07: "&",
    0x08: "'",
    0x09: "(",
    0x0a: ")",
    0x0c: "=",
    0x0d: "~",
    0x0e: "|",
    0x1b: "`",
    0x1c: "{",
    0x27: "+",
    0x28: "*",
    0x29: "}",
    0x31: "<",
    0x32: ">",
    0x33: "?",
    0x34: "_",
]

/// かな ON 時の表示文字の上書き(標準的な JIS かな入力配列)。計画書のかな表
/// (48エントリ: 数字段13 + QWERTY段12 + ASDF段12 + ZXCV段11)からそのまま転記したもの。
/// ゲストの かな ロックLED が点灯している時(guestLED の bit0 がクリア、負論理)に
/// 表示され、shiftGlyphs より優先される。
/// @/[ は濁点゛/半濁点゜の記号単体を表示するのみで、実際のかな合成は行わない。
let kanaGlyphs: [UInt8: String] = [
    // 数字段 (13)
    0x02: "ぬ", 0x03: "ふ", 0x04: "あ", 0x05: "う", 0x06: "え",
    0x07: "お", 0x08: "や", 0x09: "ゆ", 0x0a: "よ", 0x0b: "わ",
    0x0c: "ほ", 0x0d: "へ", 0x0e: "ー",
    // QWERTY段 (12)
    0x11: "た", 0x12: "て", 0x13: "い", 0x14: "す", 0x15: "か",
    0x16: "ん", 0x17: "な", 0x18: "に", 0x19: "ら", 0x1a: "せ",
    0x1b: "゛", 0x1c: "゜",
    // ASDF段 (12)
    0x1e: "ち", 0x1f: "と", 0x20: "し", 0x21: "は", 0x22: "き",
    0x23: "く", 0x24: "ま", 0x25: "の", 0x26: "り", 0x27: "れ",
    0x28: "け", 0x29: "む",
    // ZXCV段 (11)
    0x2a: "つ", 0x2b: "さ", 0x2c: "そ", 0x2d: "ひ", 0x2e: "こ",
    0x2f: "み", 0x30: "も", 0x31: "ね", 0x32: "る", 0x33: "め",
    0x34: "ろ",
]

// MARK: - 状態

/// ソフトキーボード専用の軽量な状態オブジェクト。InputManager の heldKeys /
/// keyboardState には意図的に一切触れない——物理キーボードとソフトキーボードの
/// 経路は独立している(Docs/05 §2.2 の既知の制限事項の注記を参照)。
final class SoftKeyboardState: ObservableObject {
    // (A) スティッキーキーの押下保持状態 — 4キー。
    @Published var shiftHeld = false
    @Published var ctrlHeld  = false
    @Published var opt1Held  = false
    @Published var opt2Held  = false

    // (B) ゲストの LED バイト(負論理。アイドル時 0xFF = 全消灯)。表示中はポーリングする。
    @Published var guestLED: UInt8 = 0xFF

    private var ledTimer: Timer?

    // MARK: スティッキー

    func isStickyOn(_ scancode: UInt8) -> Bool {
        switch scancode {
        case 0x70: return shiftHeld
        case 0x71: return ctrlHeld
        case 0x72: return opt1Held
        case 0x73: return opt2Held
        default:   return false
        }
    }

    /// 1回目のクリックで key_down(押下保持)、2回目のクリックで key_up を送る。
    func toggleSticky(_ scancode: UInt8) {
        let nowOn: Bool
        switch scancode {
        case 0x70: shiftHeld.toggle(); nowOn = shiftHeld
        case 0x71: ctrlHeld.toggle();  nowOn = ctrlHeld
        case 0x72: opt1Held.toggle();  nowOn = opt1Held
        case 0x73: opt2Held.toggle();  nowOn = opt2Held
        default:   return
        }
        if nowOn {
            mx68k_key_down(scancode)   // 押下保持を続ける(対応する up はまだ送らない)
        } else {
            mx68k_key_up(scancode)
        }
    }

    // MARK: パルス(B 分類のキー)

    /// ロックキーをクリックするたびに down+up のパルスを送る。結果のトグル状態は
    /// ゲストが決定し、keyLED を通じて報告してくる。
    func pulse(_ scancode: UInt8) {
        mx68k_key_down(scancode)
        mx68k_key_up(scancode)
    }

    // MARK: LED ポーリング

    func startLEDPolling() {
        stopLEDPolling()
        guestLED = mx68k_get_key_led()   // ウィンドウを開いた時点で同期しているよう即座に読む
        ledTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
            self?.guestLED = mx68k_get_key_led()
        }
    }

    func stopLEDPolling() {
        ledTimer?.invalidate()
        ledTimer = nil
    }

    // MARK: 強制解放(P228 レビュー第1ラウンドで必須とされた追加)

    /// ウィンドウを閉じる時点でまだ押下保持中のスティッキーキーを強制解放する。
    /// P227 stopGamepadMonitoring() / P229 releaseAutoPause() のアイドル時解放と
    /// 同じ考え方で、ウィンドウが消えた後に SHIFT/CTRL/OPT がゲスト側で押されたまま
    /// 残らないようにする。
    func releaseAllSticky() {
        if shiftHeld { mx68k_key_up(0x70); shiftHeld = false }
        if ctrlHeld  { mx68k_key_up(0x71); ctrlHeld  = false }
        if opt1Held  { mx68k_key_up(0x72); opt1Held  = false }
        if opt2Held  { mx68k_key_up(0x73); opt2Held  = false }
    }
}

// MARK: - 個々のキーボタン

private struct SoftKeyButton: View {
    let key: SoftKeyDef
    @ObservedObject var state: SoftKeyboardState

    /// ジェスチャー単位のローカルなガード。DragGesture の `.onChanged` が繰り返し
    /// 発火しても、1回の押下につき key_down を1回だけ送るようにする(通常キーのみ)。
    @State private var isDown = false

    private var highlighted: Bool {
        switch key.kind {
        case .sticky:
            return state.isStickyOn(key.scancode)
        case .ledPulse:
            guard let bit = key.ledBit else { return false }
            return (state.guestLED & (UInt8(1) << bit)) == 0   // 負論理: 0 = 点灯
        case .normal:
            return isDown
        }
    }

    private var displayLabel: String {
        // かなモードの表示文字が最優先: ゲストの かな ロックLED が点灯している時
        // (guestLED の bit0 がクリア、負論理)は JIS かな文字を表示する。SHIFT/小文字の
        // 判定より「前に」実行するため、かな有効中はかな表示が優先される。かな が
        // オフ(bit0 == 1)の時は、そのまま下の判定へ素直に抜ける。
        if (state.guestLED & 0x01) == 0, let k = kanaGlyphs[key.scancode] {
            return k
        }
        // 記号/数字キーは、ローカルの SHIFT が ON の時に SHIFT 時の文字を表示する。
        if state.shiftHeld, let g = shiftGlyphs[key.scancode] {
            return g
        }
        // 英字キーは SHIFT を押下保持するまで小文字で表示する(JIS キートップは大文字
        // 表記だが、実際に入力される文字に合わせている)。shiftGlyphs は記号/数字の
        // スキャンコードしか持たないので、英字が上の分岐と衝突することはない。
        if !state.shiftHeld,
           key.label.count == 1,
           let c = key.label.first, c.isLetter, c.isUppercase {
            return key.label.lowercased()
        }
        return key.label
    }

    /// 1行テキストラベルのフォントサイズ。優先順位: キーごとの明示的な上書き指定 >
    /// 2行ラベル用の固定 9pt(P232)> 幅/高さから算出する自動サイズ。自動サイズは
    /// min(w, h) を使うため、縦長で幅の狭いキー(ENTER、32×66)も、高さで膨らんだ
    /// サイズ(その後 .minimumScaleFactor で切り詰められてしまう)ではなく、同じ幅の
    /// 正方形キー(コード入力、32×32)と同じ基準サイズになる。
    private var fontSize: CGFloat {
        if let fixed = key.fixedFontSize { return fixed }
        if displayLabel.contains("\n") { return 9 }
        return max(8, min(key.w, key.h) * 0.36)
    }

    /// キートップに表示する図柄: SF Symbol アイコン(RET + カーソルキー)またはテキスト
    /// ラベル。どちらも同じハイライト時の前景色を共有する。アイコン側の分岐は高さから
    /// 算出する独自のサイズを維持する(アイコンキーはすべて正方形なので変化なし)。
    @ViewBuilder private var keyContent: some View {
        if let icon = key.iconName {
            Image(systemName: icon)
                .font(.system(size: max(8, key.h * 0.36)))
                .foregroundColor(highlighted ? Color.white : Color.primary)
        } else {
            Text(displayLabel)
                .font(.system(size: fontSize, weight: .medium))
                .foregroundColor(highlighted ? Color.white : Color.primary)
                .minimumScaleFactor(0.4)
                .multilineTextAlignment(.center)
                .lineLimit(displayLabel.contains("\n") ? 2 : 1)
        }
    }

    /// このキーがハイライトされている時の塗り色。実機のロックキー(.ledPulse)は
    /// 実際の LED の色(赤/緑)で光り、それ以外はアクセントカラーのままとする。
    private var highlightFill: Color {
        if key.kind == .ledPulse {
            return key.ledColor ?? Color.accentColor
        }
        return Color.accentColor
    }

    /// P710 — キートップの下地の塗り色。macOS 側の分岐は P228 当初の式とバイト単位で
    /// 同一。iOS には `.controlColor` が無いため、維持すべき関係「キートップはパネルの
    /// 地の色より一段明るい」を、`.systemBackground`(ライト = 白 / ダーク = 黒)と
    /// `.secondarySystemBackground`(パネルの地の色、`panelBackground` 参照)の組で保つ。
    private var keyCapFill: Color {
        #if os(macOS)
        return Color(nsColor: .controlColor)
        #else
        return Color(uiColor: .systemBackground)
        #endif
    }

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: 4)
        let content = shape
            .fill(highlighted ? highlightFill : keyCapFill)
            .overlay(shape.stroke(Color.secondary.opacity(0.5), lineWidth: 0.5))
            .overlay(keyContent.padding(1))

        switch key.kind {
        case .normal:
            content.gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        // key_down はこのジェスチャーの「最初の」変化時にのみ発火する。
                        // SwiftUI は開始後もジェスチャーのセッションを追跡し続ける
                        // (逐次のヒットテストではない)ため、カーソルがキーの外へ
                        // 出ても onEnded は確実に発火する。
                        if !isDown {
                            isDown = true
                            mx68k_key_down(key.scancode)
                        }
                    }
                    .onEnded { _ in
                        isDown = false
                        mx68k_key_up(key.scancode)
                    }
            )
        case .sticky:
            content.onTapGesture { state.toggleSticky(key.scancode) }
        case .ledPulse:
            content.onTapGesture { state.pulse(key.scancode) }
        }
    }
}

// MARK: - ルートビュー

struct SoftKeyboardView: View {
    @StateObject private var state = SoftKeyboardState()

    // keytbl.inc のレイアウトに基づくキャンバス寸法(約 766 × 218)。
    private let canvasWidth: CGFloat = 766
    private let canvasHeight: CGFloat = 218

    /// P710 — パネルの地の色。macOS 側の分岐は P228 当初の式とバイト単位で同一。
    /// iOS 側の対応色は `.secondarySystemBackground`(ライト = 淡いグレー /
    /// ダーク = 濃いグレー)で、macOS の windowBackgroundColor と controlColor の
    /// 明るさの関係(「パネルの地はキートップより暗い」)を、ライト・ダークの
    /// 「両方の」外観で保つために選んだ。
    private var panelBackground: Color {
        #if os(macOS)
        return Color(nsColor: .windowBackgroundColor)
        #else
        return Color(uiColor: .secondarySystemBackground)
        #endif
    }

    var body: some View {
        ZStack(alignment: .topLeading) {
            // 防御的フィルタ: scancode==0 のレイアウト用スペーサーを除外する(現状は存在しない)。
            ForEach(Array(softKeyLayout.enumerated()), id: \.offset) { _, key in
                if key.scancode != 0 {
                    SoftKeyButton(key: key, state: state)
                        .frame(width: key.w, height: key.h)
                        .position(x: key.x + key.w / 2, y: key.y + key.h / 2)
                }
            }
        }
        .frame(width: canvasWidth, height: canvasHeight)
        .padding(12)
        .background(panelBackground)
        .onAppear {
            state.startLEDPolling()
        }
        .onDisappear {
            // P228 レビュー第1ラウンドで必須とされた処理: まだ押下保持中のスティッキー
            // キーを解放し、続いて LED ポーリングを停止して、ウィンドウを閉じた後に
            // 何も発火し続けないようにする。
            state.releaseAllSticky()
            state.stopLEDPolling()
        }
    }
}
