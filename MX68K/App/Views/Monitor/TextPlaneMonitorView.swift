import SwiftUI

// P343: 派生テキストバッファ(TextDrawWork, 1024x1024)の生の内容を
// 画像として表示するモニタ。XM6のBGモニタと同じ「ページの生内容を
// そのまま見る」用途。OverTake等の描画不具合調査で、走査線ごとの
// レジスタ値からの推測ではなくテキスト面を直接目視確認するために使う。
struct TextPlaneMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    @State private var backdropEnabled = true   // P356

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Text Plane").font(.headline)
            Text(backdropEnabled
                 ? "派生テキストバッファ TextDrawWork(1024×1024)の生内容。インデックス0は背景色(TextPal32[0])で描画(XM6互換)。"
                 : "派生テキストバッファ TextDrawWork(1024×1024)の生内容。インデックス0は透過(黒背景)で描画。")
                .font(.caption)
                .foregroundColor(.secondary)
            Toggle("背景色描画(XM6互換)", isOn: $backdropEnabled)
                .onChange(of: backdropEnabled) { newValue in
                    mx68k_set_text_plane_backdrop(newValue ? 1 : 0)
                }
                .frame(maxWidth: 240)
            ScrollView([.horizontal, .vertical]) {
                if let img = emulatorViewModel.textPlaneImage {
                    Image(decorative: img, scale: 1.0)
                        .resizable()
                        .interpolation(.none)
                        .frame(width: 1024, height: 1024)
                        .background(Color.black)
                } else {
                    Rectangle()
                        .fill(Color.secondary.opacity(0.1))
                        .frame(width: 1024, height: 1024)
                        .overlay(
                            Text("エミュレーション未実行")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        )
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .border(Color.secondary.opacity(0.3))
        }
        .padding()
        .frame(minWidth: 640, minHeight: 480, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.textPlaneVisible = true }
        .onDisappear { emulatorViewModel.engine.textPlaneVisible = false }
    }
}
