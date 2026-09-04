import SwiftUI

// P348/P690: グラフィック面(GRP)ページの生内容を画像として表示するモニタ。
// XM6のグラフィック画面モニタと同じ「ページの生内容をそのまま見る」用途。
// 色モードに応じてページ数が変わる(16色=4面 / 256色=2面 / 65536色=1面)。
// 16色1024dotモードのみ非対応(512×512固定バッファに収まらないため)。
struct GrpPageMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    @State private var selectedPage = 0

    private var pageCount: Int { emulatorViewModel.grpPageImages.count }

    private var modeDescription: String {
        switch pageCount {
        case 4: return "16色モード(4面)"
        case 2: return "256色モード(2面)"
        case 1: return "65536色モード(1面)"
        default: return "非対応モード"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Graphics Page").font(.headline)
            Text("グラフィック面(\(modeDescription))の生内容。透過ドットは黒背景。")
                .font(.caption)
                .foregroundColor(.secondary)
            if emulatorViewModel.grpPageUsesNemesis {
                Label("拡張VRAM配置(CRTC R20 D11)を検出 — 色解釈は参考表示です(実機仕様の裏付け未確認)",
                      systemImage: "exclamationmark.triangle")
                    .font(.caption)
                    .foregroundColor(.yellow)
            }
            if pageCount > 1 {
                Picker("Page", selection: $selectedPage) {
                    ForEach(0..<pageCount, id: \.self) { page in
                        Text("Page \(page)").tag(page)
                    }
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 400)
            }
            ScrollView([.horizontal, .vertical]) {
                if let img = emulatorViewModel.grpPageImages[selectedPage] {
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
                            Text("エミュレーション未実行、または16色1024×1024モード(モニタ非対応)です")
                                .font(.caption)
                                .foregroundColor(.secondary)
                                .multilineTextAlignment(.center)
                                .padding()
                        )
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .border(Color.secondary.opacity(0.3))
        }
        .padding()
        .frame(minWidth: 640, minHeight: 480, alignment: .topLeading)
        // 色モードが切り替わってページ数が減ったとき、選択中のページ番号が
        // 範囲外に取り残されて「画像なし」表示に固着するのを防ぐ。
        .onChange(of: pageCount) { newCount in
            if selectedPage >= newCount { selectedPage = 0 }
        }
        .onAppear { emulatorViewModel.engine.grpPageVisible = true }
        .onDisappear { emulatorViewModel.engine.grpPageVisible = false }
    }
}
