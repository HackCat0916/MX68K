import SwiftUI

/// P929 — MIDI 鍵盤パネル(独立ウィンドウ)。
///
/// 内蔵 MT-32 / 内蔵 SC-55 / 外部 MIDI 出力の 3 経路共通に、ゲストが送信した Note On/Off を
/// MIDI チャンネルごとの鍵盤 16 段で表示する。鍵盤描画(白鍵/黒鍵のスロットと座標)は
/// OPMSynthesizerView(P631)と同じ寸法・同じ配置で、MIDI の和音に合わせて
/// 「1 段に複数鍵を同時点灯」できる形へ一般化している(OPM 側は 1ch 1 鍵の単音前提)。
///
/// 更新頻度: 専用の可視性フラグ(EmulatorEngine.midiKeyboardVisible)で駆動される毎フレーム更新。
///
/// ★表示は「ゲストが送った Note On/Off」であり、音源が実際に発音中のボイスとは一致しない
///   (リリース中の残響・ボイス数上限による打切り・CC64 ホールドは反映されない)。
struct MIDIKeyboardView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    /// false の間は、今 1 つも ON のノートを持たないチャンネルの鍵盤を畳んで情報行だけにする。
    @State private var showAllChannels = true

    // MARK: - 鍵盤の寸法・配色(OPMSynthesizerView と同じ値)

    /// 表示する鍵域。OPM Keyboard Viewer と同じ 8 オクターブ = MIDI ノート 0-95。
    /// ノート 96-127 は行末の範囲外インジケータで存在だけを示す。
    private static let keyboardOctaves = 0...7
    private static let displayedNoteCount = 96
    private static let whiteNotes = [0, 2, 4, 5, 7, 9, 11]
    private static let whiteKeyWidth: CGFloat = 10
    private static let whiteKeyHeight: CGFloat = 34
    private static let blackKeyWidth: CGFloat = 7
    private static let blackKeyHeight: CGFloat = 21
    private static let rowHeight: CGFloat = 38
    private static let infoWidth: CGFloat = 210
    private static let outOfRangeWidth: CGFloat = 36

    private struct BlackKeySlot: Identifiable {
        let whiteIndex: Int
        let note: Int
        var id: Int { note }
    }

    private static let blackKeySlots: [BlackKeySlot] = [
        BlackKeySlot(whiteIndex: 0, note: 1),   // C#
        BlackKeySlot(whiteIndex: 1, note: 3),   // D#
        BlackKeySlot(whiteIndex: 3, note: 6),   // F#
        BlackKeySlot(whiteIndex: 4, note: 8),   // G#
        BlackKeySlot(whiteIndex: 5, note: 10),  // A#
    ]

    private static let octaveWidth: CGFloat = whiteKeyWidth * CGFloat(whiteNotes.count)
    private static let keyboardWidth: CGFloat = octaveWidth * CGFloat(keyboardOctaves.count)

    /// CH1〜CH16 の識別色。16 色を色相環上に等間隔で取る。
    private static let channelColors: [Color] = (0..<16).map {
        Color(hue: Double($0) / 16.0, saturation: 0.75, brightness: 0.95)
    }

    // MARK: - データ

    /// C の 2 次元配列 `uint8_t vel[16][128]` は Swift へ入れ子タプルとして取り込まれるため、
    /// [ch * 128 + note] の平坦な配列へ展開する。
    private var velocities: [UInt8] {
        withUnsafeBytes(of: emulatorViewModel.midiNoteStatus.vel) { Array($0) }
    }

    // MARK: - 本体

    var body: some View {
        let vel = velocities
        let mask = emulatorViewModel.midiNoteStatus.active_mask
        return VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("MIDI Keyboard (16ch)").font(.headline)
                Spacer()
                Toggle("Show all channels", isOn: $showAllChannels)
                    .toggleStyle(.checkbox)
                    .font(.caption)
            }
            Divider()
            ScrollView([.vertical, .horizontal], showsIndicators: true) {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(0..<16, id: \.self) { ch in
                        let notes = Array(vel[(ch * 128)..<(ch * 128 + 128)])
                        let active = (mask & (UInt16(1) << UInt16(ch))) != 0
                        VStack(alignment: .leading, spacing: 0) {
                            infoRow(channel: ch, notes: notes, active: active)
                            if active || showAllChannels {
                                HStack(alignment: .top, spacing: 4) {
                                    keyboardRow(channel: ch, notes: notes)
                                    outOfRangeIndicator(channel: ch, notes: notes)
                                }
                                octaveRuler
                            }
                        }
                    }
                }
            }
            Divider()
            footnotes
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 640, minHeight: 300, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.midiKeyboardVisible = true }
        .onDisappear { emulatorViewModel.engine.midiKeyboardVisible = false }
    }

    // MARK: - 1ch 分の情報欄

    /// `CH1  ON:3` — 今 ON のノート数(表示範囲外を含む全 128 鍵分)。
    private func infoRow(channel ch: Int, notes: [UInt8], active: Bool) -> some View {
        let count = notes.reduce(0) { $0 + ($1 != 0 ? 1 : 0) }
        return HStack(spacing: 8) {
            Text(String(format: "CH%02d", ch + 1))
                .foregroundColor(active ? Self.channelColors[ch] : .secondary)
            Text(active ? "ON:\(count)" : "--")
                .foregroundColor(active ? .primary : .secondary)
        }
        .font(.system(.caption, design: .monospaced))
        .frame(width: Self.infoWidth, alignment: .leading)
    }

    // MARK: - 1ch 分の鍵盤

    /// 96 鍵 1 段。ON の鍵を全てチャンネル色で塗る(和音対応)。濃さはベロシティに比例させる。
    private func keyboardRow(channel ch: Int, notes: [UInt8]) -> some View {
        let color = Self.channelColors[ch]
        return HStack(spacing: 0) {
            ForEach(Array(Self.keyboardOctaves), id: \.self) { oct in
                octaveView(octave: oct, notes: notes, color: color)
            }
        }
        .frame(width: Self.keyboardWidth, height: Self.rowHeight, alignment: .topLeading)
    }

    private func octaveView(octave: Int, notes: [UInt8], color: Color) -> some View {
        let w = Self.whiteKeyWidth
        return ZStack(alignment: .topLeading) {
            HStack(spacing: 0) {
                ForEach(Self.whiteNotes, id: \.self) { note in
                    keyView(velocity: notes[octave * 12 + note], isBlack: false, color: color)
                        .frame(width: w, height: Self.whiteKeyHeight)
                }
            }
            ForEach(Self.blackKeySlots) { slot in
                keyView(velocity: notes[octave * 12 + slot.note], isBlack: true, color: color)
                    .frame(width: Self.blackKeyWidth, height: Self.blackKeyHeight)
                    .offset(x: CGFloat(slot.whiteIndex + 1) * w - Self.blackKeyWidth / 2)
            }
        }
        .frame(width: Self.octaveWidth, height: Self.whiteKeyHeight, alignment: .topLeading)
    }

    private func keyView(velocity: UInt8, isBlack: Bool, color: Color) -> some View {
        let base = isBlack ? Color.black : Color.white
        return ZStack {
            Rectangle().fill(base)
            if velocity != 0 {
                Rectangle().fill(color.opacity(0.4 + 0.6 * Double(velocity) / 127.0))
            }
        }
        .overlay(Rectangle().stroke(Color.gray.opacity(0.6), lineWidth: 0.5))
    }

    /// 表示鍵域(ノート 0-95)より上のノート 96-127 が ON のとき、行末に件数付きで示す。
    private func outOfRangeIndicator(channel ch: Int, notes: [UInt8]) -> some View {
        let count = notes[Self.displayedNoteCount...].reduce(0) { $0 + ($1 != 0 ? 1 : 0) }
        return Group {
            if count > 0 {
                Text("▶\(count)")
                    .foregroundColor(Self.channelColors[ch])
                    .help("Notes above B6 (MIDI 96-127) are on")
            } else {
                Text("")
            }
        }
        .font(.system(size: 10, design: .monospaced))
        .frame(width: Self.outOfRangeWidth, height: Self.whiteKeyHeight, alignment: .leading)
    }

    /// オクターブ目盛り。MIDI ノート 60 = C4 の表記(ノート 0 = C-1)。
    private var octaveRuler: some View {
        HStack(spacing: 0) {
            ForEach(Array(Self.keyboardOctaves), id: \.self) { oct in
                Text("C\(oct - 1)")
                    .font(.system(size: 8, design: .monospaced))
                    .foregroundColor(.secondary)
                    .frame(width: Self.octaveWidth, alignment: .leading)
            }
        }
        .frame(width: Self.keyboardWidth, height: 12, alignment: .leading)
    }

    // MARK: - 注記

    private var footnotes: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("※ この表示はゲストが送信した Note On/Off に基づくものであり、音源が実際に発音中のボイスとは異なる場合があります(リリース中の残響・発音数上限・ホールドペダルは反映されません)")
            Text("※ 表示範囲はノート 0〜95(C-1〜B6、ノート 60 = C4)です。ノート 96〜127 が ON のときは行末に ▶件数 を表示します")
        }
        .font(.caption)
        .foregroundColor(.secondary)
    }
}
