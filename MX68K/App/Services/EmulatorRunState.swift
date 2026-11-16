import Foundation

/// P546(D-10根本修正): メニューバーの実行状態ゲート判定に必要な
/// isRunning/powerState のみを公開する専用オブジェクト。EmulatorViewModel
/// 全体を観測すると status 等が毎秒更新されるため Commands 全体の再評価を
/// 招く(MouseCaptureState.swift・P237 と同じ理由)。
final class EmulatorRunState: ObservableObject {
    static let shared = EmulatorRunState()
    @Published var isRunning = false
    @Published var powerState: PowerState = .off
    /// P697 — 録画中か。メニュー項目のラベル切替(Start/Stop Recording)と
    /// ターボ項目の `.disabled` 判定に使う。★EmulatorViewModel を直接観測すると
    /// status 等の毎秒更新で Commands 全体が再評価される(D-10/D-49)ため、
    /// isRunning/powerState と同じ軽量ミラーとしてここに置く。
    @Published var isRecording = false
    /// P737 — クイックセーブ/ロード操作の実行中か。Quick Save/Quick Load State
    /// メニュー項目の`.disabled`判定に使う。isRecordingと同じ理由の軽量ミラー。
    @Published var isStateOperationPending = false
    /// P1046 — 外付けFDDユニット設定の軽量ミラー。FD2/FD3メニューの`.disabled`
    /// 判定に使う。isRecordingと同じ理由(EmulatorViewModel直接観測を避ける)。
    @Published var externalFDDUnitEnabled = false
    /// P1108 (D-128) — ふろっぴU君に割当て中のドライブのビットマスク
    /// (bit0=FD0 .. bit3=FD3)。FDメニューの`.disabled`判定に使う軽量ミラー。
    /// 更新側は値が変わったときだけ代入すること(D-49)。
    @Published var floppyuAssignedMask: UInt8 = 0
}
