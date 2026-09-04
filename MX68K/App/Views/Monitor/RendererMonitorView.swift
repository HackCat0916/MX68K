import SwiftUI

// P328: XM6 の「レンダラ」相当のパネル。合成後フレームバッファの
// ミニプレビューを表示する(P327 まで SpriteMonitorView にあったものを
// スプライト固有情報でないため分離)。
struct RendererMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Preview").font(.headline)
            if let preview = emulatorViewModel.previewFrame {
                Image(decorative: preview, scale: 1.0)
                    .resizable()
                    .interpolation(.none)
                    .aspectRatio(contentMode: .fit)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                    .border(Color.secondary.opacity(0.4))
            } else {
                Text("(no frame)").foregroundColor(.secondary)
            }
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 360, minHeight: 300, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.rendererVisible = true }
        .onDisappear { emulatorViewModel.engine.rendererVisible = false }
    }
}
