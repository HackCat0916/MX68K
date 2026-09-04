import SwiftUI
import MetalKit
import Carbon

class EmulatorMTKView: MTKView {
    var inputManager: InputManager?
    private var trackingArea: NSTrackingArea?

    /// P196 — 現在の framebuffer 実寸(px)。絶対座標追従の写像に使う。
    /// X68KRenderer.textureWidth/Height は Metal スレッドが書くためイベント
    /// ハンドラから直読みするとデータ競合になる。ここへは updateTextureIfNeeded の
    /// DispatchQueue.main.async(メインスレッド)からのみ書き込む。
    var fbSize = CGSize(width: 768, height: 512)

    /// P595 (D-55) — 現在の表示ジオメトリ。マウス写像を draw() の矩形と一致させる
    /// ために使う。`fbSize` と全く同じ規約で、X68KRenderer.updateGeometry() の
    /// DispatchQueue.main.async(メインスレッド)からのみ書き込む
    /// (`EmulatorViewModel.displayGeometry` と同一値・同一 dispatch)。
    var displayGeom: DisplayGeometry = .identity

    /// P190 — エミュ画面をホストしているウィンドウ。⌘1/2/3 のスケール対象を
    /// これで特定する(NSApp.keyWindow だとモニタウィンドウにフォーカスが
    /// あるときそちらをリサイズしてしまうため)。
    static weak var hostWindow: NSWindow?
    /// P192 — 描画領域の実サイズを AppKit から同期的に測るための参照。
    /// (chrome 高さはウィンドウ幅に依存して変わるため、非同期 @Published では
    ///  リサイズ直後の実寸が得られない。)
    static weak var hostView: EmulatorMTKView?

    /// P237 — マウスキャプチャの安全弁(ウィンドウ単位)を一度だけ登録するためのガード。
    /// メインウィンドウは実質 1 個で viewDidMoveToWindow が同一 window で複数回呼ばれ
    /// うるため、インスタンス変数ではなく static で多重登録を防ぐ。
    static var focusObserverRegistered = false

    /// P682 — メインウィンドウの実クローズ連動処理を一度だけ登録するためのガード
    /// (`focusObserverRegistered` と同型)。
    static var closeObserverRegistered = false

    override var acceptsFirstResponder: Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let window = window {
            EmulatorMTKView.hostWindow = window
            EmulatorMTKView.hostView = self          // P192
            // P190b — .fullScreenPrimary が無いとウィンドウはフルスクリーン非対応扱いになり
            // ^⌘F が無効化される(AppKit が標準項目を出さなかった原因)。
            window.collectionBehavior.insert(.fullScreenPrimary)
            // P193 — 前回終了時の枠を安定キーで復元し、以後の自動保存を登録する。
            WindowScaler.restoreFrame(for: window)
            // P237 — フォーカス喪失でマウスキャプチャを強制解除する安全弁(ウィンドウ単位)。
            if !EmulatorMTKView.focusObserverRegistered {
                NotificationCenter.default.addObserver(
                    forName: NSWindow.didResignKeyNotification, object: window, queue: .main
                ) { _ in
                    InputManager.shared.forceReleaseMouseCaptureIfNeeded()
                }
                EmulatorMTKView.focusObserverRegistered = true
            }
            // P682: メインウィンドウが実際に閉じられたときだけ、エミュレータ本体以外の
            // 残存ウィンドウ(モニタ・Settings・Software Keyboard・標準Aboutパネル等)を
            // 連動して閉じる。P681では EmulatorView.onDisappear で行っていたが、
            // RootView の分岐(BIOS 未設定化)でも同じ .onDisappear が発火し、
            // メインウィンドウ自体は閉じていないのに全ウィンドウを巻き込んで
            // アプリごと終了してしまう回帰があった(R-1が現実化、ユーザー実機確認)。
            // NSWindow.willCloseNotification は「このウィンドウが実際に閉じる」ときにしか
            // 発火しないため、WindowScaler.restoreFrame と同じ手法で両者を区別する。
            if !EmulatorMTKView.closeObserverRegistered {
                NotificationCenter.default.addObserver(
                    forName: NSWindow.willCloseNotification, object: window, queue: .main
                ) { _ in
                    for other in NSApplication.shared.windows where other !== window {
                        other.close()
                    }
                }
                EmulatorMTKView.closeObserverRegistered = true
            }
        }
        window?.makeFirstResponder(self)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let old = trackingArea {
            removeTrackingArea(old)
        }
        let newArea = NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],   // P196(S4): 排他の active オプション 2 つを是正
            owner: self,
            userInfo: nil
        )
        addTrackingArea(newArea)
        trackingArea = newArea
    }

    override func keyDown(with event: NSEvent) {
        inputManager?.handleKeyDown(event)
    }

    override func flagsChanged(with event: NSEvent) {
        inputManager?.handleFlagsChanged(event)
    }

    override func keyUp(with event: NSEvent) {
        inputManager?.handleKeyUp(event)
    }

    override func mouseMoved(with event: NSEvent) {
        forwardMouseMove(event)
    }

    // P196(M4): ドラッグ中の移動は今まで未配線でボタン押下中の移動が届いていなかった。
    // mouseMoved と同じ写像経路に流す(ドラッグは first responder に直接届く)。
    override func mouseDragged(with event: NSEvent) {
        forwardMouseMove(event)
    }

    override func rightMouseDragged(with event: NSEvent) {
        forwardMouseMove(event)
    }

    private func forwardMouseMove(_ event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)   // ビュー座標(左下原点)
        inputManager?.handleMouseMoved(event, viewPoint: p, viewSize: bounds.size,
                                       framebuffer: fbSize, geom: displayGeom)
    }

    override func mouseDown(with event: NSEvent) {
        inputManager?.handleMouseDown(button: 0)
    }

    override func mouseUp(with event: NSEvent) {
        inputManager?.handleMouseUp(button: 0)
    }

    override func rightMouseDown(with event: NSEvent) {
        inputManager?.handleMouseDown(button: 1)
    }

    override func rightMouseUp(with event: NSEvent) {
        inputManager?.handleMouseUp(button: 1)
    }

    override func resetCursorRects() {
        super.resetCursorRects()
        addCursorRect(bounds, cursor: .crosshair)
    }
}

// P703: `Vertex` / `DisplayGeometry` / `DisplayViewport` は
// App/Views/MainWindow/DisplayGeometry.swift へ、`X68KRenderer` は
// App/Views/MainWindow/X68KRenderer.swift へ切り出した(内容は無改変の再配置)。
// このファイルには AppKit と不可分な `EmulatorMTKView` / `EmulatorMetalView` と、
// macOS の設定 UI 専用の `DisplayFilter` だけが残る = macOS ターゲット専用のまま。

struct EmulatorMetalView: NSViewRepresentable {
    // P194: 共有インスタンスを参照(観測しない — EmulatorView と同じ理由)。
    private let inputManager = InputManager.shared
    @EnvironmentObject var viewModel: EmulatorViewModel

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    class Coordinator {
        var renderer: X68KRenderer?
    }

    func makeNSView(context: Context) -> MTKView {
        guard let device = MTLCreateSystemDefaultDevice() else {
            fatalError("Metal is not supported")
        }

        let mtkView = EmulatorMTKView(frame: .zero, device: device)
        mtkView.colorPixelFormat = .bgra8Unorm
        mtkView.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        mtkView.enableSetNeedsDisplay = false
        mtkView.isPaused = false

        guard let renderer = X68KRenderer(device: device) else {
            fatalError("Failed to create X68KRenderer (Metal library missing?)")
        }
        renderer.viewModel = viewModel
        context.coordinator.renderer = renderer
        mtkView.delegate = renderer
        mtkView.inputManager = inputManager

        return mtkView
    }

    func updateNSView(_ nsView: MTKView, context: Context) {
        if let mtkView = nsView as? EmulatorMTKView {
            mtkView.inputManager = inputManager
        }
        context.coordinator.renderer?.viewModel = viewModel
    }
}

/// P199 — 表示フィルタの単一情報源。UserDefaults キー "displayFilter"・rawValue は
/// "smooth"/"sharp"(レンダラ draw() の直読みと一致)。ラベルは String Catalog キーで返す。
enum DisplayFilter: String, CaseIterable, Identifiable {
    case smooth, sharp
    var id: String { rawValue }
    var labelKey: LocalizedStringKey {
        self == .smooth ? "表示フィルタ・スムーズ" : "表示フィルタ・くっきり"
    }
}

/// P703 — macOS 側の RendererHost 適合。`framebufferSize` / `displayGeometry` は
/// EmulatorViewModel が以前から持つ `@Published` プロパティそのもので、
/// 追加のメンバは要らない。★このファイルは macOS ターゲット専用なので、
/// iOS ターゲットがこの extension を見ることはない。
extension EmulatorViewModel: RendererHost {}
