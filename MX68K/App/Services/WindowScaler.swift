import AppKit

/// P190 — 表示メニューの ⌘1/⌘2/⌘3(ウィンドウスケール)実装。
/// P192 — 倍率を 1.0 / 1.5 / 2.0 に変更 + 復帰サイズのズレを 2 パス収束で修正。
enum WindowScaler {
    /// P212 — ウィンドウ採寸の基準は固定 4:3 面(標準テキスト 768×512 の物理 4:3)。
    /// framebuffer 実解像度(画面モードで可変)に依存すると ⌘1/1.5/2 のウィンドウサイズが
    /// モードで不安定になり、draw() の 4:3 viewport に対しレターボックス(黒帯)が出る。
    /// 固定 768×576 を 1x とし、⌘1/1.5/2 = 768×576 / 1152×864 / 1536×1152。
    static let baseLogicalSize = CGSize(width: 768, height: 576)

    /// エミュレータウィンドウの描画領域が baseLogicalSize × scale (pt) になるよう contentSize を設定。
    /// P192: chrome 高さはツールバー/ステータスバーの折返しでウィンドウ幅に依存して変わる
    /// (= 「1 回目は小さい・2 回目で正しくなる」の真因)。レイアウトを確定させて実測し直し
    /// 2 パスで収束させる。chrome が変わらなければ 2 パス目は同一 target で no-op(冪等)。
    static func applyScale(_ scale: Double) {
        for _ in 0..<2 {
            guard applyOnce(scale) else { return }
        }
    }

    @discardableResult
    private static func applyOnce(_ scale: Double) -> Bool {
        guard let window = EmulatorMTKView.hostWindow,
              let hostView = EmulatorMTKView.hostView,
              let content  = window.contentView,
              !window.styleMask.contains(.fullScreen)   // フルスクリーン中は no-op
        else { return false }
        // MTKView は contentView の直接の子ではない(NSHostingView サブツリー内)。
        // frame(superview 座標)ではなく convert で contentView 座標系の実サイズを得る。
        let viewSize = hostView.convert(hostView.bounds, to: content).size
        guard viewSize.width > 0, viewSize.height > 0 else { return false }
        let chromeW = max(0, content.frame.width  - viewSize.width)
        let chromeH = max(0, content.frame.height - viewSize.height)
        // P212: 固定 4:3 基準 × scale。framebuffer(px)には依存しない(NSWindow は pt)。
        let target = CGSize(width:  (baseLogicalSize.width  * scale).rounded() + chromeW,
                            height: (baseLogicalSize.height * scale).rounded() + chromeH)
        window.setContentSize(target)        // 画面からはみ出す場合は macOS が自動制約
        content.layoutSubtreeIfNeeded()      // 2 パス目が新しい chrome を実測できるように
        return true
    }

    /// P190b — ^⌘F のフルスクリーン切替。AppKit が標準項目を自動生成しなかったため
    /// 自前で提供する。対象はスケールと同じくエミュ画面のホストウィンドウ。
    static func toggleFullScreen() {
        EmulatorMTKView.hostWindow?.toggleFullScreen(nil)
    }

    // P193b: SwiftUI が NSWindow の autosave 名を握るため setFrameAutosaveName は効かない
    // (P193 実測: MX68KMainWindow キーが書かれず、setFrameUsingName が毎回 false を返し、
    //  「初回だけ」のはずの原寸フォールバックが毎回走ってサイズ復元を壊していた)。
    // AppKit の名前ベース autosave への依存をやめ、UserDefaults に自前キーで枠を保存する。
    private static let frameKey = "MX68KMainWindowFrame"
    private static var didRestoreFrame = false

    /// 前回終了時のウィンドウ枠を復元する。
    static func restoreFrame(for window: NSWindow) {
        guard !didRestoreFrame else { return }
        didRestoreFrame = true

        // ★M2: 赤ボタンで閉じると(最後のウィンドウを閉じた時点でアプリが終了するため)
        //      applicationWillTerminate より先にウィンドウが解放され、weak な hostWindow が
        //      nil になって保存に失敗しうる。閉じる直前にも保存する。
        NotificationCenter.default.addObserver(
            forName: NSWindow.willCloseNotification, object: window, queue: .main
        ) { note in
            if let w = note.object as? NSWindow { saveFrame(w) }
        }

        // SwiftUI/AppKit のウィンドウ確定処理の後に適用する(順序は P193 実測で実証済:
        // 我々の async ホップは SwiftUI の枠復元より後に着地する)。
        DispatchQueue.main.async { [weak window] in
            guard let window, !window.styleMask.contains(.fullScreen) else { return }
            guard let saved = UserDefaults.standard.string(forKey: frameKey) else {
                // ★M1: 初回のみ(以後は saveFrame が必ずキーを書くのでここは通らない)。
                // SwiftUI が復元した枠がタイル(≒ visibleFrame と同寸)ならタイルを解除する。
                // 無条件のリサイズは行わない — それが P193 でサイズ復元を壊した張本人。
                if let vf = window.screen?.visibleFrame,
                   window.frame.width  >= vf.width  - 1,
                   window.frame.height >= vf.height - 1 {
                    window.setContentSize(NSSize(width: 768, height: 576))
                    window.center()
                }
                return
            }
            let rect = NSRectFromString(saved)
            guard rect.width > 0, rect.height > 0 else { return }
            // 画面外(外部ディスプレイを外した等)なら寸法だけ使って中央に置く。
            let onScreen = NSScreen.screens.contains { $0.visibleFrame.intersects(rect) }
            if onScreen {
                window.setFrame(rect, display: true)   // constrainFrameRect が画面内に収める
            } else {
                window.setContentSize(rect.size)
                window.center()
            }
        }
    }

    /// ウィンドウ枠を保存する。フルスクリーン中は保存しない(次回が全画面枠になるのを防ぐ)。
    /// ★M2: window を引数で受け取り、weak 参照のタイミングに依存しない。
    static func saveFrame(_ window: NSWindow? = nil) {
        guard let window = window ?? EmulatorMTKView.hostWindow,
              !window.styleMask.contains(.fullScreen) else { return }
        UserDefaults.standard.set(NSStringFromRect(window.frame), forKey: frameKey)
    }
}
