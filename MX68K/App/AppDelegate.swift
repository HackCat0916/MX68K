import AppKit
import Foundation
import Dispatch

/// P53 — 正常終了の各経路(Cmd-Q・メニューの Quit・Dock の Quit・SIGTERM・SIGINT)を
/// C ブリッジへ接続し、mx68k_shutdown / atexit サマリが確実に実行されるようにする
/// NSApplicationDelegate。
///
/// ファイル名は Docs/02_プロジェクト構造.md:100 に合わせている(`AppDelegate.swift`)。
/// クラス名は MX68K* というプロジェクト内の命名との対称性のため
/// `MX68KAppDelegate` のまま残している。
///
/// 終了処理の連鎖(すべての正常終了経路で共通):
///   signal/Cmd-Q
///     -> applicationWillTerminate
///        -> mx68k_log_delegate_fire()                  (C ブリッジ経由で debug.log へ出力)
///        -> EmulatorViewModel.shared.stopEmulation()   (engine.stop -> オーディオ解体 -> mx68k_shutdown)
///     -> AppKit exit(3)
///     -> atexit -> mx68k_atexit_summary()              (P52-SUMMARY 等)
///
/// /tmp/mx68k_P53_plan.md §7 Edit C / §12.3 を参照。
final class MX68KAppDelegate: NSObject, NSApplicationDelegate {

    /// 強参照での保持が必須 — DispatchSourceSignal はスコープを外れると
    /// キャンセルされる(Spec Inv §C)。
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
        // P53 アブレーション: マスター + sigsrc スイッチ。0 のときは SIGTERM が
        // 既定の処理(default disposition)に戻る。
        guard mx68k_p53_sigsrc_enabled() != 0 else { return }

        // SIGTERM — 主経路(CI 相当。CLAUDE.md の起動テストで使用)。
        // 既定の終了処理がシグナルソースと競合しないよう、先に SIG_IGN を設定する。
        signal(SIGTERM, SIG_IGN)
        let term = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        term.setEventHandler { [weak self] in
            self?.logSig("SIGTERM")
            NSApp.terminate(nil)
        }
        term.resume()
        self.sigtermSource = term

        // SIGINT — 副経路(フォアグラウンドでの Ctrl-C)。nohup 経由の起動でも
        // インストールしたままにしておくコストは小さい(R-3 承知済み)。
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

        // P53 アブレーション: マスター + appdelegate スイッチ。
        guard mx68k_p53_appdelegate_enabled() != 0 else { return }

        // (Code Review C-2) マーカーを C ブリッジ経由で出力し、タグが確実に
        // ~/Library/Application Support/MX68K/debug.log へ書き込まれるようにする。
        // NSLog では Apple Unified Logging にしか出力されない。
        mx68k_log_delegate_fire()

        // (Code Review C-1) mx68k_shutdown が MEM/IPL/FONT を解放する「前に」
        // CVDisplayLink スレッドを停止させる。既存の stopEmulation() は
        // engine.stop -> オーディオ解体 -> mx68k_shutdown の順で処理するため、
        // それを再利用する。C 側の `s_p53_shutdown_done` と Swift 側の `isRunning`
        // ガードが、.onDisappear からの重複呼び出しを吸収する。
        //
        // N-1(P54 で追跡): CVDisplayLinkStop は非同期のため、実行中のコールバックが
        // 約 16 ms の間 free(MEM) と競合しうる。間に挟まるオーディオ解体処理
        // (約 150 ms)が結果的に遅延として働いている。アトミックな終了フラグによる
        // 正式な同期は P54 へ先送りした。
        if let vm = EmulatorViewModel.shared {
            vm.stopEmulation()
        } else {
            // 設定画面のみの経路: エンジンは一度も起動しておらず、MEM/IPL/FONT も
            // 未確保。free(NULL) は C 言語仕様上 no-op なので、ここで
            // mx68k_shutdown を直接呼んでも安全。
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

    /// 内部ヘルパー — シグナル捕捉マーカーを C ブリッジ経由で出力する
    /// (Code Review C-2)。NSLog は意図的に使わない。
    private func logSig(_ name: String) {
        name.withCString { mx68k_log_sig_catch($0) }
    }
}
