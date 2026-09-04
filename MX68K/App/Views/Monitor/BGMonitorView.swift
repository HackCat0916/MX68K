import SwiftUI

struct BGMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    @State private var bg0Visible = (mx68k_get_bg_layer_visible(0) != 0)   // P363: C側の実値から初期化
    @State private var bg1Visible = (mx68k_get_bg_layer_visible(1) != 0)   // P363: C側の実値から初期化

    private func hex16(_ v: UInt16) -> String { String(format: "%04X", v) }

    private func boolLabel(_ title: String, _ on: Bool) -> some View {
        Label(title, systemImage: on ? "checkmark.circle.fill" : "xmark.circle")
            .foregroundColor(on ? .green : .secondary)
    }

    var body: some View {
        let s = emulatorViewModel.bgStatus
        VStack(alignment: .leading, spacing: 8) {
            Text("BG Status").font(.headline)
            Divider()
            HStack(spacing: 24) {
                boolLabel("BG0", s.bg0_on)
                boolLabel("BG1", s.bg1_on)
                Text("CHR Size: \(s.bg_chrsize)")
            }
            Divider()
            HStack(spacing: 24) {
                Toggle("BG0 表示", isOn: $bg0Visible)
                    .onChange(of: bg0Visible) { newValue in
                        mx68k_set_bg_layer_visible(0, newValue ? 1 : 0)
                    }
                Toggle("BG1 表示", isOn: $bg1Visible)
                    .onChange(of: bg1Visible) { newValue in
                        mx68k_set_bg_layer_visible(1, newValue ? 1 : 0)
                    }
            }
            Divider()
            HStack(spacing: 24) {
                Text("BG0 Scroll X: \(s.bg0_scroll_x)")
                Text("Y: \(s.bg0_scroll_y)")
            }
            HStack(spacing: 24) {
                Text("BG1 Scroll X: \(s.bg1_scroll_x)")
                Text("Y: \(s.bg1_scroll_y)")
            }
            Divider()
            HStack(spacing: 24) {
                Text("BG0 Top: \(hex16(s.bg0_top))")
                Text("BG1 Top: \(hex16(s.bg1_top))")
            }
            Divider()
            Text("Active Sprites: \(Int(s.sprite_active_count))")
            Spacer()
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 360, minHeight: 320, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.bgVisible = true }
        .onDisappear { emulatorViewModel.engine.bgVisible = false }
    }
}
