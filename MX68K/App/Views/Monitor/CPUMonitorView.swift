import SwiftUI

struct CPUMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    private func hex32(_ v: UInt32) -> String { String(format: "%08X", v) }
    private func hex16(_ v: UInt16) -> String { String(format: "%04X", v) }

    var body: some View {
        let s = emulatorViewModel.status
        // ★Code Review #1 反映: MX68KStatus.d/a は C の uint32_t[8] → Swift ではタプル
        // (UInt32,…×8) としてインポートされ添字 [i] が使えない。明示的に配列化する。
        let dRegs = [s.d.0, s.d.1, s.d.2, s.d.3, s.d.4, s.d.5, s.d.6, s.d.7]
        let aRegs = [s.a.0, s.a.1, s.a.2, s.a.3, s.a.4, s.a.5, s.a.6, s.a.7]
        // P408: 派生値。分母が 0(未計測)のときは計算せず "—" を表示する。
        let lineBudgetText: String = s.vline_total > 0
            ? String(Int(s.clk_total) / Int(s.vline_total))
            : "—"
        VStack(alignment: .leading, spacing: 8) {
            Text("CPU Status").font(.headline)
            Divider()
            HStack(spacing: 24) {
                Text("PC: \(hex32(s.pc))")
                Text("SR: \(hex16(s.sr))")
                if s.paused { Text("⏸ PAUSED").foregroundColor(.orange) }
            }
            Divider()
            // D0-D7 / A0-A7 を 8 行 2 列で
            Grid(alignment: .leading, horizontalSpacing: 24, verticalSpacing: 2) {
                ForEach(0..<8, id: \.self) { i in
                    GridRow {
                        Text("D\(i): \(hex32(dRegs[i]))")
                        Text("A\(i): \(hex32(aRegs[i]))")
                    }
                }
            }
            Divider()
            HStack(spacing: 24) {
                Text("USP: \(hex32(s.usp))")
                Text("ISP: \(hex32(s.isp))")
            }
            Divider()
            HStack(spacing: 24) {
                Text("Machine: \(s.machineDisplayName)")
                Text("Clock: \(Int(s.clock_mhz))MHz")
                Text("MEM: \(Int(s.memory_mb))MB")
                Text("FPU: \(s.fpu_enabled ? String(localized: "Yes") : String(localized: "No"))")
            }
            HStack(spacing: 24) {
                Label("FDD0", systemImage: s.fdd0_inserted ? "opticaldisc.fill" : "opticaldisc")
                    .foregroundColor(s.fdd0_active ? .red : (s.fdd0_inserted ? .primary : .secondary))
                Label("FDD1", systemImage: s.fdd1_inserted ? "opticaldisc.fill" : "opticaldisc")
                    .foregroundColor(s.fdd1_active ? .red : (s.fdd1_inserted ? .primary : .secondary))
                // P689: ドライブ 2/3 は fdd2/3_inserted を持たず、挿入状態は
                // media_present(P443 設計)で表す。
                Label("FDD2", systemImage: s.fdd2_media_present ? "opticaldisc.fill" : "opticaldisc")
                    .foregroundColor(s.fdd2_active ? .red : (s.fdd2_media_present ? .primary : .secondary))
                Label("FDD3", systemImage: s.fdd3_media_present ? "opticaldisc.fill" : "opticaldisc")
                    .foregroundColor(s.fdd3_active ? .red : (s.fdd3_media_present ? .primary : .secondary))
            }
            Divider()
            // P408: 実行粒度(CLOCK_SLICE)の可視化。読み取り専用の実測値表示。
            // 派生値 line_budget は元になった生値 (clk_total / vline_total) を
            // 必ず同一画面に併記する(Referent-binding 規律)。
            VStack(alignment: .leading, spacing: 2) {
                Text(String(localized: "Execution Granularity"))
                    .foregroundColor(.secondary)
                HStack(spacing: 24) {
                    Text("CLOCK_SLICE: \(Int(s.clock_slice))")
                    Text("clkdiv: \(Int(s.clkdiv))")
                }
                HStack(spacing: 24) {
                    Text("line_budget: \(lineBudgetText)")
                    Text("(clk_total: \(Int(s.clk_total)) / vline_total: \(Int(s.vline_total)))")
                        .foregroundColor(.secondary)
                }
                HStack(spacing: 24) {
                    Text("chunks/frame: \(Int(s.chunks_last_frame))")
                    Text("cum_zero: \(Int(s.cum_zero))")
                }
                Text(String(localized: "CLOCK_SLICE = max CPU cycles per execution chunk (default = upstream px68k value). line_budget = clk_total / vline_total."))
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 380, minHeight: 460, alignment: .topLeading)   // P408: 新セクション分
    }
}

extension MX68KStatus {
    /// 機種のSASI/SCSI区分表示名(ステータスバー・モニタで共通使用)。
    /// P449: 実際にBridgeへ配線される値は0(SASI)/4(SCSI)の2値のみ
    /// (EmulatorViewModel.machineTypeValue()参照)なので、それ以外の
    /// 具体的モデル名を装った表示は実態と一致しない死にコードだった。
    var machineDisplayName: String {
        machine_type == 4 ? "X68000 (SCSI)" : "X68000 (SASI)"
    }
}
