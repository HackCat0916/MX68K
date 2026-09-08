import SwiftUI

/// P742 — DMAC(HD63450 / MC68450 相当)レジスタモニタ。
///
/// 4チャンネル分の 17 レジスタを生の 16 進値のまま表示する診断用ダンプ。
/// ビット単位の意味的デコード(STR/CE/OPE 等)は本サイクルのスコープ外
/// (デコードを行う場合は一次資料[MC68450 データシート等]の裏取りと
///  Fix Plan 記号表の記入が別途必要 —— P742 Fix Plan「記号表」節を参照)。
///
/// 更新経路は CRTC/VC/BG モニタ(P487)と同型: `.onAppear`/`.onDisappear` で
/// `engine.dmacVisible` をトグルし、可視の間だけエミュレーションスレッドが
/// 毎フレーム `mx68k_get_dmac_status()` でスナップショットして main へ push する。
struct DMACMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    var body: some View {
        let s = emulatorViewModel.dmacStatus
        // C の固定長配列 ch[4] は Swift へタプルとして import されるため、
        // ForEach で回せるよう配列へ展開する。
        let channels = [s.ch.0, s.ch.1, s.ch.2, s.ch.3]
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                Text("DMAC Registers").font(.headline)
                ForEach(0..<4, id: \.self) { i in
                    Divider()
                    channelSection(index: i, c: channels[i])
                }
            }
            .font(.system(.body, design: .monospaced))
            .padding()
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .frame(minWidth: 560, minHeight: 420, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.dmacVisible = true }
        .onDisappear { emulatorViewModel.engine.dmacVisible = false }
    }

    @ViewBuilder
    private func channelSection(index: Int, c: MX68KDMACChannelStatus) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(verbatim: "CH\(index)").bold()
            HStack(spacing: 14) {
                Text(verbatim: "CSR:\(h2(c.csr))")
                Text(verbatim: "CER:\(h2(c.cer))")
                Text(verbatim: "DCR:\(h2(c.dcr))")
                Text(verbatim: "OCR:\(h2(c.ocr))")
                Text(verbatim: "SCR:\(h2(c.scr))")
                Text(verbatim: "CCR:\(h2(c.ccr))")
            }
            HStack(spacing: 14) {
                Text(verbatim: "MTC:\(h4(c.mtc))")
                Text(verbatim: "MAR:\(h8(c.mar))")
                Text(verbatim: "DAR:\(h8(c.dar))")
            }
            HStack(spacing: 14) {
                Text(verbatim: "BTC:\(h4(c.btc))")
                Text(verbatim: "BAR:\(h8(c.bar))")
            }
            HStack(spacing: 14) {
                Text(verbatim: "NIV:\(h2(c.niv))")
                Text(verbatim: "EIV:\(h2(c.eiv))")
                Text(verbatim: "MFC:\(h2(c.mfc))")
                Text(verbatim: "CPR:\(h2(c.cpr))")
                Text(verbatim: "DFC:\(h2(c.dfc))")
                Text(verbatim: "BFC:\(h2(c.bfc))")
                Text(verbatim: "GCR:\(h2(c.gcr))")
            }
        }
    }

    private func h2(_ v: UInt8)  -> String { String(format: "%02X", v) }
    private func h4(_ v: UInt16) -> String { String(format: "%04X", v) }
    private func h8(_ v: UInt32) -> String { String(format: "%08X", v) }
}
