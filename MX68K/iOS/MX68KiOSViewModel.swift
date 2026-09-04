//
//  MX68KiOSViewModel.swift
//  MX68K-iOS
//
//  P703: iOS ターゲット用の**最小** ViewModel。
//
//  macOS 側の EmulatorViewModel(1800 行)は ArchiveMountService / ScreenshotService /
//  約 20 枚のモニタパネルを芋づるで引き込むため、本サイクルでも共有しない。
//  ここが担うのは「config から BIOS/ハードウェア設定を Bridge へ push →
//  mx68k_init() → FDD0 マウント → engine.start()」だけである。
//
//  P706: 設定画面の配線。ハードコードされていた機種/メモリ/クロックの 3 行と、
//        固定パスのブートディスクを廃し、**共有された EmulatorConfig を唯一の
//        真実**とする(iOS 専用の設定ストアを新規に作らない — P706 計画 §S-5)。
//        ★`ConfigManager` はここでは **所有しない**。2 インスタンスが同じ
//        config.json へ書くと片方の変更が消えるため、所有はルートビュー 1 箇所に
//        限り、この VM へは値(EmulatorConfig)として渡す(P706 §D-1 / R-11)。
//
//  P712: SASI / 内蔵SCSI / MO / CD のマウント配線を追加。ファイル取り込みは
//        FDD/BIOS のコピーイン方式ではなく、セキュリティスコープブックマーク方式
//        (`DiskBookmarkStore`)—— 大容量イメージのコピー時間/ストレージ二重消費を
//        避けるためのユーザーの選択(2026-08-30)。
//
//  意図的な非実装(P706 計画 §0-2 の非ゴール表のうち P712 でも扱わない残り、
//  うっかりの漏れではない):
//    General/Audio/Input/Windrv の各設定タブ /
//    ターボ・ノーウェイト / スクリーンショット / 録画 / 音声 / モニタパネル。
//    (セーブステートは P725 でクイックセーブ/ロード方式として実装済み。)
//

import Foundation
import CoreGraphics
import Combine

/// ★スレッド規約は macOS 側 EmulatorViewModel と同じ —— クラス自体に `@MainActor` は
/// 付けず、書き込みは常にメインスレッドから行う。X68KRenderer は
/// `DispatchQueue.main.async` 越しに `framebufferSize` / `displayGeometry` を書き、
/// `EmulatorEngine.onStatusUpdate` も同じくメインスレッドで発火する。
final class MX68KiOSViewModel: ObservableObject, RendererHost {

    // MARK: - RendererHost(X68KRenderer が publish してくる面)

    @Published var framebufferSize: CGSize = .zero
    @Published var displayGeometry: DisplayGeometry = .identity

    // MARK: - 画面に出す 1 行の状態表示

    @Published var statusText: String = "Not started"
    /// P723 — 状態表示の Speed% 項目。macOS 版 `EmulatorViewModel` と同名・同型・同既定値。
    @Published var speedPercent: Double = 100   // P658: StatusBar常時表示用
    /// P730 — マウント/イジェクト成功のたびにインクリメントするだけの値。
    /// どのビューからも値そのものは参照されない —— `@Published` 変更を発火させ、
    /// `fddMenu` の Eject `.disabled()` 判定(`mx68k_fdd_is_inserted` を直接呼ぶ)を
    /// 即座に再評価させるためだけのトリガー。これが無いと ZIP 単一イメージ経路
    /// (`mountFDDFromArchive`)は他に `@Published` 変更を起こさず、既存の 1Hz
    /// `onStatusUpdate` ポーリングまで最悪約1秒 Eject の有効化が遅延する。
    @Published var fddMountGeneration: Int = 0
    /// Metal 初期化に失敗したときの理由(EmulatorMetalView_iOS が設定する)。
    /// ★iOS では metallib の同梱漏れが現実的な失敗モードなので、macOS 側の
    /// `fatalError` とは違い、クラッシュさせずに画面へ出す(P703 計画 §D-2)。
    @Published var metalError: String?

    /// P706 §D-3 — BIOS が未設定/実体不在で起動を保留している状態。
    /// ルートビューがこれを見て `SettingsView(showCloseButton: false)` を全画面で出す
    /// (macOS の `RootView`(MX68KApp.swift:37-49)と同じ形)。
    @Published var needsConfiguration: Bool = false

    let engine = EmulatorEngine()

    /// `.task` / `.onAppear` はビューの再生成で複数回発火し得る。engine.start() 自身も
    /// `guard !isRunning` で冪等だが、BIOS 探索や Bridge への push をやり直す意味は
    /// 無いのでここでも一度きりに落とす。
    /// ★P706: 起動に**成功した**ときにだけ立てる。BIOS 未設定で保留した場合は false の
    /// ままなので、設定を Apply したあとアプリを再起動せずに再試行できる(§R-4)。
    private var didStart = false

    // MARK: - 起動
    //
    // ★P706: パスは **すべて config 由来**になった。P703 が持っていた
    //   `appSupportDir` / `biosDir` / `bootDiskPath` の固定パス組み立ては削除。
    //   コピーイン先の規約(bios/ と disks/)は `FilePickerButton.swift` の
    //   `ImportedFileStore.directory(for:)` が唯一の定義箇所であり、ここへ
    //   二重定義しない(値が割れる余地を残さない)。BIOS 未設定でも
    //   `ConfigManager.autodetectBIOS()` が bios/ 直下を拾うため、
    //   手動配置済みの環境は自動で引き継がれる(§S-13 / R-7)。

    /// P706 §D-3 — config 駆動の起動。BIOS が揃っていなければ起動を保留し、
    /// `needsConfiguration` を立てて設定画面へ誘導する(旧実装の statusText 行き止まりを置換)。
    func start(config: EmulatorConfig) {
        guard !didStart else { return }

        // P729 — 前回起動時に残った zip 展開先を掃除する(macOS `MX68KApp.swift:77`
        // の `ArchiveMountService.sweepStaleTempDirs()` 呼び出しに相当する、
        // アプリ起動時 1 回のベストエフォート掃除)。
        //
        // ★`guard !didStart` より**後**に置くこと(計画は「先頭へ」と書いているが、
        //   そのまま前へ置くと実害がある)。`.task` はビューの再出現で再発火し得る
        //   ため、ガードより前に置くと「zip をマウント済みの状態で start() が再入
        //   → 使用中の一時ディレクトリごと掃除される → マウント中のディスクの実体が
        //   消える」という経路が生まれる。ガードの後なら、起動に成功した後の再発火は
        //   ここへ到達しない。BIOS 未設定で保留中の再試行ではまだ到達し得るが、その
        //   状態では設定画面が全画面を占めており FDD メニュー自体に触れられないため、
        //   掃除対象になる一時ディレクトリは存在しない。
        IOSArchiveMountService.sweepStaleTempDirs()

        let fm = FileManager.default
        let iplrom = config.bios.iplromPath
        let cgrom = config.bios.cgromPath

        var missing: [String] = []
        if iplrom.isEmpty || !fm.fileExists(atPath: iplrom) { missing.append("IPLROM.DAT") }
        if cgrom.isEmpty  || !fm.fileExists(atPath: cgrom)  { missing.append("CGROM.DAT") }

        guard missing.isEmpty else {
            needsConfiguration = true
            statusText = "BIOS not configured — missing \(missing.joined(separator: ", "))"
            mx68k_log("[Swift][iOS] start deferred, missing: \(missing.joined(separator: ", "))")
            return
        }

        didStart = true
        needsConfiguration = false

        pushConfig(config)

        mx68k_log("[Swift][iOS] calling mx68k_init()")
        mx68k_init()
        mx68k_log("[Swift][iOS] mx68k_init() returned")

        // P706 §D-3: ブートディスクは config 由来。空でも**起動は止めない** ——
        // BIOS さえあれば IPL 画面までは進み、そこから「Boot Disk…」で入れられる。
        let bootDisk = config.fdd.lastFDD0Path
        if !bootDisk.isEmpty, fm.fileExists(atPath: bootDisk) {
            let inserted = bootDisk.withCString { mx68k_fdd_insert(0, $0) }
            mx68k_log("[Swift][iOS] mx68k_fdd_insert(0) -> \(inserted)")
        } else {
            mx68k_log("[Swift][iOS] no boot disk in config (fdd.lastFDD0Path=\"\(bootDisk)\")")
        }

        /* ★登録するコールバックはこの 2 本だけ(P723 で onSpeedUpdate を追加)。
         * 他の約 18 本のモニタ用コールバックは登録しない —— iOS にはモニタパネルが
         * 1 枚も無く、*Visible フラグは常に false なので、登録しても毎フレーム
         * 無駄になるだけ。 */

        /* P723: 状態表示から PC アドレスを外し、代わりに実行速度 % を出す。
         * 項目の並びは macOS 版 `StatusBarView.swift` と同じ
         * CPU → MEM → Speed → FD0 → FD1 → HDD。
         * ★TIMER はユーザーの明示確認により対象外(2026-09-02)。
         * speedPercent は下の onSpeedUpdate 経由。EmulatorEngine 側で
         * onSpeedUpdate → onStatusUpdate の順にメインキューへ enqueue されるため
         * (EmulatorEngine.swift:748-750、シリアル FIFO)、ここで読む値は
         * 同じ 1 秒周期で算出された最新値になる。 */
        engine.onStatusUpdate = { [weak self] status in
            // EmulatorEngine 側で DispatchQueue.main.async 済み(メインスレッド)。
            guard let self else { return }
            self.statusText = String(format: "CPU: %dMHz  MEM: %dMB  Speed: %d%%  FD0=%@  FD1=%@  HDD=%@",
                                     status.clock_mhz, status.memory_mb, Int(self.speedPercent.rounded()),
                                     status.fdd0_media_present ? "yes" : "no",
                                     status.fdd1_media_present ? "yes" : "no",
                                     (status.hdd0_inserted || status.hdd1_inserted) ? "yes" : "no")
        }
        engine.onSpeedUpdate = { [weak self] pct in   // P658
            self?.speedPercent = pct
        }

        engine.start()
        statusText = "Running"
    }

    // MARK: - 設定の反映

    /// P706 §D-3 — 設定画面の Apply から呼ばれる。
    ///
    /// P706 改訂 1 — 設定値は Bridge へ push するだけで、**確定は明示的な Reset ボタン
    /// 待ち**。macOS `EmulatorViewModel` の「『設定値』として保持され、配線は
    /// ハードリセット(⌘R)で確定する」と同じ意味論であり、iOS も同じ操作モデルになった
    /// (帯の Hard Reset ボタンが ⌘R の役割を担う)。
    func applySettings(_ config: EmulatorConfig) {
        guard didStart else {
            // まだ起動していない = BIOS 未設定で保留していた経路。ここで再試行する
            // (アプリの再起動を要求しない — §R-4)。これは「リセット」ではなく
            // 初回起動そのものなので、改訂 1 でもこの経路は維持する。
            start(config: config)
            return
        }
        pushConfig(config)
        mx68k_log("[Swift][iOS] applySettings -> config pushed (pending reset)")
    }

    // MARK: - リセット(P706 改訂 1 / 改訂 2)

    /// 帯の Hard Reset ボタンが立てる確認ダイアログのフラグ。
    /// macOS `EmulatorViewModel` と同名・同型(規範をそのまま踏襲する)。
    @Published var showHardResetConfirm: Bool = false

    /// macOS `EmulatorViewModel.hardReset()` のうち、iOS に存在する部分だけ。
    /// AudioEngine 再初期化(iOS は Audio 設定タブ非実装)と
    /// `InputManager.shared.requestMouseHoming()`(iOS はマウス非対応)は持ち込まない
    /// —— 新しい挙動を発明せず、iOS に無い機能への呼び出しを削るだけ。
    func hardReset() {
        mx68k_log("[Swift][iOS] hardReset() -> schedule hard reset")
        mx68k_schedule_hard_reset()
    }

    /// macOS `ToolbarView` の Soft Reset ボタンと同じく**確認ダイアログ無し**で即時実行
    /// (Hard Reset より破壊的でないため)。
    func softReset() {
        mx68k_log("[Swift][iOS] softReset() -> schedule soft reset")
        mx68k_schedule_soft_reset()
    }

    /// P709 — macOS `EmulatorViewModel.nmi()`(:740-743)のうち、iOS に存在する部分だけ。
    /// `showTransientMessage` は iOS 未実装(一時メッセージ UI そのものが無い)ため
    /// 持ち込まない —— 新しい挙動を発明せず、iOS に無い機能への呼び出しを削るだけ
    /// (hardReset / softReset と同型)。
    func nmi() {
        mx68k_log("[Swift][iOS] nmi() -> mx68k_nmi()")
        mx68k_nmi()
    }

    /// P721 — 設定画面の「Clear SRAM…」ボタンが立てる確認ダイアログのフラグ。
    /// macOS `EmulatorViewModel` と同名・同型(規範をそのまま踏襲する)。
    @Published var showSRAMClearConfirm: Bool = false

    /// P721 — macOS `EmulatorViewModel.clearSRAM()`(:733-738)のうち、iOS に存在する
    /// 部分だけ。バッテリバックアップ SRAM を工場出荷時設定(ROM 内蔵既定値)へ戻す
    /// (実機のバックアップ電池を外す操作に相当)。破壊的操作のため呼出し側で確認
    /// ダイアログを経由すること。
    /// `showTransientMessage` は iOS 未実装(一時メッセージ UI そのものが無い)ため
    /// 持ち込まない —— 新しい挙動を発明せず、iOS に無い機能への呼び出しを削るだけ
    /// (hardReset / softReset / nmi と同型)。
    func clearSRAM() {
        mx68k_log("[Swift][iOS] clearSRAM() -> schedule sram clear")
        mx68k_schedule_sram_clear()   // フレーム境界で安全にクリア(直接 mx68k_sram_clear は run_frame とレース)。
    }

    // MARK: - State Save/Load (P725)

    /// 一時的な一回限りの通知メッセージ(macOS `EmulatorViewModel.transientMessage`
    /// に相当)。`MX68KiOSApp.swift` の `statusLines` が 1 行として表示し、3 秒後に
    /// 自動で消える(`showTransientMessage(_:)`)。
    /// ★P728 でセーブ/ロード結果専用(`stateOperationMessage`)として作られたが、
    /// 実装内容は macOS 版と同型の汎用一時通知機構であり、P731 で名称を一般化した
    /// —— 1 秒周期で無条件に上書きされる `statusText` は、この種の通知には使わない。
    @Published var transientMessage: String?

    /// ★Code Review 指摘 2 により追加 —— 現在セーブ/ロード操作が進行中(ポーリング待ち)か。
    /// Menu 側でこれが true の間 `.disabled` にし、ポーリング未完了のまま 2 つ目の
    /// save/load がキューされて `stateOpTask?.cancel()` が 1 つ目の結果通知を
    /// 無言で握り潰す競合(狭いが実在するレース)を防ぐ。
    @Published var isStateOperationPending: Bool = false

    /// ★P726改訂——`Timer`(RunLoop依存)から`Task`(`@MainActor`、構造化並行性)へ
    /// 置き換えた。P725で報告された「結果メッセージが一度も表示されない」不具合
    /// (ユーザーhands-on確認、2026-09-03)への対応。RunLoopモード依存によるTimer
    /// 未発火、またはメインスレッド外での`@Published`変更という2つの仮説を
    /// 同時に排除する——`@MainActor`修飾により、ループの各反復が確実にメイン
    /// アクター上で実行されることをコンパイラが保証する。
    private var stateOpTask: Task<Void, Never>?

    /// macOS `EmulatorViewModel.showTransientMessage(_:)`(:749-757)の逐語移植。
    /// 世代カウンタで「表示中に次の操作が来て新しいメッセージへ上書きされた場合、
    /// 古い(3秒前にスケジュールされた)消去タイマーがそれを誤って消さない」ことを
    /// 保証する。
    private var transientMessageGeneration = 0

    /// `transientMessage`を設定し、3秒後に自動で消す(ユーザー要望、
    /// 2026-09-03、P727で表示は直ったが自動消去が無いことを指摘された)。
    private func showTransientMessage(_ text: String) {
        transientMessageGeneration += 1
        let generation = transientMessageGeneration
        transientMessage = text
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in
            guard let self, self.transientMessageGeneration == generation else { return }
            self.transientMessage = nil
        }
    }

    private static let stateFileExtension = "mxstate"

    /// クイックセーブの保存先。`DiskBookmarkStore` と同じ
    /// `~/Library/Application Support/MX68K/` 配下(macOS 版の
    /// `~/Documents/MX68K/states/` とは意図的に異ならせる —— iOS の `~/Documents` は
    /// Files.app 経由でユーザーに公開されうる場所であり、ファイルピッカーを介さない
    /// 内部管理というクイックセーブ方式の設計選択とは相性が悪いため)。
    ///
    /// ★`DiskBookmarkStore`(このディレクトリ規約自体の一次情報源)は「保存/解決の
    /// 失敗を無言にしない」自己反証可能性の規律(全経路で 1 行ログ)を確立済み。
    /// 同じディレクトリへ触れる新規 iOS コードはこの規律に揃える(「macOS 版の逐語
    /// 移植だから」は、この箇所については `try?` 握り潰しの免責にならない ——
    /// 移植元の macOS 版が書かれた時点ではこの規律自体が存在しなかった)。
    private func statesDirectory() -> URL {
        let dir = URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent("Library/Application Support/MX68K/states", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        } catch {
            mx68k_log("[Swift][iOS][P725-STATE] statesDirectory createDirectory failed: \(error)")
        }
        return dir
    }

    /// クイックセーブ: ファイルピッカーを介さず、タップ一つで自動命名保存する。
    /// ファイル名フォーマットは macOS 版 `EmulatorViewModel.saveState()`(:895-900)
    /// からの逐語移植 —— `yyyyMMdd_HHmmss` は辞書順ソートが時刻順と一致するため、
    /// `listSavedStates()` 側でファイル名文字列のソートだけで新しい順に並べられる
    /// (ファイル属性の読み出しが不要)。
    func saveState() {
        let fmt = DateFormatter()
        fmt.locale = Locale(identifier: "en_US_POSIX")
        fmt.dateFormat = "yyyyMMdd_HHmmss"
        let name = "mx68k_\(fmt.string(from: Date())).\(Self.stateFileExtension)"
        let url = statesDirectory().appendingPathComponent(name)
        let seq0 = mx68k_state_op_seq()
        guard mx68k_save_state(url.path) == 0 else {
            showTransientMessage(String(localized: "Failed to save state."))
            return
        }
        mx68k_log("[Swift][iOS][P725-STATE] saveState queued name=\(name)")
        awaitStateOp(seq0, isLoad: false, name: name)
    }

    /// クイックロード: 一覧(`StateListView`)でユーザーが選んだファイルを読み込む。
    func loadState(url: URL) {
        let seq0 = mx68k_state_op_seq()
        guard mx68k_load_state(url.path) == 0 else {
            showTransientMessage(String(localized: "Failed to load state."))
            return
        }
        mx68k_log("[Swift][iOS][P725-STATE] loadState queued name=\(url.lastPathComponent)")
        awaitStateOp(seq0, isLoad: true, name: url.lastPathComponent)
    }

    /// macOS `EmulatorViewModel.awaitStateOp`(:955-978)の非同期完了待ちを
    /// `Task`ベースへ再実装したもの(ロジックの意味論——ポーリング間隔0.05秒・
    /// タイムアウト2.0秒・rc判定——はP725から不変、実行機構のみ変更)。
    /// ★iOSには`isPaused`概念が無い前提はP725から不変(変更なし)。
    ///
    /// ★Code Review指摘により訂正——`@MainActor`は関数自体にではなく`Task`の
    ///   クロージャへ付ける。`awaitStateOp`は非`@MainActor`・非`async`の
    ///   `saveState()`/`loadState(url:)`から同期的に呼ばれる(このクラス全体は
    ///   意図的に`@MainActor`化していない、ファイル冒頭のスレッド規約コメント
    ///   参照)。関数自体を`@MainActor`にすると、非同期コンテキストを持たない
    ///   呼び出し元から直接呼べずコンパイルエラーになる(Code Reviewが実際に
    ///   `swiftc -typecheck`で再現・確認済み)。
    private func awaitStateOp(_ seq0: UInt32, isLoad: Bool, name: String) {
        stateOpTask?.cancel()
        isStateOperationPending = true
        let deadline = Date().addingTimeInterval(2.0)
        stateOpTask = Task { @MainActor [weak self] in
            while true {
                guard let self, !Task.isCancelled else { return }
                if mx68k_state_op_seq() != seq0 {
                    self.isStateOperationPending = false
                    let rc = mx68k_last_state_rc()
                    self.showTransientMessage(rc == 0
                        ? String(localized: isLoad ? "State loaded: \(name)" : "State saved: \(name)")
                        : Self.stateErrorText(rc: rc, isLoad: isLoad))
                    // ★自己反証可能性——完了処理へ実際に到達したことを示す1行。
                    mx68k_log(String(format: "[Swift][iOS][P725-STATE] awaitStateOp completed rc=%d isLoad=%d",
                                     Int(rc), isLoad ? 1 : 0))
                    return
                }
                if Date() >= deadline {
                    self.isStateOperationPending = false
                    self.showTransientMessage(String(localized:
                        "State operation did not complete (emulation stopped or paused)."))
                    mx68k_log(String(format: "[Swift][iOS][P725-STATE] awaitStateOp timedOut isLoad=%d",
                                     isLoad ? 1 : 0))
                    return
                }
                try? await Task.sleep(nanoseconds: 50_000_000)   // 0.05秒、P725から不変
            }
        }
    }

    /// macOS `EmulatorViewModel.stateErrorText(rc:isLoad:)`(:983-998)の逐語移植。
    private static func stateErrorText(rc: Int32, isLoad: Bool) -> String {
        switch rc {
        case -11:
            return String(localized: "This state file was saved by an older, incompatible version of MX68K and cannot be loaded.")
        case -15:
            return String(localized: "This state file does not match the current memory size setting.")
        case -2:
            return String(localized: "Could not read or write the state file.")
        case -10, -12, -13, -14:
            return String(localized: "This state file is corrupted or invalid.")
        default:
            return isLoad
                ? String(localized: "Failed to load state.")
                : String(localized: "Failed to save state.")
        }
    }

    /// `StateListView` が表示する一覧。ファイル名文字列の降順ソートで新しい順になる
    /// (`yyyyMMdd_HHmmss` は辞書順 = 時刻順)。
    ///
    /// ★`try?` で失敗を空配列へ握り潰すと、「本当に保存ファイルが 0 件」と
    /// 「ディレクトリ読み取りに失敗した」が `StateListView` 側で区別できなくなる
    /// (自己反証可能性: 1 つの出力に複数の説明が収束する典型形)。失敗時は
    /// `DiskBookmarkStore` と同じ流儀で 1 行ログを出してから空配列を返す。
    func listSavedStates() -> [URL] {
        let dir = statesDirectory()
        do {
            let files = try FileManager.default.contentsOfDirectory(
                at: dir, includingPropertiesForKeys: nil)
            return files
                .filter { $0.pathExtension == Self.stateFileExtension }
                .sorted { $0.lastPathComponent > $1.lastPathComponent }
        } catch {
            mx68k_log("[Swift][iOS][P725-STATE] listSavedStates contentsOfDirectory failed: \(error)")
            return []
        }
    }

    /// `StateListView` のスワイプ削除から呼ばれる。
    ///
    /// ★`try?` の成否を見ずに常に同じログ文言を出すと、削除に失敗しても成功した
    /// かのような行が残る(`DiskBookmarkStore.remove(key:)` が確立した「成否を
    /// `ok:` 相当で受けてログへ埋め込む」流儀と不一致になる)。
    func deleteState(url: URL) {
        do {
            try FileManager.default.removeItem(at: url)
            mx68k_log("[Swift][iOS][P725-STATE] deleteState name=\(url.lastPathComponent) result=ok")
        } catch {
            mx68k_log("[Swift][iOS][P725-STATE] deleteState name=\(url.lastPathComponent) result=failed error=\(error)")
        }
    }

    /// P706 §D-3 — macOS `EmulatorViewModel.pushConfig(_:)` の BIOS + ハードウェア部分
    /// だけを抜き出した小関数。★iOS 用に新しい既定値・新しい写像を発明しない。
    ///
    /// P712 — SASI / 内蔵SCSI / MO / CD の再送を追加した(下半分)。
    /// MIDI / Mercury / Windrv 系の setter は引き続き **呼ばない**
    /// (iOS 未対応、P706 §0-2 非ゴールのうち P712 でも扱わない残り)。
    private func pushConfig(_ config: EmulatorConfig) {
        config.bios.iplromPath.withCString { ipl in
            config.bios.cgromPath.withCString { cg in
                mx68k_set_bios_path(ipl, cg)
            }
        }
        if !config.bios.iplrom30Path.isEmpty {
            config.bios.iplrom30Path.withCString { mx68k_set_bios_path_030($0) }
        }
        // P712c — SCSI ROM の再送(macOS EmulatorViewModel.swift:476-490 の逐語移植)。
        // これを呼ばないと Bridge 側 s_scsi_in_rom_loaded が false のままとなり、
        // scsi_in_bridge_install() の gate_machine && gate_rom が成立せず
        // SCSI I/O ウィンドウが恒久的に未配線になる(内蔵SCSI が認識されない)。
        if !config.bios.scsiExtRomPath.isEmpty {
            config.bios.scsiExtRomPath.withCString { path in
                mx68k_set_scsi_ext_rom_path(path)
            }
        }
        if !config.bios.scsiInRomPath.isEmpty {
            config.bios.scsiInRomPath.withCString { path in
                mx68k_set_scsi_in_rom_path(path)
            }
        }
        mx68k_set_machine_type(Int32(Self.machineTypeValue(config.hardware.machineType)))
        mx68k_set_memory_size(Int32(config.hardware.memoryMB))
        mx68k_set_clock(Int32(config.hardware.clockMHz))
        mx68k_set_fpu_enabled(config.hardware.fpuEnabled)
        // ★P706 Step 5b の判断: 以下 2 本は §D-3 の setter 列挙には無いが、
        //   §C-3 の表が iOS でも **表示する**と定めた 2 つのコントロール
        //   (Machine Configuration の「External FDD Unit」と SRAM の「Capacity」)の
        //   反映先である。呼ばなければ「値だけ設定できて効かないトグル」になり、
        //   §0-4 提案(3)が禁じた壊れた UI そのものになる。§D-3 が明示的に除外して
        //   いるのは SCSI/SASI/MIDI/Mercury/Windrv 系であり、この 2 本は含まれない。
        mx68k_set_ext_fdd_enabled(config.extensions.externalFDDUnit)   // P686
        mx68k_set_sram_64k_enabled(config.extensions.sram64kEnabled)   // P493

        // ------------------------------------------------------------------
        // P712 §方針3 — SASI / 内蔵SCSI / MO / CD の再送。
        //
        // macOS `EmulatorViewModel.pushConfig`(:492-580)のロジックを、実在チェック
        // (`fileExists`)だけ「ブックマーク解決 **かつ** アクセス開始の両方が成功」へ
        // 置き換えた形で移植する。macOS 側の 3 つの設計点はそのまま維持する:
        //   (1) 空文字も必ず push する(skip ではなく明示的な clear — P450 の教訓)
        //   (2) 機種ゲートは付けない(判定は Bridge が一手に担う)。ただし
        //       scsi0Path〜scsi6Path が内蔵/外付けで **共有** フィールドである
        //       ことに由来する `scsiMode == "internal"` ゲートだけは Swift 側に残る
        //       (macOS :495-506 と同じ理由。MO/CD は専用フィールドなので不要)
        //   (3) 実在チェック相当のゲートのみ掛ける
        // ------------------------------------------------------------------
        let useInternalSCSI = (config.extensions.scsiMode == "internal")
        for id in 0..<7 {
            let key = "scsi\(id)"
            // 外付けモードでは同じキー "scsi<id>" のアクセス中URLは insertSCSI/ejectSCSI が
            // ライブマウントとして管理している。pushConfig は start()/applySettings() の
            // 両方から呼ばれるため、ここで replaceAccessURL(key:with:nil) を呼ぶと
            // SCSI と無関係な設定を Apply しただけでライブマウント中のアクセスが
            // 打ち切られてしまう。外付けモードではアクセス状態に一切触れない。
            let path = useInternalSCSI ? resolvedPath(key: key) : ""
            path.withCString { mx68k_set_scsi_in_disk_path(Int32(id), $0) }
        }
        resolvedPath(key: "mo").withCString { mx68k_set_mo_path($0) }
        resolvedPath(key: "cd").withCString { mx68k_set_cd_path($0) }
        for unit in 0..<ExtensionsConfig.hddUnitCount {
            resolvedPath(key: "hdd\(unit)").withCString { mx68k_set_hdd_path(Int32(unit), $0) }
        }
        // SASI / SCSI タブ自体の前提条件なので同時に持ち込む(macOS :576/:580)。
        mx68k_set_memsw_auto_update(config.extensions.memSwitchAutoUpdate)
        mx68k_set_scsi_ext_board_installed(config.extensions.scsiMode == "external")
    }

    // P221b/P706: 新 2 値(SASI/SCSI)を C 側 enum 値へ。**macOS の
    // `EmulatorViewModel.machineTypeValue()`(:672-678)と同一の写像**を使う
    // (P706 §S-14 / §記号表)。
    private static func machineTypeValue(_ type: String) -> Int {
        switch MachineTypeMigration.normalize(type) {
        case "SASI": return 0
        case "SCSI": return 4
        default: return 4
        }
    }

    // MARK: - ディスク

    /// P706 §D-3 / 改訂 2 — 指定ドライブへディスクイメージを入れる。
    ///
    /// ★`EmulatorViewModel.mountFDD`(:1306-1332)と同じく、**メインスレッドから
    /// エミュレーションロック無しで** `mx68k_fdd_insert` を呼ぶ(macOS で確立済みの
    /// 既存方式を踏襲し、新方式を発明しない — §S-15)。
    ///
    /// 意味論も macOS `mountFDD` と同一 —— 稼働中は**ライブ差し替えのみ**でリセットせず、
    /// 確定は明示的な Reset ボタン待ち(改訂 1 で iOS も同じ挙動へ統一済み)。
    /// write protect は iOS 未対応のまま(§0-2 非ゴール継続)。
    /// config への保存は ConfigManager を所有するルートビュー側で行う。
    /// - Returns: 挿入に成功したか。
    @discardableResult
    func mountDisk(drive: Int, path: String) -> Bool {
        // P729 — macOS `mountFDD`(:1306-1332)と同じく、**通常ファイル経路も含めた
        // すべてのマウント経路の入口で**、そのドライブの旧 zip 一時ディレクトリを
        // 破棄する。zip 由来のドライブへ eject を経ずに別の通常ファイルを上書き
        // マウントするケースを取りこぼさないための必須の対称性 —— これが無いと
        // 一時ディレクトリが次回起動時のスイープまでリークする。
        releaseArchiveTempDir(drive: drive)
        let result = path.withCString { mx68k_fdd_insert(Int32(drive), $0) }
        mx68k_log("[Swift][iOS] mountDisk drive=\(drive) mx68k_fdd_insert -> \(result) path=\(path)")
        guard result == 0 else {
            // P731 — `statusText` は `engine.onStatusUpdate` が約1秒周期で無条件に
            // 上書きするため、一回限りのエラー通知の宛先としては成立しない
            // (最悪1秒未満で消え、体感上「一度も表示されない」)。文言は据え置き、
            // 表示経路のみ `showTransientMessage`(3秒自動消去)へ変更。
            showTransientMessage("Failed to mount \((path as NSString).lastPathComponent) on FD\(drive)")
            return false
        }
        // P730 — 通常ファイル経路・ZIP単一/複数イメージ経路(`mountFDDFromArchive` は
        // 内部でここを呼ぶ)を一括カバーする Eject 再描画トリガー。
        fddMountGeneration += 1
        return true
    }

    /// P713 — macOS `EmulatorViewModel.ejectFDD(drive:)`(:1354-1368)のうち、
    /// iOS に存在する部分だけ。
    ///
    /// ★P729 で更新 —— P713 時点のこの doc は「archive 一時ディレクトリ解放・
    ///   write protect 解除はどちらも iOS 未実装機能なので持ち込まない」と
    ///   書いていたが、**その前提の片方が本サイクルで崩れた**。zip 対応を
    ///   追加した以上、archive 一時ディレクトリの解放は iOS でも必要な処理で
    ///   あり、省略は成立しない。
    ///   - archive 一時ディレクトリ解放 …… **持ち込む**(P729 で zip 対応追加)。
    ///   - write protect 解除(`restoreWriteProtectIfNeeded` 相当)…… 引き続き
    ///     **持ち込まない**。iOS には書込み禁止トグルの UI がそもそも無く
    ///     (P706 §0-2 非ゴール継続)、復元すべき「ユーザーが設定した元の状態」が
    ///     存在しない。新しい挙動を発明せず、iOS に無い機能への呼び出しを削る
    ///     だけ(hardReset / softReset / nmi と同型)。
    func ejectFDD(drive: Int) {
        releaseArchiveTempDir(drive: drive)
        mx68k_log("[Swift][iOS] ejectFDD drive=\(drive)")
        mx68k_fdd_eject(Int32(drive))
        // P730 — Eject 自体の直後反映を明示的に保証(既存の configManager.save() 頼みの
        // 暗黙的な反映に依存しない)。
        fddMountGeneration += 1
    }

    // MARK: - ディスク(ZIP圧縮イメージ、P729)

    /// アーカイブ選択シートの表示状態。`showArchivePicker`が立っている間、
    /// ルートビューが`IOSSheetKind.archivePicker`を提示する。
    @Published var showArchivePicker = false
    @Published var archivePickerImages: [URL] = []
    private var archivePickerTempDir: URL?
    private var archivePickerDrive: Int = 0

    /// FDD0/FDD1それぞれのzip展開用一時ディレクトリ(macOS版
    /// `fdd0ArchiveTempDir`/`fdd1ArchiveTempDir`に相当。iOSはFDD0/FDD1のみ
    /// UIを持つため2台分のみ——P712/P713のiOS FDD UIスコープに合わせる)。
    private var fdd0ArchiveTempDir: URL?
    private var fdd1ArchiveTempDir: URL?

    private func archiveTempDir(_ drive: Int) -> URL? {
        drive == 0 ? fdd0ArchiveTempDir : fdd1ArchiveTempDir
    }
    private func setArchiveTempDir(_ drive: Int, _ v: URL?) {
        if drive == 0 { fdd0ArchiveTempDir = v } else { fdd1ArchiveTempDir = v }
    }

    /// macOS `EmulatorViewModel.handleArchiveSelection`(:1424-1455)のうち、
    /// iOSに存在する部分だけ。requestAutoPause/releaseAutoPauseはiOSに
    /// 存在しない概念のため呼ばない(P706 §D-3で確立済みの「iOSに無い機能への
    /// 呼び出しを削るだけ、新しい挙動を発明しない」方針、hardReset等と同型)。
    /// ★P731で更新——P729時点のこのdocは「一時メッセージ表示(`showTransientMessage`
    /// 相当)もiOS未実装のため、失敗時は`statusText`へ書く」と書いていたが、その前提は
    /// 誤りだった(P728の`stateOperationMessage`機構が実質同型の一時通知機構として
    /// 既に存在していた)。P731でその機構を`showTransientMessage`へ一般化し、
    /// 失敗時はmacOS版(:1447-1453)と同じくそちらへ書く。
    func handleArchiveSelection(drive: Int, zipURL: URL) {
        let result = IOSArchiveMountService.extract(zipURL: zipURL)
        switch result {
        case .success(let archive):
            if archive.images.count == 1 {
                mountFDDFromArchive(drive: drive, extractedPath: archive.images[0].path, tempDir: archive.tempDir)
            } else {
                archivePickerImages = archive.images
                archivePickerTempDir = archive.tempDir
                archivePickerDrive = drive
                showArchivePicker = true
            }
        case .failure(let error):
            if let extractError = error as? IOSArchiveMountService.ExtractError,
               case .noDiskImageFound = extractError {
                showTransientMessage(String(localized: "No disk image found in the archive"))
            } else {
                showTransientMessage(String(localized: "Failed to extract the archive"))
            }
        }
    }

    /// macOS `EmulatorViewModel.completeArchiveSelection`(:1458-1464)の逐語移植。
    func completeArchiveSelection(_ url: URL) {
        let drive = archivePickerDrive
        let tempDir = archivePickerTempDir
        clearArchivePickerState()
        guard let tempDir else { return }
        mountFDDFromArchive(drive: drive, extractedPath: url.path, tempDir: tempDir)
    }

    /// macOS `EmulatorViewModel.cancelArchiveSelection`(:1467-1472)の逐語移植。
    func cancelArchiveSelection() {
        if let tempDir = archivePickerTempDir {
            IOSArchiveMountService.cleanup(tempDir)
        }
        clearArchivePickerState()
    }

    private func clearArchivePickerState() {
        showArchivePicker = false
        archivePickerImages = []
        archivePickerTempDir = nil
    }

    /// macOS `EmulatorViewModel.mountFDDFromArchive`(:1372-1389)のうち、
    /// iOSに存在する部分だけ。★書込み禁止の**強制**(`mx68k_fdd_set_write_protect`)
    /// はmacOS版と同じくデータ保護のため必須で持ち込む——展開先は一時ディレクトリで
    /// アプリ終了/イジェクト時に破棄されるため、書込みを許すとゲストの保存内容が
    /// 消える。macOS版が持つ「南京錠トグル表示をONにする」処理(`setFddWriteProtect`
    /// による見た目の同期)は**持ち込まない**——iOSにはそもそも書込み禁止トグルの
    /// UIが無い(P706 §0-2非ゴール)ため、表示を同期させる対象自体が存在しない。
    /// これは新しい挙動の発明ではなく、存在しないUIへの同期呼び出しを削るだけ
    /// (ejectFDD等、既存のiOS簡略化と同型)。
    ///
    /// ★Code Review指摘により訂正——macOS版のコメント「マウント結果によらず
    ///   記録する。失敗時も次回のマウント/イジェクト、最悪でも次回起動時の
    ///   スイープで確実に破棄される」(`EmulatorViewModel.swift:1386-1387`)は
    ///   `setFddArchiveTempDir`を**無条件**に呼ぶことを明示的な設計として
    ///   述べている。当初案は`guard mountDisk(...) else { return }`でこれを
    ///   ガードしてしまい、マウント失敗時に`setArchiveTempDir`が呼ばれず
    ///   一時ディレクトリが追跡されないまま残る(次のマウント/イジェクトでも
    ///   最終的なアプリ再起動時のスイープでも掃除されない、次回起動まで
    ///   リークする)欠陥だった。`mountDisk`の成否に関わらず
    ///   `setArchiveTempDir`を呼び、`mx68k_fdd_set_write_protect`だけを
    ///   成功時に限定する形へ訂正する。
    func mountFDDFromArchive(drive: Int, extractedPath: String, tempDir: URL) {
        releaseArchiveTempDir(drive: drive)
        let ok = mountDisk(drive: drive, path: extractedPath)
        if ok {
            mx68k_fdd_set_write_protect(Int32(drive), 1)
        }
        // macOS: マウント結果によらず記録する(:1386-1387)。ここを`guard`で
        // ガードしない——失敗時も一時ディレクトリの追跡・掃除は必要。
        setArchiveTempDir(drive, tempDir)
        mx68k_log("[Swift][iOS][P729-ARCHIVE] mountFDDFromArchive drive=\(drive) ok=\(ok) writeProtect=\(ok ? "forced" : "n/a")")
    }

    /// macOS `EmulatorViewModel.releaseArchiveTempDir`(:1391-1404)のうち、
    /// iOSに存在する部分だけ。restoreWriteProtectIfNeeded相当はiOS未実装の
    /// 書込み禁止トグル機構に依存するため呼ばない(§理由は上記と同型)。
    private func releaseArchiveTempDir(drive: Int) {
        guard let tempDir = archiveTempDir(drive) else { return }
        IOSArchiveMountService.cleanup(tempDir)
        setArchiveTempDir(drive, nil)
    }

    // MARK: - ディスク(SASI / 内蔵SCSI / MO / CD、P712)
    //
    // 戻り値は **8 関数すべて `(ok: Bool, message: String?)` に統一**する(§方針3 / R-B)。
    // macOS の `showTransientMessage` に相当する一時メッセージ UI が iOS には無いため、
    // 失敗理由も成功時の注記(「次回ハードリセットで反映」等)も同じ 1 本の経路で
    // 呼び出し側 View へ返し、View が 1 行として表示する。文言は macOS の
    // `showTransientMessage` 引数を **そのまま再利用**する(新しい文言を発明しない)。

    /// マウント中のセキュリティスコープ付き URL(キー → アクセス中 URL)。
    /// アプリ終了時はプロセスごと終わるため明示的な stop は不要。
    private var activeAccessURLs: [String: URL] = [:]

    /// 同じキーの旧 URL があればアクセスを閉じてから差し替える(§R-4)。
    /// insert / eject / pushConfig のすべてがこの 1 点を通る。
    private func replaceAccessURL(key: String, with url: URL?) {
        if let old = activeAccessURLs[key] {
            old.stopAccessingSecurityScopedResource()
        }
        activeAccessURLs[key] = url
    }

    /// 保存済みブックマークを解決し、アクセス開始まで成功したときだけパスを返す。
    ///
    /// ★§R-A: 「`resolve()` 成功」と「`startAccessingSecurityScopedResource()` 成功」は
    /// **別の失敗点**。どちらか一方でも失敗すれば空文字を返す —— macOS 版の
    /// `fileExists` 実在チェック(「実在しなければ空文字」)に対応する iOS 側のゲート。
    /// `started` を無視してパスだけ渡すと、`sasi.c:426` のデバイス在席判定が
    /// 「在席するが全リードが失敗する」壊れた状態を作ってしまう。
    private func resolvedPath(key: String) -> String {
        guard let url = DiskBookmarkStore.resolve(key: key),
              url.startAccessingSecurityScopedResource() else {
            replaceAccessURL(key: key, with: nil)
            return ""
        }
        replaceAccessURL(key: key, with: url)
        return url.path
    }

    /// 基本メッセージと永続化の注記を 1 本にまとめる(両方 nil なら nil)。
    private func combineMessages(_ base: String?, _ note: String?) -> String? {
        let parts = [base, note].compactMap { $0 }
        return parts.isEmpty ? nil : parts.joined(separator: " ")
    }

    /// マウント成功後の共通後処理。ブックマーク保存が失敗した場合だけ注記を返す。
    /// ★§R-D: 永続化の失敗を無言にしない。ライブマウント自体は成功しているので
    /// `ok` は真のまま、次回起動での復元が保証できない旨だけ伝える。
    private func rememberMount(key: String, url: URL) -> String? {
        replaceAccessURL(key: key, with: url)
        let saved = DiskBookmarkStore.save(key: key, url: url)
        return saved ? nil
                     : String(localized: "Mounted, but this may not be remembered after the app restarts")
    }

    /// アクセス開始に失敗したときの共通の戻り値。
    private func accessDeniedResult(_ what: String) -> (ok: Bool, message: String?) {
        mx68k_log("[Swift][iOS] \(what) startAccessing failed")
        return (false, String(localized: "Could not access the selected file"))
    }

    // MARK: SASI HDD (.hdf)

    /// SASI HDD イメージをマウントする。ブックマークキーは `"hdd<unit>"`。
    @discardableResult
    func insertHDD(unit: Int, url: URL) -> (ok: Bool, message: String?) {
        let key = "hdd\(unit)"
        guard url.startAccessingSecurityScopedResource() else {
            return accessDeniedResult("insertHDD unit=\(unit)")
        }
        let path = url.path
        let result = path.withCString { mx68k_hdd_insert(Int32(unit), $0) }
        mx68k_log("[Swift][iOS] insertHDD unit=\(unit) mx68k_hdd_insert -> \(result)")
        guard result == 0 else {
            url.stopAccessingSecurityScopedResource()
            switch result {
            case -3:
                return (false, String(localized: "HDD image size does not match a valid SASI capacity (10/20/40MB)"))
            case -4:
                return (false, String(localized: "This image may be SCSI-formatted (X68SCSI1 identifier detected). Try inserting it into a SCSI slot."))
            default:
                return (false, String(localized: "HDD image mount failed"))
            }
        }
        return (true, rememberMount(key: key, url: url))
    }

    @discardableResult
    func ejectHDD(unit: Int) -> (ok: Bool, message: String?) {
        let result = mx68k_hdd_eject(Int32(unit))
        mx68k_log("[Swift][iOS] ejectHDD unit=\(unit) mx68k_hdd_eject -> \(result)")
        guard result == 0 else {
            return (false, String(localized: "HDD eject failed"))
        }
        replaceAccessURL(key: "hdd\(unit)", with: nil)
        DiskBookmarkStore.remove(key: "hdd\(unit)")
        return (true, nil)
    }

    // MARK: SCSI (ID0-6)

    /// SCSI ディスクイメージを装着する。ブックマークキーは `"scsi<id>"`。
    ///
    /// - Parameter live: 外付けボード経路(`scsiMode == "external"`)なら `true` で、
    ///   macOS `EmulatorViewModel.insertSCSI` と同じく `mx68k_scsi_insert` を呼ぶ。
    ///   内蔵経路(`"internal"`)は `false` —— macOS 側も内蔵モードでは config への
    ///   書込みだけを行い、Bridge へは `pushConfig` の
    ///   `mx68k_set_scsi_in_disk_path` 経由で次回ハードリセット時に反映される
    ///   (`SCSISettingsView.swift:12-14, 208-215` と同じ分岐)。`mx68k_scsi_insert` は
    ///   配線確定機種が SCSI のとき `-2` を返して必ず拒否するため、内蔵モードで
    ///   呼んではならない。どちらの経路でもブックマーク保存とアクセス開始は行う。
    @discardableResult
    func insertSCSI(id: Int, url: URL, live: Bool) -> (ok: Bool, message: String?) {
        let key = "scsi\(id)"
        guard url.startAccessingSecurityScopedResource() else {
            return accessDeniedResult("insertSCSI id=\(id)")
        }
        if live {
            let path = url.path
            let result = path.withCString { mx68k_scsi_insert(Int32(id), $0) }
            mx68k_log("[Swift][iOS] insertSCSI id=\(id) mx68k_scsi_insert -> \(result)")
            guard result == 0 else {
                url.stopAccessingSecurityScopedResource()
                switch result {
                case -3:
                    return (false, String(localized: "SCSI image size is out of the valid range"))
                default:
                    return (false, String(localized: "SCSI image mount failed"))
                }
            }
        } else {
            mx68k_log("[Swift][iOS] insertSCSI id=\(id) internal mode — bookmark only (applies on hard reset)")
        }
        return (true, rememberMount(key: key, url: url))
    }

    @discardableResult
    func ejectSCSI(id: Int, live: Bool) -> (ok: Bool, message: String?) {
        if live {
            // macOS `ejectSCSI`(:1610-1614)と同じく `mx68k_scsi_eject` は void。
            mx68k_scsi_eject(Int32(id))
        }
        mx68k_log("[Swift][iOS] ejectSCSI id=\(id) live=\(live ? 1 : 0)")
        replaceAccessURL(key: "scsi\(id)", with: nil)
        DiskBookmarkStore.remove(key: "scsi\(id)")
        return (true, nil)
    }

    // MARK: SCSI MO (ID5 固定スロット)

    /// SCSI MO イメージを装着する。ブックマークキーは `"mo"`。
    ///
    /// ★スレッド安全性は macOS `insertMO`(:1649-1679)と同じ —— `Open`/`Eject` は
    /// `disk.dcache` を delete/new するため、描画スレッドの `mx68k_run_frame()` と
    /// 排他する必要がある。`engine.withEmulationLock` で Bridge 呼び出しを包む。
    /// iOS には `requestAutoPause` に相当する UI が無いので、そちらは持ち込まない。
    @discardableResult
    func insertMO(url: URL) -> (ok: Bool, message: String?) {
        guard url.startAccessingSecurityScopedResource() else {
            return accessDeniedResult("insertMO")
        }
        let path = url.path
        let result = engine.withEmulationLock {
            path.withCString { mx68k_mo_insert($0) }
        }
        mx68k_log("[Swift][iOS] insertMO mx68k_mo_insert -> \(result)")
        switch result {
        case 0:
            return (true, rememberMount(key: "mo", url: url))
        case 1:
            let note = rememberMount(key: "mo", url: url)
            return (true, combineMessages(
                String(localized: "MO image set: \(url.lastPathComponent) (takes effect after a hard reset ⌘R)"),
                note))
        case -3:
            url.stopAccessingSecurityScopedResource()
            return (false, String(localized: "MO image size must be exactly 128, 230, 540 or 640 MB"))
        case -5:
            url.stopAccessingSecurityScopedResource()
            return (false, String(localized: "The guest has locked the MO drive — eject it from the guest first"))
        default:
            url.stopAccessingSecurityScopedResource()
            return (false, String(localized: "MO image mount failed"))
        }
    }

    @discardableResult
    func ejectMO() -> (ok: Bool, message: String?) {
        let result = engine.withEmulationLock { mx68k_mo_eject() }
        mx68k_log("[Swift][iOS] ejectMO mx68k_mo_eject -> \(result)")
        switch result {
        case 0, 1:
            replaceAccessURL(key: "mo", with: nil)
            DiskBookmarkStore.remove(key: "mo")
            return (true, result == 1
                    ? String(localized: "MO image ejected (takes effect after a hard reset ⌘R)")
                    : nil)
        case -5:
            return (false, String(localized: "The guest has locked the MO drive — eject it from the guest first"))
        default:
            return (false, String(localized: "MO eject failed"))
        }
    }

    // MARK: SCSI CD-ROM (ID6 固定スロット)

    /// SCSI CD-ROM(ISO / Mode1)イメージを装着する。ブックマークキーは `"cd"`。
    /// スレッド安全性の理由づけは `insertMO` と完全に同一。
    @discardableResult
    func insertCD(url: URL) -> (ok: Bool, message: String?) {
        guard url.startAccessingSecurityScopedResource() else {
            return accessDeniedResult("insertCD")
        }
        let path = url.path
        let result = engine.withEmulationLock {
            path.withCString { mx68k_cd_insert($0) }
        }
        mx68k_log("[Swift][iOS] insertCD mx68k_cd_insert -> \(result)")
        switch result {
        case 0:
            return (true, rememberMount(key: "cd", url: url))
        case 1:
            let note = rememberMount(key: "cd", url: url)
            return (true, combineMessages(
                String(localized: "CD-ROM image set: \(url.lastPathComponent) (takes effect after a hard reset ⌘R)"),
                note))
        case -3:
            url.stopAccessingSecurityScopedResource()
            return (false, String(localized: "Not a valid CD-ROM image (must be a multiple of the 2048-byte or 2352-byte sector size)"))
        case -5:
            url.stopAccessingSecurityScopedResource()
            return (false, String(localized: "The guest has locked the CD-ROM drive — eject it from the guest first"))
        default:
            url.stopAccessingSecurityScopedResource()
            return (false, String(localized: "CD-ROM image mount failed"))
        }
    }

    @discardableResult
    func ejectCD() -> (ok: Bool, message: String?) {
        let result = engine.withEmulationLock { mx68k_cd_eject() }
        mx68k_log("[Swift][iOS] ejectCD mx68k_cd_eject -> \(result)")
        switch result {
        case 0, 1:
            replaceAccessURL(key: "cd", with: nil)
            DiskBookmarkStore.remove(key: "cd")
            return (true, result == 1
                    ? String(localized: "CD-ROM image ejected (takes effect after a hard reset ⌘R)")
                    : nil)
        case -5:
            return (false, String(localized: "The guest has locked the CD-ROM drive — eject it from the guest first"))
        default:
            return (false, String(localized: "CD-ROM eject failed"))
        }
    }
}
