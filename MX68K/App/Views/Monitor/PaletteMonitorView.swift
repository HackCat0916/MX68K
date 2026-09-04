import SwiftUI

/// P550 — パレットモニタ。テキスト面(TextPal32)・グラフィック面(GrphPal32)の
/// 各256色と、コントラスト値(Contrast_Value)を可視化する。
///
/// 更新頻度: CRTC/VC/BG と同じく、このパネルが表示されている間だけ動く
/// 毎フレーム更新(EmulatorEngine.paletteVisible)。read-only スナップショット
/// なのでエミュレーション本体への影響はない。
///
/// ★色成分の並び: Core が保持する 32bit パレット語は px68k ネイティブの
///   0xRRGGBB0A(R = bits 31-24 / G = bits 23-16 / B = bits 15-8 / bit0 = 半透明
///   用 Abit)。この配置は Bridge/EmulatorBridge.c の mx68k_init() が
///   WinDraw_Pal32R/G/B = 0xFF000000 / 0x00FF0000 / 0x0000FF00 を設定すること
///   で決まっており、表示用フレームバッファ(BGRA)へは同ファイルの
///   px68k_color_to_rgba() が (c >> 8) | 0xFF000000 で変換している。
///   最下位バイトは表示用アルファではないため、スウォッチは常に不透明で描く。
struct PaletteMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    private static let swatchSize: CGFloat = 15
    private static let swatchSpacing: CGFloat = 1

    /// C の固定長配列 `uint32_t text_pal[256]` は Swift へタプルとして取り込まれる
    /// ため、描画用に [UInt32] へ展開する(SoundMonitorView の waveform と同型)。
    private var textPalette: [UInt32] {
        withUnsafeBytes(of: emulatorViewModel.paletteStatus.text_pal) { raw in
            Array(raw.bindMemory(to: UInt32.self))
        }
    }

    private var graphicsPalette: [UInt32] {
        withUnsafeBytes(of: emulatorViewModel.paletteStatus.grph_pal) { raw in
            Array(raw.bindMemory(to: UInt32.self))
        }
    }

    /// 0xRRGGBB0A → SwiftUI Color(不透明)。
    private static func color(_ v: UInt32) -> Color {
        Color(red:   Double((v >> 24) & 0xFF) / 255.0,
              green: Double((v >> 16) & 0xFF) / 255.0,
              blue:  Double((v >>  8) & 0xFF) / 255.0)
    }

    /// ホバー時のツールチップ用: インデックス・RGB 16進・Abit。
    private static func tooltip(_ index: Int, _ v: UInt32) -> String {
        // ★String(format:) に Int を渡して %d で受けるのは 64bit/32bit 幅が
        //   食い違うため、インデックスは補間で組み立てる。
        let rgb = String(format: "R=%02X G=%02X B=%02X",
                         (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF)
        return "#\(index)  \(rgb)  A=\(v & 1)"
    }

    private func grid(_ entries: [UInt32]) -> some View {
        VStack(alignment: .leading, spacing: Self.swatchSpacing) {
            ForEach(0..<16, id: \.self) { row in
                HStack(spacing: Self.swatchSpacing) {
                    ForEach(0..<16, id: \.self) { col in
                        let index = row * 16 + col
                        let value = index < entries.count ? entries[index] : 0
                        Rectangle()
                            .fill(Self.color(value))
                            .frame(width: Self.swatchSize, height: Self.swatchSize)
                            .overlay(Rectangle().stroke(Color.secondary.opacity(0.25), lineWidth: 0.5))
                            .help(Self.tooltip(index, value))
                    }
                }
            }
        }
    }

    /// ★`title` は `String` ではなく `LocalizedStringKey`: `Text(String)` は
    ///   非ローカライズ版オーバーロード(`Text.init<S: StringProtocol>`)が選ばれて
    ///   しまい、`Localizable.xcstrings` の訳語が反映されない(P553)。
    private func paletteSection(_ title: LocalizedStringKey, _ entries: [UInt32]) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.subheadline).bold()
            grid(entries)
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Palette").font(.headline)
            Divider()
            Text("Contrast: \(Int(emulatorViewModel.paletteStatus.contrast)) / 15")
            Divider()
            HStack(alignment: .top, spacing: 24) {
                paletteSection("Text / BG / Sprite (TextPal32)", textPalette)
                paletteSection("Graphics (GrphPal32)", graphicsPalette)
            }
            Text("並びは左上 #0 → 右下 #255(16×16)。マウスオーバーで RGB 値を表示。")
                .font(.caption)
                .foregroundColor(.secondary)
            Spacer()
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 620, minHeight: 400, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.paletteVisible = true }
        .onDisappear { emulatorViewModel.engine.paletteVisible = false }
    }
}
