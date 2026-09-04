import SwiftUI

// P348: BGページ(0/1)の生タイルマップ+パターン内容を画像として表示する
// モニタ。XM6のBG画面(ページ0)モニタと同じ「ページの生内容をそのまま見る」
// 用途。OverTake等の描画不具合調査で、路面/背景テクスチャがBridge側で
// 正しく合成されているか目視確認するために使う。CHRSIZE設定により実効
// サイズは512または1024(いずれの場合も左上のみに内容があり残りは透過)。
struct BGPageMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    @State private var selectedPage = 0

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("BG Page").font(.headline)
            Text("BGタイルマップ+パターンの生内容。透過ドットは黒背景。")
                .font(.caption)
                .foregroundColor(.secondary)
            Picker("Page", selection: $selectedPage) {
                Text("BG0").tag(0)
                Text("BG1").tag(1)
            }
            .pickerStyle(.segmented)
            .frame(maxWidth: 200)
            ScrollView([.horizontal, .vertical]) {
                if let img = emulatorViewModel.bgPageImages[selectedPage] {
                    Image(decorative: img, scale: 1.0)
                        .resizable()
                        .interpolation(.none)
                        .frame(width: CGFloat(img.width), height: CGFloat(img.height))
                        .background(Color.black)
                } else {
                    Rectangle()
                        .fill(Color.secondary.opacity(0.1))
                        .frame(width: 512, height: 512)
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
        .onAppear { emulatorViewModel.engine.bgPageVisible = true }
        .onDisappear { emulatorViewModel.engine.bgPageVisible = false }
    }
}
