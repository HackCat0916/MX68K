import SwiftUI

/// P287 — パフォーマンスモニタ。EmulatorEngine が 1 秒毎に集計する実測 fps・
/// 1 フレームあたりの処理時間(平均/最大)・予算超過フレーム数を表示する。
/// XM6 のスケジューラモニタ(Frame Rate / Over Cycle / Over Time)に相当。
struct PerformanceMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    var body: some View {
        let s = emulatorViewModel.perfStatus
        // 平均/最大処理時間が予算(1 フレームの目標時間)を超えていれば赤字で警告。
        let avgOver = s.budgetMs > 0 && s.avgFrameTimeMs > s.budgetMs
        let maxOver = s.budgetMs > 0 && s.maxFrameTimeMs > s.budgetMs
        VStack(alignment: .leading, spacing: 8) {
            Text("Performance").font(.headline)
            Divider()
            HStack(spacing: 24) {
                Text("Measured FPS: \(String(format: "%.1f", s.measuredFPS))")
                Text("Budget: \(String(format: "%.2f", s.budgetMs))ms")
            }
            Divider()
            Text("Avg frame time: \(String(format: "%.2f", s.avgFrameTimeMs))ms")
                .foregroundColor(avgOver ? .red : .primary)
            Text("Max frame time: \(String(format: "%.2f", s.maxFrameTimeMs))ms")
                .foregroundColor(maxOver ? .red : .primary)
            Divider()
            Text("Over budget: \(s.overBudgetFrameCount) / \(s.framesThisWindow) frames")
                .foregroundColor(s.overBudgetFrameCount > 0 ? .red : .primary)
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 320, minHeight: 220, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.perfVisible = true }
        .onDisappear { emulatorViewModel.engine.perfVisible = false }
    }
}
