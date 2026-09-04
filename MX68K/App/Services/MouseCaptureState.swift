import Foundation

/// P237: マウスキャプチャ状態のみを公開する専用オブジェクト。InputManager
/// 全体を観測すると keyboardState(キー入力毎に変化)まで拾ってしまうため
/// (EmulatorView.swift:6-9 が同じ理由で InputManager 直接観測を避けている)、
/// メニューラベルの反応にはこちらだけを observe する。
final class MouseCaptureState: ObservableObject {
    static let shared = MouseCaptureState()
    @Published var isCaptured = false
}
