import Foundation

/// P693 — MIDI モニタ(MIDI Viewer)用の MIDI ボード(CZ-6BM1 / YM3802)状態。
///
/// パネル表示中のみ毎フレーム更新される(`EmulatorEngine.midiVisible`)。
///
/// ★設計方針(Fix Plan「自己反証可能性」節): カウンタは必ず「分母」と一緒に
///   持つ。`txMessages`(ゲストが送信を試みた回数)と `txBytes`(実際に
///   CoreMIDI へ渡せたバイト数)を別々に保持しているのはそのためで、
///   `txMessages > 0 && txBytes == 0` なら送出層で落ちている、`txBytes > 0`
///   なのに無音なら外部機器側 —— とゼロ表示の解釈を 1 つに絞れるようにする。
///
/// ★デバイス名はこの型に入れない。名前一覧は `MIDI_Init()`(init / ハード
///   リセット)でしか更新されないため、`EmulatorViewModel.refreshMidiDeviceList()`
///   が持つ既存の配列を設定画面と共有し、モニタ側は `.onAppear` で 1 回だけ
///   取り込む。毎フレームの tick から取りに行くと、更新されない値を
///   ライブ値であるかのように見せてしまう。
struct EmulatorMIDIStatus {
    /// MIDI ボードが配線済みか(設定値ではなくラッチ後の確定値、
    /// `mx68k_midi_is_wired()`)。
    var wired: Bool = false
    /// CoreMIDI が見つけた出力/入力デバイス数。カウンタがゼロのときに
    /// 「そもそも送り先が無い」のか「送っているのに届いていない」のかを
    /// 分けるための分母。
    var outputDeviceCount: Int = 0
    var inputDeviceCount: Int = 0

    /// TX: ゲスト → 実 MIDI 機器。
    var txMessages: UInt64 = 0
    var txBytes: UInt64 = 0
    /// 直近 1 メッセージのパック生値(`status | len<<8 | data1<<16 | data2<<24`)。
    /// 展開値ではなく生値を保持し、表示側で展開する(派生値だけを見せない)。
    var txLast: UInt32 = 0

    /// RX: 実 MIDI 機器 → ゲスト。`rxMessages` の単位は CoreMIDI パケット
    /// (1 パケットに複数メッセージが載りうるので厳密なメッセージ数ではない)。
    var rxMessages: UInt64 = 0
    var rxBytes: UInt64 = 0
    var rxLast: UInt32 = 0

    /// YM3802 レジスタ生値(7 項目)。`MIDI_IntFlag` / `MIDI_IntVect` は
    /// 既知の 2 スレッド無同期共有(Docs/09 の D-73)のため意図的に含まない。
    var regs = MX68K_MIDIRegs()
}

/// 直近メッセージのパック値を展開したもの。表示専用の派生値で、元の
/// パック生値(`EmulatorMIDIStatus.txLast` / `.rxLast`)は失わない。
struct MIDILastMessage {
    var status: UInt8
    var length: UInt8
    var data1: UInt8
    var data2: UInt8

    /// パックはまだ 1 度も書かれていない(= 送受信が 1 度も無かった)。
    var isEmpty: Bool { length == 0 }

    init(packed: UInt32) {
        status = UInt8(packed & 0xff)
        length = UInt8((packed >> 8) & 0xff)
        data1 = UInt8((packed >> 16) & 0xff)
        data2 = UInt8((packed >> 24) & 0xff)
    }

    /// 実際に意味のあるデータバイトだけを 16 進で並べる。`length` は
    /// min(実長, 0xff) にクランプされた目安値なので、3 バイトを超える分は
    /// 「…」で省略していることを示す。
    var hexDescription: String {
        if isEmpty { return "--" }
        var parts = [String(format: "%02X", status)]
        if length >= 2 { parts.append(String(format: "%02X", data1)) }
        if length >= 3 { parts.append(String(format: "%02X", data2)) }
        if length > 3 { parts.append("…") }
        return parts.joined(separator: " ")
    }

    /// ステータスバイトの種別名。生値の `status` はそのまま別に表示するので、
    /// この派生名が誤っていても元の値は読み取れる。
    var statusName: String {
        switch status & 0xf0 {
        case 0x80: return "Note Off"
        case 0x90: return "Note On"
        case 0xa0: return "Poly Pressure"
        case 0xb0: return "Control Change"
        case 0xc0: return "Program Change"
        case 0xd0: return "Ch Pressure"
        case 0xe0: return "Pitch Bend"
        case 0xf0:
            switch status {
            case 0xf0: return "SysEx"
            case 0xf1: return "MTC Quarter Frame"
            case 0xf2: return "Song Position"
            case 0xf3: return "Song Select"
            case 0xf8: return "Timing Clock"
            case 0xfa: return "Start"
            case 0xfb: return "Continue"
            case 0xfc: return "Stop"
            case 0xfe: return "Active Sensing"
            case 0xff: return "System Reset"
            default:   return "System"
            }
        default: return "(data)"
        }
    }

    /// MIDI チャンネル(1-16)。システムメッセージ(0xF0 系)には無い。
    var channel: Int? {
        if (status & 0xf0) == 0xf0 { return nil }
        if status < 0x80 { return nil }
        return Int(status & 0x0f) + 1
    }
}
