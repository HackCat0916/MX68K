//
//  SoftKeyboardView.swift
//  MX68K
//
//  P228 — On-screen software keyboard (full JIS replica, independent window,
//  guest-LED mirror). Phase 2 completion-criterion (6) final item.
//
//  Design (see .mx68k_cycles/P228_plan.md):
//  This view is COMPLETELY separate from InputManager's heldKeys/keyboardState
//  bookkeeping. It talks to the guest exclusively through the C bridge calls
//  mx68k_key_down / mx68k_key_up / mx68k_get_key_led, so the physical-keyboard
//  path and the soft-keyboard path never share Swift state.
//
//  Three key classifications:
//   (A) Sticky-momentary — SHIFT/CTRL/OPT.1/OPT.2. Host convenience toggle:
//       first click = key_down (held), second click = key_up. Local bool state.
//       (Real hardware has no LED here — this is an intentional, user-approved
//       non-hardware-accurate convenience. See Docs/05 §2.2.)
//   (B) Pulse + guest-LED-mirror — かな/ローマ字/コード入力/CAPS/INS/ひらがな/全角.
//       Every click sends a down+up pulse. NO local toggle state — the visual
//       LED highlight is driven ENTIRELY by polling mx68k_get_key_led().
//   Normal keys — momentary press/release via DragGesture (down on first change,
//       up on end).
//

import SwiftUI

#if os(iOS)
import UIKit   // P710: UIColor, referenced by Color(uiColor:). Do not rely on
               // SwiftUI's implicit re-export the way the macOS NSColor use does.
#endif

// MARK: - Layout data model

/// One key on the JIS replica. Coordinates/label/scancode are transcribed
/// verbatim from px68k's authoritative `x11/keytbl.inc` — do NOT alter them.
struct SoftKeyDef {
    let x: CGFloat
    let y: CGFloat
    let w: CGFloat
    let h: CGFloat
    let label: String
    let scancode: UInt8
    /// SF Symbol name to render in place of `label` (RET + cursor keys). Default nil.
    /// (Swift's synthesized memberwise init keeps this optional's default, so the
    ///  109 existing entries that omit it continue to compile unchanged.)
    var iconName: String? = nil
    /// Explicit override for the 1-line label font size (pt). When non-nil it wins
    /// over the width/height-derived auto-size, used to visually unify adjacent
    /// keys with different label lengths (HOME/INS, BREAK/COPY). Default nil.
    var fixedFontSize: CGFloat? = nil
}

enum SoftKeyKind {
    case sticky      // SHIFT / CTRL / OPT.1 / OPT.2 — local held toggle
    case ledPulse    // 7 lock keys — pulse + guest-LED mirror
    case normal      // everything else — momentary
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

    /// LED bit index for the 7 lock keys (negative logic in keyLED byte), else nil.
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

    /// Real-hardware lock-LED colour (spec-confirm gate #4, ユーザー real-machine
    /// photo 2026-07-18): かな/ローマ字/コード入力/CAPS/INS lit red, ひらがな/全角
    /// lit green. Only meaningful for `.ledPulse` keys; nil elsewhere.
    var ledColor: Color? {
        switch scancode {
        case 0x5a, 0x5b, 0x5c, 0x5d, 0x5e: return .red
        case 0x5f, 0x60:                   return .green
        default:                           return nil
        }
    }
}

/// Canvas is roughly 766 × 218 pt. Left-top origin; each key is `.position`-ed
/// at (x + w/2, y + h/2). Values transcribed verbatim from `x11/keytbl.inc`.
/// (The one `scancode == 0` spacer entry is a layout gap, not a key, and is not
///  present in this table — a defensive filter also drops any such entry.)
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
    SoftKeyDef(x: 444, y:  48, w: 32, h: 32, label: "¥",     scancode: 0x0e),  // ¥ key
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
    SoftKeyDef(x: 412, y: 150, w: 32, h: 32, label: " ",     scancode: 0x34),  // ろ / underscore key
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
    SoftKeyDef(x: 174, y: 184, w: 128, h: 32, label: "",     scancode: 0x35),  // space key (blank label, real key)
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

/// SHIFT-ON glyph overrides, transcribed verbatim from the plan's shift-glyph
/// table (21 entries). Keys NOT in this table (letters, function keys, editing
/// cluster, etc.) keep their fixed label — JIS keycaps don't change letter case
/// visually. 0x0b (0) is intentionally absent (unchanged under shift).
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

/// かな-ON glyph overrides (standard JIS kana input layout), transcribed verbatim
/// from the plan's kana table (48 entries: 13 number-row + 12 QWERTY-row +
/// 12 ASDF-row + 11 ZXCV-row). Shown when the guest's かな lock LED is lit
/// (guestLED bit0 clear, negative logic) — takes priority over shiftGlyphs.
/// @/[ display the bare 濁点゛/半濁点゜ marks; actual kana composition is not done.
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

// MARK: - State

/// Lightweight state object dedicated to the soft keyboard. Deliberately does NOT
/// touch InputManager's heldKeys / keyboardState — the physical and soft keyboard
/// paths are independent (see Docs/05 §2.2 known-limitation note).
final class SoftKeyboardState: ObservableObject {
    // (A) sticky held state — 4 keys.
    @Published var shiftHeld = false
    @Published var ctrlHeld  = false
    @Published var opt1Held  = false
    @Published var opt2Held  = false

    // (B) guest LED byte (negative logic; idle 0xFF = all off). Polled while visible.
    @Published var guestLED: UInt8 = 0xFF

    private var ledTimer: Timer?

    // MARK: sticky

    func isStickyOn(_ scancode: UInt8) -> Bool {
        switch scancode {
        case 0x70: return shiftHeld
        case 0x71: return ctrlHeld
        case 0x72: return opt1Held
        case 0x73: return opt2Held
        default:   return false
        }
    }

    /// First click sends key_down (held), second click sends key_up.
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
            mx68k_key_down(scancode)   // held indefinitely, no matching up yet
        } else {
            mx68k_key_up(scancode)
        }
    }

    // MARK: pulse (B keys)

    /// Every click on a lock key sends a down+up pulse; the guest decides the
    /// resulting toggle state and reports it back via keyLED.
    func pulse(_ scancode: UInt8) {
        mx68k_key_down(scancode)
        mx68k_key_up(scancode)
    }

    // MARK: LED polling

    func startLEDPolling() {
        stopLEDPolling()
        guestLED = mx68k_get_key_led()   // immediate read so the window opens in-sync
        ledTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
            self?.guestLED = mx68k_get_key_led()
        }
    }

    func stopLEDPolling() {
        ledTimer?.invalidate()
        ledTimer = nil
    }

    // MARK: force-release (P228 round-1 required addition)

    /// Force-release any sticky key still held when the window closes, mirroring
    /// P227 stopGamepadMonitoring() / P229 releaseAutoPause() idle-release so a
    /// held SHIFT/CTRL/OPT can't stay latched in the guest after the window is gone.
    func releaseAllSticky() {
        if shiftHeld { mx68k_key_up(0x70); shiftHeld = false }
        if ctrlHeld  { mx68k_key_up(0x71); ctrlHeld  = false }
        if opt1Held  { mx68k_key_up(0x72); opt1Held  = false }
        if opt2Held  { mx68k_key_up(0x73); opt2Held  = false }
    }
}

// MARK: - Individual key button

private struct SoftKeyButton: View {
    let key: SoftKeyDef
    @ObservedObject var state: SoftKeyboardState

    /// Local per-gesture guard so the DragGesture's repeated `.onChanged` firings
    /// only send one key_down per press (normal keys only).
    @State private var isDown = false

    private var highlighted: Bool {
        switch key.kind {
        case .sticky:
            return state.isStickyOn(key.scancode)
        case .ledPulse:
            guard let bit = key.ledBit else { return false }
            return (state.guestLED & (UInt8(1) << bit)) == 0   // negative logic: 0 = lit
        case .normal:
            return isDown
        }
    }

    private var displayLabel: String {
        // かな-mode glyph takes highest priority: when the guest's かな lock LED is
        // lit (guestLED bit0 clear, negative logic) show the JIS kana glyph. Runs
        // BEFORE the shift/lowercase checks so kana display wins while active; when
        // かな is off (bit0 == 1) this falls through cleanly to the checks below.
        if (state.guestLED & 0x01) == 0, let k = kanaGlyphs[key.scancode] {
            return k
        }
        // Symbol/number keys show their shifted glyph when the local SHIFT is on.
        if state.shiftHeld, let g = shiftGlyphs[key.scancode] {
            return g
        }
        // Letter keys read lowercase until SHIFT is held (JIS keycaps show upper,
        // but this matches the actual glyph produced). shiftGlyphs holds only
        // symbol/number scancodes, so letters never collide with the branch above.
        if !state.shiftHeld,
           key.label.count == 1,
           let c = key.label.first, c.isLetter, c.isUppercase {
            return key.label.lowercased()
        }
        return key.label
    }

    /// 1-line text label font size. Priority: explicit per-key override >
    /// 2-line fixed 9pt (P232) > width/height-derived auto-size. The auto-size
    /// uses min(w, h) so a tall-narrow key (ENTER, 32×66) uses the same baseline
    /// as a square key of the same width (コード入力, 32×32) instead of a
    /// height-inflated size that then truncates under .minimumScaleFactor.
    private var fontSize: CGFloat {
        if let fixed = key.fixedFontSize { return fixed }
        if displayLabel.contains("\n") { return 9 }
        return max(8, min(key.w, key.h) * 0.36)
    }

    /// The glyph shown on the keycap: an SF Symbol icon (RET + cursor keys) or the
    /// text label. Both share the same highlight foreground colour. The icon branch
    /// keeps its own height-derived size (all icon keys are square, so unchanged).
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

    /// Fill colour when this key is highlighted. Real-hardware lock keys (.ledPulse)
    /// glow their actual LED colour (red/green); everything else keeps the accent.
    private var highlightFill: Color {
        if key.kind == .ledPulse {
            return key.ledColor ?? Color.accentColor
        }
        return Color.accentColor
    }

    /// P710 — keycap base fill. The macOS branch is byte-identical to P228's
    /// original expression. iOS has no `.controlColor`; the relationship being
    /// preserved is "keycap sits one step brighter than the panel ground", which
    /// `.systemBackground` (light = white / dark = black) holds against
    /// `.secondarySystemBackground` (the panel ground, see `panelBackground`).
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
                        // Fire key_down only on the FIRST change of this gesture.
                        // SwiftUI keeps tracking the gesture session after
                        // initiation (not live hit-testing), so onEnded still
                        // fires reliably even if the cursor leaves the key.
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

// MARK: - Root view

struct SoftKeyboardView: View {
    @StateObject private var state = SoftKeyboardState()

    // Canvas dimensions per the keytbl.inc layout (~766 × 218).
    private let canvasWidth: CGFloat = 766
    private let canvasHeight: CGFloat = 218

    /// P710 — panel ground colour. The macOS branch is byte-identical to P228's
    /// original expression. The iOS counterpart is `.secondarySystemBackground`
    /// (light = pale grey / dark = deep grey), chosen to keep macOS's
    /// windowBackgroundColor-vs-controlColor brightness relationship
    /// ("panel ground darker than keycap") in BOTH light and dark appearances.
    private var panelBackground: Color {
        #if os(macOS)
        return Color(nsColor: .windowBackgroundColor)
        #else
        return Color(uiColor: .secondarySystemBackground)
        #endif
    }

    var body: some View {
        ZStack(alignment: .topLeading) {
            // Defensive filter: drop any scancode==0 layout spacer (none present).
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
            // P228 round-1 required: release any sticky key still held, then
            // stop the LED poll so nothing keeps firing after the window closes.
            state.releaseAllSticky()
            state.stopLEDPolling()
        }
    }
}
