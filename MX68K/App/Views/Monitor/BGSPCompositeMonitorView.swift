import SwiftUI

// P365: BG+Sprite合成バッファ(最終GRP/テキスト優先度合成"前"の中間状態)
// を画像として表示するモニタ。XM6の「BG/スプライトバッファ」モニタ相当。
// 「Renderer」パネル(最終合成後)と並べて比較することで、黒帯等の描画
// 不具合がBG/スプライト面自体の欠落によるものか、その後の優先度合成
// 段階によるものかを切り分けられる。
struct BGSPCompositeMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    // P373: 状態スタンプ — スクショだけでゲーム状態を自己証明させ、
    // 撮影タイミングの取り違え(P366§6/P369/P370/P372で計4回発生)を防ぐ。
    // P383: VC/BGレジスタも frame_num と同じく毎レンダリング生値を取得する
    // (旧実装は~1Hzゲート越しのvcStatus/bgStatusを見ており最大1秒古い値を
    // 最新のフレーム番号と並べて表示しうる)。vc1_0は表示用と bg_above_text の
    // 両方で使うが、値のズレを避けるため取得は1回だけ。
    // 各値を個別のletに分けているのは、長い + 連結式がSwiftの型検査を
    // 極端に遅くする(コンパイル不能になり得る)のを避けるため。
    // なお画像(bgspCompositeImage)自体は引き続き~1Hz更新であり、
    // このスタンプのレジスタ値より古い時点のものでありうる。
    private var stateStamp: String {
        let frame = mx68k_get_frame_num()
        let vc1_0 = mx68k_get_vc_reg1_0_live()
        let vc2_1 = mx68k_get_vc_reg2_1_live()
        let bg9 = mx68k_get_bg_regs9_live()
        let regs = String(format: "VC1[0]=0x%02x VC2[1]=0x%02x", vc1_0, vc2_1)
        let above = ((UInt32(vc1_0) & 0x30) >> 2) < (UInt32(vc1_0) & 0x0c) ? 1 : 0
        let bg0 = (bg9 & 0x01) != 0 ? "ON" : "OFF"
        let bg1 = (bg9 & 0x08) != 0 ? "ON" : "OFF"
        let togBg0 = mx68k_get_bg_layer_visible(0) != 0 ? "ON" : "OFF"
        let togBg1 = mx68k_get_bg_layer_visible(1) != 0 ? "ON" : "OFF"
        let w = emulatorViewModel.bgspCompositeImage?.width ?? 0
        let h = emulatorViewModel.bgspCompositeImage?.height ?? 0
        return "f=\(frame) \(regs) bg_above_text=\(above) "
             + "BG0=\(bg0) BG1=\(bg1) トグルBG0=\(togBg0) BG1=\(togBg1) \(w)x\(h)"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("BG+Sprite Composite").font(.headline)
            Text("最終合成前のBG+スプライト面。透過ドットは黒。")
                .font(.caption)
                .foregroundColor(.secondary)
            Text(stateStamp)
                .font(.system(.caption, design: .monospaced))
                .foregroundColor(.secondary)
            ScrollView([.horizontal, .vertical]) {
                if let img = emulatorViewModel.bgspCompositeImage {
                    Image(decorative: img, scale: 1.0)
                        .resizable()
                        .interpolation(.none)
                        .frame(width: CGFloat(img.width), height: CGFloat(img.height))
                        .background(Color.black)
                } else {
                    Rectangle()
                        .fill(Color.secondary.opacity(0.1))
                        .frame(width: 768, height: 512)
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
        // P521: Swift側の読出しゲートに加え、C側のs_bgsp_buffer書込みゲートも
        // 同じタイミングで開閉する(パネル非表示中は毎フレームのストアを省く)。
        .onAppear {
            emulatorViewModel.engine.bgspCompositeVisible = true
            mx68k_set_bgsp_composite_visible(1)
        }
        .onDisappear {
            emulatorViewModel.engine.bgspCompositeVisible = false
            mx68k_set_bgsp_composite_visible(0)
        }
    }
}
