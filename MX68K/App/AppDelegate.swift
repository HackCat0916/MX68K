import AppKit
import Foundation
import Dispatch

/// P53 — NSApplicationDelegate that wires graceful termination paths
/// (Cmd-Q, menu Quit, dock Quit, SIGTERM, SIGINT) into the C bridge so
/// that mx68k_shutdown / atexit summary actually run.
///
/// File name matches Docs/02_プロジェクト構造.md:100 (`AppDelegate.swift`).
/// Class name retained as `MX68KAppDelegate` for MX68K* project naming
/// symmetry.
///
/// Shutdown chain (every graceful path):
///   signal/Cmd-Q
///     -> applicationWillTerminate
///        -> mx68k_log_delegate_fire()                  (C bridge -> debug.log)
///        -> EmulatorViewModel.shared.stopEmulation()   (engine.stop -> audio teardown -> mx68k_shutdown)
///     -> AppKit exit(3)
///     -> atexit -> mx68k_atexit_summary()              (P52-SUMMARY etc.)
///
/// See /tmp/mx68k_P53_plan.md §7 Edit C / §12.3.
final class MX68KAppDelegate: NSObject, NSApplicationDelegate {

    /// Strong references required — DispatchSourceSignal is cancelled when
    /// it goes out of scope (Spec Inv §C).
    private var sigtermSource: DispatchSourceSignal?
    private var sigintSource:  DispatchSourceSignal?

    /// P193 — SwiftUI が型名由来キーで保存した旧ウィンドウ枠を一度だけ破棄する掃除処理。
    ///
    /// ★P193b 訂正: 「これでタイリング状態(Fill)の再流入を防げる」という P193 の前提は
    /// **誤り**だった。SwiftUI は終了のたびに型名由来キーを再生成し、そこへ tilingState を
    /// 書き戻す(実測で確認済)。一度きり・移行フラグ付きの掃除では再発を防げない。
    /// 起動時に最大化されないことを保証しているのは、この掃除ではなく
    /// WindowScaler.restoreFrame が保存枠で無条件に setFrame し直す動作である。
    ///
    /// 移行フラグ("P193_frameKeysMigrated")は既に立っているため、以下は二度と走らない
    /// = 実質的な dead code。古い環境での一度きりの後始末としてのみ残置している。
    /// 同じ誤った前提を繰り返さないこと。
    func applicationWillFinishLaunching(_ notification: Notification) {
        let migrationKey = "P193_frameKeysMigrated"
        let defaults = UserDefaults.standard
        guard !defaults.bool(forKey: migrationKey) else { return }
        // 削除対象は "NSWindow Frame SwiftUI." 接頭辞のみ(型名由来キー)。
        // "NSWindow Frame monitor-cpu" / "…com_apple_SwiftUI_Settings_window" は
        // 接頭辞が異なるため残る(モニタ/設定ウィンドウの復元は壊さない)。
        for key in defaults.dictionaryRepresentation().keys
        where key.hasPrefix("NSWindow Frame SwiftUI.") {
            defaults.removeObject(forKey: key)
        }
        defaults.set(true, forKey: migrationKey)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // P53 ablation: master + sigsrc switch. When 0, SIGTERM reverts to
        // default disposition.
        guard mx68k_p53_sigsrc_enabled() != 0 else { return }

        // SIGTERM — primary CI-equivalent path (CLAUDE.md launch test).
        // SIG_IGN first to avoid the default termination racing the source.
        signal(SIGTERM, SIG_IGN)
        let term = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        term.setEventHandler { [weak self] in
            self?.logSig("SIGTERM")
            NSApp.terminate(nil)
        }
        term.resume()
        self.sigtermSource = term

        // SIGINT — secondary (Ctrl-C in foreground). Cheap to keep installed
        // even on nohup paths (R-3 acknowledged).
        signal(SIGINT, SIG_IGN)
        let intSrc = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
        intSrc.setEventHandler { [weak self] in
            self?.logSig("SIGINT")
            NSApp.terminate(nil)
        }
        intSrc.resume()
        self.sigintSource = intSrc
    }

    func applicationWillTerminate(_ notification: Notification) {
        WindowScaler.saveFrame()      // P193b: 次回起動でこの枠を復元する

        // P53 ablation: master + appdelegate switch.
        guard mx68k_p53_appdelegate_enabled() != 0 else { return }

        // (Code Review C-2) Route the marker through the C bridge so the
        // tag actually lands in ~/Library/Application Support/MX68K/debug.log.
        // NSLog would only hit Apple Unified Logging.
        mx68k_log_delegate_fire()

        // (Code Review C-1) Halt the CVDisplayLink thread BEFORE
        // mx68k_shutdown frees MEM/IPL/FONT. The existing stopEmulation()
        // sequences engine.stop -> audio teardown -> mx68k_shutdown, so we
        // re-use it. C-side `s_p53_shutdown_done` + Swift-side `isRunning`
        // guard absorb duplicate calls from .onDisappear.
        //
        // N-1 (P54-tracked): CVDisplayLinkStop is async; an in-flight
        // callback can race with free(MEM) for ~16 ms. The audio teardown
        // sandwich (~150 ms) provides incidental delay; formal sync via an
        // atomic shutdown flag is deferred to P54.
        if let vm = EmulatorViewModel.shared {
            vm.stopEmulation()
        } else {
            // Settings-only path: no engine ever started, MEM/IPL/FONT
            // were never allocated. free(NULL) is C-spec no-op, so calling
            // mx68k_shutdown directly here is safe.
            mx68k_shutdown()
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return true
    }

    /// P237 — マウスキャプチャの安全弁(アプリ単位)。⌘Tab で他アプリへ切り替える
    /// 最も起こりやすいケースを、ウィンドウ単位の経路とは独立にカバーする。
    /// CGAssociateMouseAndMouseCursorPosition(0) はシステム全体に効くため、
    /// 単一経路に頼らず二重化する。
    func applicationDidResignActive(_ notification: Notification) {
        InputManager.shared.forceReleaseMouseCaptureIfNeeded()
    }

    /// Internal helper — routes signal-catch markers through the C bridge
    /// (Code Review C-2). NSLog is intentionally NOT used.
    private func logSig(_ name: String) {
        name.withCString { mx68k_log_sig_catch($0) }
    }
}
