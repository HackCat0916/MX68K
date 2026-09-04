import SwiftUI

struct SpriteMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    private let thumbSize: CGFloat = 16
    private let imgColWidth: CGFloat = 30   // P331: ヘッダ"IMG"が折り返さない幅(thumbSizeより広め)

    private func hex(_ v: Int, _ digits: Int) -> String {
        String(format: "%0\(digits)X", v)
    }

    private let columnSlotRanges: [ClosedRange<Int32>] = [0...42, 43...86, 87...127]   // P332: 固定スロット範囲

    private func splitByFixedSlotRange(_ entries: [MX68KSpriteEntry]) -> [[MX68KSpriteEntry]] {
        columnSlotRanges.map { range in
            entries.filter { range.contains($0.slot) }
        }
    }

    @ViewBuilder
    private func spriteHeaderRow() -> some View {
        HStack(spacing: 0) {
            Text("IMG").frame(width: imgColWidth, alignment: .center)
            Text("#").frame(width: 34, alignment: .trailing)
            Text("X").frame(width: 44, alignment: .trailing)
            Text("Y").frame(width: 44, alignment: .trailing)
            Text("PAT").frame(width: 44, alignment: .trailing)
            Text("PAL").frame(width: 44, alignment: .trailing)
            Text("FLIP").frame(width: 44, alignment: .center)
            Text("PRI").frame(width: 40, alignment: .trailing)
        }
        .foregroundColor(.secondary)
    }

    private func flipLabel(_ e: MX68KSpriteEntry) -> String {
        let h = e.hflip ? "H" : "-"
        let v = e.vflip ? "V" : "-"
        return "\(h)\(v)"
    }

    @ViewBuilder
    private func spriteThumb(_ e: MX68KSpriteEntry) -> some View {
        if let img = emulatorViewModel.spritePatterns[Int(e.slot)] {
            Image(decorative: img, scale: 1.0)
                .resizable()
                .interpolation(.none)
                .frame(width: thumbSize, height: thumbSize)
                .border(Color.secondary.opacity(0.3))
        } else {
            Rectangle()
                .fill(Color.secondary.opacity(0.1))
                .frame(width: thumbSize, height: thumbSize)
                .border(Color.secondary.opacity(0.3))
        }
    }

    @ViewBuilder
    private func spriteRow(_ e: MX68KSpriteEntry) -> some View {
        HStack(spacing: 0) {
            spriteThumb(e).frame(width: imgColWidth, alignment: .center)
            Text("\(e.slot)").frame(width: 34, alignment: .trailing)
            Text("\(e.x)").frame(width: 44, alignment: .trailing)
            Text("\(e.y)").frame(width: 44, alignment: .trailing)
            Text(hex(Int(e.pattern), 2)).frame(width: 44, alignment: .trailing)
            Text(hex(Int(e.palette_hi), 2)).frame(width: 44, alignment: .trailing)
            Text(flipLabel(e)).frame(width: 44, alignment: .center)
            Text("\(e.priority)").frame(width: 40, alignment: .trailing)
        }
    }

    var body: some View {
        let columns = splitByFixedSlotRange(emulatorViewModel.spriteList)
        VStack(alignment: .leading, spacing: 8) {
            Text("Sprite List").font(.headline)
            HStack(alignment: .top, spacing: 16) {
                ForEach(0..<3, id: \.self) { _ in
                    spriteHeaderRow()
                }
            }
            .font(.system(.caption, design: .monospaced))
            ScrollView {
                HStack(alignment: .top, spacing: 16) {
                    ForEach(0..<columns.count, id: \.self) { col in
                        VStack(alignment: .leading, spacing: 2) {
                            ForEach(columns[col], id: \.slot) { entry in
                                spriteRow(entry)
                            }
                        }
                    }
                }
            }
            .font(.system(.caption, design: .monospaced))
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 1060, minHeight: 480, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.spriteMonitorVisible = true }
        .onDisappear { emulatorViewModel.engine.spriteMonitorVisible = false }
    }
}
