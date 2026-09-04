import SwiftUI

struct CRTCMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    var body: some View {
        let s = emulatorViewModel.crtcStatus
        // P595 (D-55): 表示ジオメトリ(単位「標準表示窓=1.0」)。
        let g = emulatorViewModel.displayGeometry
        VStack(alignment: .leading, spacing: 8) {
            Text("CRTC Status").font(.headline)
            Divider()
            HStack(spacing: 24) {
                Text("Horizontal: \(String(format: "%.2f", s.hsync_khz))kHz")
                Text("Vertical: \(String(format: "%.2f", s.vsync_hz))Hz")
            }
            HStack(spacing: 24) {
                Text("Width: \(s.text_dot_x)dot")
                Text("Height: \(s.text_dot_y)dot")
            }
            Divider()
            HStack(spacing: 24) {
                Text("HSTART: \(s.crtc_hstart)")
                Text("HEND: \(s.crtc_hend)")
            }
            HStack(spacing: 24) {
                Text("VSTART: \(s.crtc_vstart)")
                Text("VEND: \(s.crtc_vend)")
            }
            Text("VLINE Total: \(s.vline_total)")
            Divider()
            // P595 (D-55): 標準ラスタでは 1.0/1.0/0.0/0.0・mode=1。他タイトルに
            // 非標準ラスタが無いかを実測で確認するための常時可視化
            // (Fix Plan「自己反証可能性」節により必須)。
            HStack(spacing: 24) {
                Text("H Scale: \(String(format: "%.4f", Double(g.hScale)))")
                Text("V Scale: \(String(format: "%.4f", Double(g.vScale)))")
            }
            HStack(spacing: 24) {
                Text("Off X: \(String(format: "%.4f", Double(g.offX)))")
                Text("Off Y: \(String(format: "%.4f", Double(g.offY)))")
                Text("Geo Mode: \(g.geoMode)")
            }
            Divider()
            HStack(spacing: 24) {
                Text("Text Scroll X: \(s.text_scroll_x)")
                Text("Text Scroll Y: \(s.text_scroll_y)")
            }
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 320, minHeight: 260, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.crtcVisible = true }
        .onDisappear { emulatorViewModel.engine.crtcVisible = false }
    }
}
