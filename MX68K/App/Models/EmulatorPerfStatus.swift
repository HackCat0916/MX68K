import Foundation

/// P287 — パフォーマンスモニタ用の実測値。EmulatorEngine.runFrame() が 1 秒
/// ウィンドウ毎に集計して発行する純 Swift 型(Bridge の C 型とは独立)。
struct EmulatorPerfStatus {
    var measuredFPS: Double = 0
    var avgFrameTimeMs: Double = 0
    var maxFrameTimeMs: Double = 0
    var budgetMs: Double = 0
    var overBudgetFrameCount: Int = 0
    var framesThisWindow: Int = 0
}
