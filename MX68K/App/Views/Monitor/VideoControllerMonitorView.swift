import SwiftUI

struct VideoControllerMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    private func hex8(_ v: UInt8) -> String { String(format: "%02X", v) }

    private func boolLabel(_ title: String, _ on: Bool) -> some View {
        Label(title, systemImage: on ? "checkmark.circle.fill" : "xmark.circle")
            .foregroundColor(on ? .green : .secondary)
    }

    private func modeName(_ mode: Int32) -> String {
        switch mode {
        case 0:  return "16-color"
        case 1, 2: return "256-color"
        case 3:  return "65536-color"
        default: return "\(mode)"
        }
    }

    var body: some View {
        let s = emulatorViewModel.vcStatus
        VStack(alignment: .leading, spacing: 8) {
            Text("Video Controller Status").font(.headline)
            Divider()
            HStack(spacing: 24) {
                Text("VCReg0: \(hex8(s.vc_reg0_0)) \(hex8(s.vc_reg0_1))")
                Text("VCReg1: \(hex8(s.vc_reg1_0)) \(hex8(s.vc_reg1_1))")
                Text("VCReg2: \(hex8(s.vc_reg2_0)) \(hex8(s.vc_reg2_1))")
            }
            Divider()
            Text("Mode: \(modeName(s.mode))")
            HStack(spacing: 24) {
                Text("Pri GR: \(s.pri_gr)")
                Text("Pri TX: \(s.pri_tx)")
                Text("Pri SP: \(s.pri_sp)")
            }
            Divider()
            HStack(spacing: 24) {
                boolLabel("Text", s.text_on)
                boolLabel("Sprite/BG", s.sp_on)
            }
            boolLabel("SP special-priority (256c page0)", s.sp_cond_256)
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 360, minHeight: 280, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.vcVisible = true }
        .onDisappear { emulatorViewModel.engine.vcVisible = false }
    }
}
