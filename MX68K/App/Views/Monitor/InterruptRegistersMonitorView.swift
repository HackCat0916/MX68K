import SwiftUI

/// P743 — 割込み系レジスタモニタ(MFP MC68901 / I/O コントローラ /
/// システムポート / CPU の現在割込みレベル)。
///
/// `Docs/01`§5.4.3 の債務表にあった「割込レベル 7-1(Device/Mask/Vector/Req/Ack)」
/// および「MFP / システムポート / I/O コントローラ」を 1 画面へ統合したもの。
/// MFP の IERA/IERB(Enable=Mask)・IPRA/IPRB(Pending=Request)・ISRA/ISRB
/// (In-Service=Ack)・IMRA/IMRB(Mask)・VR(Vector)が割込みレベル欄の実体である。
///
/// 全レジスタを生の 16 進値のまま表示する診断用ダンプ。ビット単位の意味的
/// デコード(どの割込み要因ビットがどのデバイスか等)は本サイクルのスコープ外
/// (デコードを行う場合は一次資料[MC68901 データシート / Inside X68000 等]の
///  裏取りと Fix Plan 記号表の記入が別途必要 —— P743 Fix Plan「記号表」節を参照)。
///
/// 更新経路は CRTC/VC/BG(P487)・DMAC(P742)モニタと同型: `.onAppear`/`.onDisappear`
/// で `engine.intRegsVisible` をトグルし、可視の間だけエミュレーションスレッドが
/// 毎フレーム `mx68k_get_int_regs_status()` でスナップショットして main へ push する。
struct InterruptRegistersMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    /// MFP[24] の添字に対応するレジスタ名(`Core/px68k/x68k/mfp.h` の
    /// `MFP_GPIP`(0)〜`MFP_UDR`(23) の `#define` 名をそのまま流用)。
    private static let mfpNames = [
        "GPIP", "AER", "DDR",
        "IERA", "IERB", "IPRA", "IPRB", "ISRA", "ISRB", "IMRA", "IMRB", "VR",
        "TACR", "TBCR", "TCDCR", "TADR", "TBDR", "TCDR", "TDDR",
        "SCR", "UCR", "RSR", "TSR", "UDR"
    ]

    var body: some View {
        let s = emulatorViewModel.intRegsStatus
        // C の固定長配列は Swift へタプルとして import されるため、
        // 添字アクセスできるよう生バイト列へ展開する。
        let mfp = withUnsafeBytes(of: s.mfp) { Array($0) }
        let sysport = withUnsafeBytes(of: s.sysport) { Array($0) }

        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                Text("Interrupt Registers").font(.headline)

                Divider()
                // 割込み関連(Mask / Request / In-Service / Vector)を先頭にまとめる。
                section("MFP — Interrupt") {
                    regRow(indices: [3, 4, 5, 6], mfp: mfp)     // IERA/IERB/IPRA/IPRB
                    regRow(indices: [7, 8, 9, 10], mfp: mfp)    // ISRA/ISRB/IMRA/IMRB
                    regRow(indices: [11], mfp: mfp)             // VR
                }

                Divider()
                section("MFP — GPIO") {
                    regRow(indices: [0, 1, 2], mfp: mfp)        // GPIP/AER/DDR
                }

                Divider()
                section("MFP — Timer") {
                    regRow(indices: [12, 13, 14], mfp: mfp)     // TACR/TBCR/TCDCR
                    regRow(indices: [15, 16, 17, 18], mfp: mfp) // TADR/TBDR/TCDR/TDDR
                }

                Divider()
                section("MFP — Serial") {
                    regRow(indices: [19, 20, 21, 22, 23], mfp: mfp) // SCR/UCR/RSR/TSR/UDR
                }

                Divider()
                section("I/O Controller") {
                    HStack(spacing: 14) {
                        Text(verbatim: "IntStat:\(h2(s.ioc_int_stat))")
                        Text(verbatim: "IntVect:\(h2(s.ioc_int_vect))")
                    }
                }

                Divider()
                section("System Port") {
                    HStack(spacing: 14) {
                        ForEach(0..<sysport.count, id: \.self) { i in
                            Text(verbatim: "[\(i)]:\(h2(sysport[i]))")
                        }
                    }
                }

                Divider()
                section("CPU") {
                    Text(verbatim: "IRQLine:\(s.cpu_irq_line)")
                }
            }
            .font(.system(.body, design: .monospaced))
            .padding()
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .frame(minWidth: 520, minHeight: 460, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.intRegsVisible = true }
        .onDisappear { emulatorViewModel.engine.intRegsVisible = false }
    }

    @ViewBuilder
    private func section<Content: View>(_ title: LocalizedStringKey,
                                        @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).bold()
            content()
        }
    }

    @ViewBuilder
    private func regRow(indices: [Int], mfp: [UInt8]) -> some View {
        HStack(spacing: 14) {
            ForEach(indices, id: \.self) { i in
                Text(verbatim: "\(Self.mfpNames[i]):\(h2(mfp[i]))")
            }
        }
    }

    private func h2(_ v: UInt8) -> String { String(format: "%02X", v) }
}
