import SwiftUI

// P329: 座標/パターン/パレット等のデータ列を廃止し、定義済み
// スプライトパターンの画像だけを16列×8行のグリッドで一覧表示する。
// Sprite List(稼働中スプライトの詳細情報)とは明確に異なる用途:
// こちらは「今どんなパターンが128スロットに定義されているか」を
// 一目で見るためのビュー。
struct SpriteTableMapView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    private let thumbSize: CGFloat = 32
    private let columns = Array(repeating: GridItem(.fixed(40), spacing: 4), count: 16)

    private func isUnused(_ e: MX68KSpriteEntry) -> Bool {
        e.x == 0 && e.y == 0
    }

    @ViewBuilder
    private func cell(_ e: MX68KSpriteEntry) -> some View {
        VStack(spacing: 1) {
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
            Text("\(e.slot)")
                .font(.system(size: 8, design: .monospaced))
                .foregroundColor(.secondary)
        }
        .opacity(isUnused(e) ? 0.35 : 1.0)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Sprite Table Map").font(.headline)
            Text("全128スロットのパターン画像(未使用スロットは淡色表示)")
                .font(.caption)
                .foregroundColor(.secondary)
            ScrollView {
                LazyVGrid(columns: columns, spacing: 6) {
                    ForEach(emulatorViewModel.spriteTable, id: \.slot) { entry in
                        cell(entry)
                    }
                }
                .padding(.top, 4)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .padding()
        .frame(minWidth: 760, minHeight: 420, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.spriteTableVisible = true }
        .onDisappear { emulatorViewModel.engine.spriteTableVisible = false }
    }
}
