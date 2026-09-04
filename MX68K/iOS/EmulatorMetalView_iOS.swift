//
//  EmulatorMetalView_iOS.swift
//  MX68K-iOS
//
//  P703: iOS 側の Metal 表示ビュー。macOS の `EmulatorMetalView`(NSViewRepresentable)
//  に対応する UIViewRepresentable。描画そのものは macOS と同じ共有
//  `X68KRenderer` が行う。
//
//  P705: MTKView をサブクラス化した(`EmulatorMTKView_iOS`)。P703 の時点では
//        「本サイクルは入力を扱わないため、イベント処理を載せる先が無い」と
//        して意図的にサブクラス化していなかったが、本サイクルがまさにその
//        想定トリガである。macOS の `EmulatorMTKView`
//        (EmulatorMetalView.swift:5, 39, 41-79, 97-107)を 1:1 で写す。
//

import SwiftUI
import UIKit
import Metal
import MetalKit

/// P705: キーイベントを受けるための `MTKView` サブクラス。
/// macOS 側 `EmulatorMTKView` の iOS 対応物(`acceptsFirstResponder` →
/// `canBecomeFirstResponder`、`viewDidMoveToWindow` → `didMoveToWindow`、
/// `keyDown`/`keyUp` → `pressesBegan`/`pressesEnded`)。
///
/// ★`UIPress` は**ヒットテストではなくレスポンダチェーン**を辿って配送される。
///   したがって SwiftUI 側の `.allowsHitTesting` やビューの重なり順は
///   キー入力の可否と**無関係**である。将来「効かないのはヒットテストの
///   せいでは」と疑われないよう、この事実をここに残す(P705 計画 §C)。
final class EmulatorMTKView_iOS: MTKView {

    /// `UIPress` → X68000 スキャンコードの写像と押下状態の保持。
    let keyboard = IOSKeyboardInput()

    /// レスポンダ再取得の失敗ログが毎更新で溢れないようにする上限。
    /// (`updateUIView` は ViewModel の @Published 変化のたびに呼ばれ得る。)
    private var responderRetryLogsLeft = 5

    /// アプリのバックグラウンド遷移を観測するトークン(下の説明を参照)。
    private var backgroundObserver: NSObjectProtocol?

    override var canBecomeFirstResponder: Bool { true }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        let ok = becomeFirstResponder()
        mx68k_log("[Swift][iOS] becomeFirstResponder -> \(ok) "
                + "isFirstResponder=\(isFirstResponder) hasWindow=\(window != nil)")
        installBackgroundObserverIfNeeded()
    }

    /// `didMoveToWindow` の時点でウィンドウ階層がまだ受け付けない場合の保険
    /// (P705 計画 §C / 残留リスク R-7)。取得済みなら何もしない。
    func ensureFirstResponder() {
        guard window != nil, !isFirstResponder else { return }
        let ok = becomeFirstResponder()
        if responderRetryLogsLeft > 0 {
            responderRetryLogsLeft -= 1
            mx68k_log("[Swift][iOS] retry becomeFirstResponder -> \(ok) "
                    + "isFirstResponder=\(isFirstResponder) "
                    + "(further retry logs suppressed after \(responderRetryLogsLeft) more)")
        }
    }

    // MARK: - 押しっぱなし解除(アプリライフサイクル)
    //
    // ★T-0e(2026-08-29, ユーザー hands-on)で、アプリをバックグラウンドへ送っても
    //   `pressesCancelled` / `resignFirstResponder` の**どちらも発火しない**ことが
    //   実測された。UIKit のレスポンダチェーン経由の 2 機構だけに頼ると、
    //   キーを押したまま Home / アプリ切替をした場合にゲスト側でキーが
    //   「押しっぱなし」のまま残る(R-4)。
    //   したがって**主たる保証は明示的なアプリライフサイクル監視**に置く。
    //
    // ★`@Environment(\.scenePhase)`(SwiftUI 側で監視)ではなく
    //   `UIApplication.didEnterBackgroundNotification` を選んだ理由:
    //   `IOSKeyboardInput` の実体を**所有しているのはこのビュー**であり、
    //   `MX68KiOSRootView` / `MX68KiOSViewModel` はその参照を持たない。
    //   scenePhase 方式では「ビュー → ViewModel → キーボード」という参照の
    //   引き回しを新設する必要があり、iOS 側 ViewModel を意図的に薄く保つ
    //   P703 の設計方針(§0-2 の非ゴール)に逆行する。所有者自身が観測して
    //   直接 releaseAll を呼ぶ方が結線が 1 本で済み、寿命管理も deinit で閉じる。
    //
    // ★`willResignActiveNotification` は**あえて観測しない** —— 通知バナーや
    //   コントロールセンターの引き下ろしでも発火するため、まだ実際に押している
    //   キーを不意に上げてしまう。計画が要求しているのは「バックグラウンド遷移」である。

    private func installBackgroundObserverIfNeeded() {
        // `didMoveToWindow` はウィンドウの着脱で複数回呼ばれ得るので二重登録を防ぐ。
        guard backgroundObserver == nil else { return }
        backgroundObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.didEnterBackgroundNotification,
            object: nil,
            queue: .main            // ★mx68k_key_up はメインスレッドから(§B-7)
        ) { [weak self] _ in
            self?.keyboard.releaseAll(reason: "didEnterBackground")
            // P714 — 仮想パッドも同じ契機で押しっぱなしを解除する。
            // ★`keyboard` と違い所有者はこのビューではない(SwiftUI のオーバーレイと
            //   共有するシングルトン)ため `self` を介さず直接呼ぶ。
            TouchJoystickInput.shared.releaseVirtualPad(reason: "didEnterBackground")
        }
    }

    deinit {
        if let token = backgroundObserver {
            NotificationCenter.default.removeObserver(token)
        }
    }

    // MARK: - キーイベント
    //
    // ★自分が処理できなかった press だけを `super` へ転送する(全部飲み込まない)。
    //   飲み込むと、キーイベントに依存する OS 側の挙動まで殺してしまう。

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let unhandled = keyboard.handle(phase: .began, presses: presses)
        // ★一時診断ログ(入力後退調査、調査完了後に削除)
        mx68k_log("[Swift][iOS][DIAG-INPUT] pressesBegan count=\(presses.count) "
                + "unhandled=\(unhandled.count) isFirstResponder=\(isFirstResponder)")
        if !unhandled.isEmpty { super.pressesBegan(unhandled, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let unhandled = keyboard.handle(phase: .ended, presses: presses)
        if !unhandled.isEmpty { super.pressesEnded(unhandled, with: event) }
    }

    /// 補助経路(T-0e により、これ自体は当てにできないことが実測されている)。
    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let unhandled = keyboard.handle(phase: .cancelled, presses: presses)
        keyboard.releaseAll(reason: "pressesCancelled")
        TouchJoystickInput.shared.releaseVirtualPad(reason: "pressesCancelled")   // P714
        if !unhandled.isEmpty { super.pressesCancelled(unhandled, with: event) }
    }

    /// 補助経路(同上)。
    override func resignFirstResponder() -> Bool {
        keyboard.releaseAll(reason: "resignFirstResponder")
        TouchJoystickInput.shared.releaseVirtualPad(reason: "resignFirstResponder")   // P714
        return super.resignFirstResponder()
    }
}

struct EmulatorMetalView_iOS: UIViewRepresentable {
    /// ★`@ObservedObject` にはしない —— このビューが映すのはレンダラが直接
    /// テクスチャへ書くフレームだけで、ViewModel の @Published 変化のたびに
    /// updateUIView を回す必要が無い(状態表示は親ビューが担当する)。
    let viewModel: MX68KiOSViewModel

    /// ★一時診断ログ(入力後退調査、調査完了後に削除)用の updateUIView 呼び出し回数。
    /// 実運用ではインスタンスは常に 1 つなので struct の static で足りる。
    static var updateCallCount = 0

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator {
        var renderer: X68KRenderer?
    }

    /* ★P705: 返り値型を `MTKView` から `EmulatorMTKView_iOS` へ**狭める**。
     *   `updateUIView` でレスポンダ再取得(R-7)を呼ぶ必要があるため、
     *   `as?` キャストを増やすより型を正確にする方を選ぶ(P705 計画 §C)。 */
    func makeUIView(context: Context) -> EmulatorMTKView_iOS {
        /* ★macOS 側(EmulatorMetalView.makeNSView)は失敗時に fatalError を呼ぶが、
         * iOS では metallib の同梱漏れが現実的な失敗モード(P703 §残留リスク R-1)で
         * あり、プログラマのミスではない。クラッシュさせず、画面に出せる形で
         * 理由を報告する。失敗理由(device / library / function のどれか)は
         * mx68k_log() へも残す —— 画面が黒いだけでは複数の原因が区別できない。 */
        guard let device = MTLCreateSystemDefaultDevice() else {
            report("Metal initialization failed: no Metal device "
                 + "(MTLCreateSystemDefaultDevice returned nil)")
            /* ★Metal が死んでいてもキー入力の配線は成立する(依存しているのは
             *   レスポンダチェーンであって描画ではない)。よってこの経路でも
             *   同じサブクラスを返す。 */
            return configured(EmulatorMTKView_iOS(frame: .zero, device: nil))
        }

        let mtkView = configured(EmulatorMTKView_iOS(frame: .zero, device: device))

        guard let renderer = X68KRenderer(device: device) else {
            report("Metal initialization failed: \(rendererFailureReason(device))")
            return mtkView
        }
        renderer.viewModel = viewModel
        context.coordinator.renderer = renderer
        mtkView.delegate = renderer
        return mtkView
    }

    func updateUIView(_ uiView: EmulatorMTKView_iOS, context: Context) {
        // ViewModel インスタンスが差し替わった場合に備えて publish 先を更新する
        // (本サイクルの使い方では起こらないが、macOS 側 updateNSView と同じ規約)。
        context.coordinator.renderer?.viewModel = viewModel
        // ★一時診断ログ(入力後退調査、調査完了後に削除)
        Self.updateCallCount += 1
        mx68k_log("[Swift][iOS][DIAG-INPUT] updateUIView call=\(Self.updateCallCount) "
                + "isFirstResponder=\(uiView.isFirstResponder) hasWindow=\(uiView.window != nil)")
        // P705 §C / R-7: didMoveToWindow で取れていなければ再試行する(取得済みなら no-op)。
        uiView.ensureFirstResponder()
    }

    /// macOS 側と同一の MTKView 設定(黒クリア・連続描画)。
    private func configured<V: MTKView>(_ view: V) -> V {
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        return view
    }

    /// `X68KRenderer.init?` は失敗理由を返さない(既存設計、macOS と共有のため
    /// 変更しない)ので、同じ判定をここで独立に行って理由を切り分ける。
    private func rendererFailureReason(_ device: MTLDevice) -> String {
        guard let library = device.makeDefaultLibrary() else {
            return "makeDefaultLibrary() returned nil (no metallib in the app bundle?)"
        }
        let missing = ["x68kVertex", "x68kFragment"].filter { library.makeFunction(name: $0) == nil }
        if !missing.isEmpty {
            return "shader function(s) not found: \(missing.joined(separator: ", ")) "
                 + "(library has: \(library.functionNames.sorted().joined(separator: ", ")))"
        }
        return "pipeline/sampler creation failed (device: \(device.name))"
    }

    /// ★`makeUIView` は SwiftUI のビュー更新中に呼ばれるため、@Published を
    /// その場で書くと「Modifying state during view update」になる。非同期に回す。
    private func report(_ message: String) {
        mx68k_log("[Swift][iOS] \(message)")
        let vm = viewModel
        DispatchQueue.main.async {
            vm.metalError = message
        }
    }
}
