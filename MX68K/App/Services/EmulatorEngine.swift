import Foundation
import CoreVideo
import CoreMedia          // P697: CMTime(録画フレームの PTS)
import QuartzCore         // P697: CACurrentMediaTime()
import Metal
import MetalKit
import os

/* P703 — iOS のフレーム駆動ターゲット用の最小プロキシ。
 *
 * `CADisplayLink` は target/selector 形式の API しか持たず(ブロック形式は存在しない)、
 * Swift の `@objc` メンバは NSObject 派生クラスにしか置けない。EmulatorEngine 自体を
 * NSObject 派生へ変えると macOS 側の既存クラスにも波及する(そして `#if` で
 * クラス宣言行だけを切り替えることは Swift ではできない)ため、リンクのターゲット
 * 専用の NSObject をここに 1 つ置く。
 *
 * ★所有関係: engine → displayLink → (強参照) proxy → (weak) engine。
 *   CADisplayLink がターゲットを強参照するので proxy は生きたまま保たれ、
 *   proxy 側は weak なので循環参照にならない。 */
#if !os(macOS)
private final class DisplayLinkProxy: NSObject {
    weak var engine: EmulatorEngine?
    init(engine: EmulatorEngine) {
        self.engine = engine
        super.init()
    }
    @objc func tick() { engine?.displayLinkTick() }
}
#endif

class EmulatorEngine: ObservableObject {
    /* P703 — フレーム駆動元。macOS は専用スレッドで回る CVDisplayLink、
     * iOS は**メインスレッド**で回る CADisplayLink(P703 計画 §残留リスク R-4 で
     * 明示的に受容した既知の逸脱。タッチ/キーボードを足す P704 以降で再検討する)。 */
    #if os(macOS)
    private var displayLink: CVDisplayLink?
    #else
    private var displayLink: CADisplayLink?
    private var displayLinkProxy: DisplayLinkProxy?
    #endif
    private var isRunning = false
    private let pauseLock = OSAllocatedUnfairLock(initialState: false)
    private var lastStatusTime: CFAbsoluteTime = 0
    /// P484 — サウンドモニタパネルが表示されている間だけ true。表示中のみ毎フレーム
    /// 更新を行い(P484b)、非表示時は追加コストをゼロにする。SoundMonitorView の
    /// .onAppear / .onDisappear が設定する。
    var soundMonitorVisible: Bool = false
    /// P631 — OPM シンセサイザーパネル(独立ウィンドウ)専用の可視性ゲート。
    /// ★soundMonitorVisible には**相乗りしない**: 相乗りするとサウンドモニタを
    /// 閉じた瞬間に OPM シンセサイザーの更新が止まってしまう(paletteVisible が
    /// P550 で同じ理由から専用フラグにされたのと同じ判断)。
    /// OPMSynthesizerView の .onAppear / .onDisappear が設定する。
    var opmSynthVisible: Bool = false   // ⌘⌥Y
    /* P486: 重量系モニタ(CGImage生成を伴う)のパネル可視性ゲート。soundMonitorVisible
     * (P484b)と同じパターン——Bool 1個、Viewの.onAppear/.onDisappearが設定する。
     * ここでは"更新レート"は変えない(1秒ゲートのまま)。パネルが閉じている間、
     * 該当項目の計算自体をスキップしてコストをゼロにするのが目的。 */
    var spriteMonitorVisible: Bool = false        // ⌘⌥8
    var spriteTableVisible: Bool = false          // ⌘⌥9
    var rendererVisible: Bool = false             // ⌘⌥0
    var textPlaneVisible: Bool = false            // ⌘⌥T
    var bgPageVisible: Bool = false               // ⌘⌥B
    var grpPageVisible: Bool = false              // ⌘⌥G
    var bgspCompositeVisible: Bool = false        // ⌘⌥C
    /* P487: 軽量モニタの可視性ゲート(CPUステータスはStatusBar常時表示のため対象外)。
     * CRTC/VC/BG/Sprite一覧/SpriteTableは毎フレーム更新(ユーザー決定 — 調査の際の
     * レジスタ値ズレを避けるため)。Performanceは1秒集計ウィンドウを維持し可視性
     * ゲートのみ追加(レートを変えるとFPS表示の意味が変わるため)。 */
    var crtcVisible: Bool = false      // ⌘⌥4
    var vcVisible: Bool = false        // ⌘⌥5
    var bgVisible: Bool = false        // ⌘⌥6
    var perfVisible: Bool = false      // ⌘⌥7
    /* P742: DMACレジスタモニタ専用の可視性ゲート。CRTC/VC/BG と同じく
     * DMACMonitorView の .onAppear/.onDisappear が設定する。 */
    var dmacVisible: Bool = false      // ⌘⌥U
    /* P743: 割込み系レジスタモニタ(MFP/IOC/システムポート/IRQLine)専用の
     * 可視性ゲート。CRTC/VC/BG/DMAC と同じく InterruptRegistersMonitorView の
     * .onAppear/.onDisappear が設定する。 */
    var intRegsVisible: Bool = false   // ⌘⌥F
    /* P550: パレットモニタ専用の可視性フラグ。CRTC/VC/BG と同じく、
     * PaletteMonitorView の .onAppear/.onDisappear が設定する。他モニタの
     * フラグに相乗りしない(相乗りすると「BG Viewer を開いている間だけ
     * Palette Viewer が更新される」という誤動作になる)。 */
    var paletteVisible: Bool = false   // ⌘⌥P
    /* P692: ストレージモニタ(SASI/SCSI/MO/CD のマウント状況)専用の可視性ゲート。
     * CRTC/VC/BG と同じく StorageMonitorView の .onAppear/.onDisappear が設定する。
     * 他モニタのフラグには相乗りしない(P550 パレットモニタと同じ理由)。 */
    var storageVisible: Bool = false   // ⌘⌥E
    /* P693: MIDI モニタ(MIDIボード CZ-6BM1 / YM3802 の状態)専用の可視性ゲート。
     * CRTC/VC/BG/Palette/Storage と同じく MIDIMonitorView の .onAppear/.onDisappear
     * が設定する。他モニタのフラグには相乗りしない(P550 パレットモニタと同じ理由)。 */
    var midiVisible: Bool = false      // ⌘⌥N
    /* P694: RTC モニタ(RP5C15 の制御/アラームレジスタ + ホスト時計スナップショット)
     * 専用の可視性ゲート。RTCMonitorView の .onAppear/.onDisappear が設定する。
     * ★命名は crtcVisible(CRTC = 表示コントローラ)と紛れないよう
     * rtcMonitorVisible とする —— CRTC と RTC は別デバイス。 */
    var rtcMonitorVisible: Bool = false   // ⌘⌥L
    /* P695: 入力モニタ(Input Viewer)のキーボードロック LED セクション専用の
     * 可視性ゲート。InputMonitorView の .onAppear/.onDisappear が設定する。
     * ★このパネルの他のセクション(ゲームパッド/マウス)は InputManager.shared
     *   (ホスト側)の Combine 購読だけで更新され、このフラグとは無関係 ——
     *   フラグが効くのは唯一 Bridge 読み取りを伴う keyLED の取得だけ。
     * ★命名は型名 InputMonitorView と字面が近いので配線先に注意
     *   (rtcMonitorVisible と同じ「パネル名 + Visible」規則に従う)。 */
    var inputMonitorVisible: Bool = false   // ⌘⌥I

    /* P487: spritePatterns(⌘⌥8+⌘⌥9共有、1秒のまま据置き)がSprite一覧を必要とするが、
     * Sprite一覧自体は毎フレーム側(spriteMonitorVisible単独条件)に移すため、
     * 1秒側が参照できるよう直近値をここに保持する。 */
    private var latestActiveSprites: [MX68KSpriteEntry] = []
    private var frameCount = 0
    // P211: fixed-step wall-clock pacing for mx68k_run_frame(). CVDisplayLink fires at
    // the display rate; the accumulator gates guest frames to the guest VSYNC rate so
    // tempo tracks real hardware (and ProMotion 120Hz no longer overruns).
    private var accumulator: Double = 0
    private var lastTick: CFAbsoluteTime = 0
    /* P555: ターボ倍率。1 = 通常(既存挙動と完全に同一)、2〜5 = その倍だけ
     * 単位実時間あたりのゲストフレーム数を増やす。runFrame() の固定ステップ
     * アキュムレータの step を 1/N に縮めるだけで、ループ構造自体は非変更。
     * 同期規約: 書込み = メインスレッド(setTurboMultiplier)、読取り =
     * CVDisplayLink スレッド(runFrame)。Bridge 側 g_clock_mhz と同じ
     * 単純変数の設計を踏襲する(Int 1 個の読み書きで、途中値は存在しない)。 */
    private var turboMultiplier: Int = 1

    /// P555 — ターボ倍率を設定する。1 = ターボOFF(既存挙動)。
    func setTurboMultiplier(_ multiplier: Int) {
        turboMultiplier = max(1, min(5, multiplier))
    }

    /* ============================================================================
     * P556 — 真の「ノーウェイト」(倍率上限なし)実行のための専用スレッド機構
     * ============================================================================
     * P555 のターボは CVDisplayLink コールバック内の固定ステップアキュムレータの
     * step を 1/N に縮める方式なので、コールバック自体が ~60Hz でしか呼ばれない
     * 以上、実効倍率には上限がある。ノーウェイトはこの制約を外すため、
     * mx68k_run_frame() を専用のバックグラウンドスレッドで無条件に連続実行する。
     *
     * 【同期規約(Fable5 独立監査 + Code Review 2 往復を経て確定した設計)】
     *
     * 1. emulationLock — mx68k_run_frame() を呼ぶ区間を、CVDisplayLink 側の
     *    アキュムレータループの「各反復」とノーウェイトループの「各反復」の
     *    両方で囲む。フレームバッファ(Bridge 側 P179 のダブルバッファ +
     *    atomic 索引)は **単一ライタ**を前提にした acquire/release ハンドオフ
     *    であり、ノーウェイト ON/OFF の切替瞬間に両駆動元が一時的に重なる窓が
     *    あるため、この共有ロックでその窓を「稀な排他待ち」へ変換する。
     *    定常時は非競合の unfair lock なのでオーバーヘッドは無視できる。
     *
     * 2. noWaitGenerationLock — ループの唯一の脱出条件である世代トークンを保護する。
     *    単純な Int プロパティのままだと、コンパイラがループ内の読取りをホイスト
     *    する可能性を理論上否定できず、それが起きると stopNoWaitThread() の
     *    セマフォ待機が無期限にフリーズする(g_clock_mhz のような「読み違えても
     *    1 フレーム分」で済む値とは危険度が違う)。既存の pauseLock と同じ機構を使う。
     *
     * 3. noWaitStoppedSemaphore — stopNoWaitThread() はスレッドの**実終了**を
     *    待ってから返る。flag を倒して noWaitThread = nil にするだけの設計では、
     *    高速な OFF→ON 往復でスレッドが 2 本並走したまま合流できなくなる
     *    (= 単一ライタ保証の破れ + スレッドリーク)。
     *
     * 4. noWaitActiveLock — runFrame()(CVDisplayLink スレッド)からの
     *    「ノーウェイト中か」の読取り専用フラグ。noWaitThread プロパティ自体は
     *    メインスレッド専用とし、クロススレッド参照はこのロック付きフラグに限る。
     *    ON は thread.start() の**前**に立て、OFF はスレッド合流の**後**に倒す
     *    ため、どちらの向きの切替でも「両方が同時に自分の担当だと思う」窓が
     *    生じない(ロックが release/acquire を兼ねるので accumulator/lastTick の
     *    リセットも CVDisplayLink スレッドから正しく観測される)。
     *
     * 【受容するトレードオフ(意図した仕様、欠陥ではない)】
     * ノーウェイト中はライタ(専用スレッド)がリーダ(Metal 描画)を 1 描画
     * サイクル内に周回し得るため、表示のティアリング(画面の上下が別フレームに
     * なる)が起こり得る。固定長配列 + atomic 索引公開のためメモリ安全性は
     * 保たれる(未定義動作にはならない)。本サイクルでは対策しない。
     * また、CPU コアを 1 つ持続的に高負荷で占有する(これも意図した挙動)。
     */
    private let emulationLock = OSAllocatedUnfairLock()
    private let noWaitGenerationLock = OSAllocatedUnfairLock(initialState: 0)
    private let noWaitActiveLock = OSAllocatedUnfairLock(initialState: false)
    private let noWaitStoppedSemaphore = DispatchSemaphore(value: 0)
    /// メインスレッドからのみ読み書きするプロパティ(クロススレッド参照は
    /// noWaitActiveLock 経由で行う)。
    private var noWaitThread: Thread?

    /* P556: ノーウェイトループがモニタ取得 / onFrame 通知を行う間隔(ゲストフレーム数)。
     * onFrame には現状 consumer が居らず、表示自体は MTKView の連続描画
     * (enableSetNeedsDisplay = false / isPaused = false)が独立に回しているため、
     * この間引きは表示の滑らかさに影響しない。値の根拠: 主目的は
     * DispatchQueue.main.async の発行レートを表示レート(~60Hz)以下に抑えること。
     * ノーウェイト時の現実的な実効レンジ(概ね 500〜4000 fps)で 64 フレーム毎なら
     * 約 8〜62 回/秒に収まり、メインキューを溢れさせない。 */
    private let noWaitMonitorUpdateInterval: UInt64 = 64

    /* P556: ゲスト VSYNC 由来の 1 フレーム分の実時間予算。P555 まで runFrame() の
     * ローカル変数だった値を、ノーウェイトループからも使えるようプロパティへ昇格した。
     * ★値はキャッシュしない(computed property のまま)——31kHz⇔15kHz のモード切替を
     * ライブに追従するという Case A の性質を壊さないため、呼ばれる度に再計算する。 */
    private var currentBaseStep: Double { 1.0 / max(1.0, mx68k_get_vsync_hz()) }

    /* P697: 動画録画(映像のみ)。所有者をこの EmulatorEngine にしているのは、
     * ゲストフレームが 1 つ進むフックがここにしか無いため(ScreenshotService が
     * 静的 enum で済むのに対し、録画はセッション状態を持つ class)。
     * ターボ/ノーウェイト中の録画は EmulatorViewModel 側で禁止されているため、
     * ここでは PTS の壁時計基準だけを守ればよい。 */
    #if os(macOS)
    private let videoRecording = VideoRecordingService()

    /// P697 — 録画中か(UI ミラー用。実体は VideoRecordingService のロック状態)。
    var isRecordingVideo: Bool { videoRecording.isRecording }
    #else
    /// P703 — iOS では録画未対応(AVAssetWriter 側の配線と WindowScaler 依存が
    /// 未整理のため、意図的に非対応)。呼び出し側を分岐させずに済むよう
    /// no-op スタブだけを置く。
    var isRecordingVideo: Bool { false }
    private func captureRecordingFrameIfNeeded() {}
    #endif

    var onFrame: (() -> Void)?
    var onStatusUpdate: ((MX68KStatus) -> Void)?
    var onCRTCStatusUpdate: ((MX68KCRTCStatus) -> Void)?   // P286
    var onVCStatusUpdate: ((MX68KVCStatus) -> Void)?       // P286
    var onBGStatusUpdate: ((MX68KBGStatus) -> Void)?       // P286
    var onDMACStatusUpdate: ((MX68KDMACStatus) -> Void)?   // P742
    var onIntRegsStatusUpdate: ((MX68KIntRegsStatus) -> Void)?   // P743
    var onPaletteStatusUpdate: ((MX68KPaletteStatus) -> Void)?   // P550
    var onStorageStatusUpdate: ((EmulatorStorageStatus) -> Void)?   // P692
    var onMidiStatusUpdate: ((EmulatorMIDIStatus) -> Void)?         // P693
    var onRTCMonitorStatusUpdate: ((MX68K_RTCStatus) -> Void)?      // P694
    /* P695: キーボードロック LED(keyLED 生バイト、負論理)。1 フィールドしか
     * 無いので専用の struct は作らず生バイトのまま渡す。 */
    var onKeyLEDUpdate: ((UInt8) -> Void)?                          // P695
    var onSpriteListUpdate: (([MX68KSpriteEntry]) -> Void)?   // P326
    var onSpriteTableUpdate: (([MX68KSpriteEntry]) -> Void)?  // P328(全128スロット)
    var onSpritePatternsUpdate: (([Int: CGImage]) -> Void)?   // P327
    var onPreviewFrameUpdate: ((CGImage?) -> Void)?            // P326
    var onTextPlaneUpdate: ((CGImage?) -> Void)?              // P343
    var onBGPageUpdate: (([Int: CGImage]) -> Void)?           // P348
    var onGrpPageUpdate: (([Int: CGImage], Bool) -> Void)?    // P348/P690(第2引数=D11経路か)
    var onBGSPCompositeUpdate: ((CGImage?) -> Void)?          // P365
    var onADPCMStatusUpdate: ((MX68K_ADPCMStatus) -> Void)?    // P484
    var onOPMStatusUpdate: ((MX68K_OPMStatus) -> Void)?        // P485
    var onMercuryOPNStatusUpdate: ((MX68K_MercuryOPNStatus) -> Void)?   // P491
    var onMercuryPCMStatusUpdate: ((MX68K_MercuryPCMStatus) -> Void)?   // P635
    var onAudioBufferStatusUpdate: ((MX68K_AudioBufferStatus) -> Void)?  // P549
    var onOPMDetailStatusUpdate: ((MX68K_OPMDetailStatus) -> Void)?      // P631

    // P287: パフォーマンス計測用の集計状態(1秒ウィンドウ毎にリセット)
    private var perfFrameCount = 0
    private var perfTotalFrameTime: Double = 0
    private var perfMaxFrameTime: Double = 0
    private var perfOverBudgetCount = 0

    var onPerfUpdate: ((EmulatorPerfStatus) -> Void)?

    /// P658 — ステータスバー常時表示用の速度%。onPerfUpdate(perfVisible依存)とは
    /// 独立した経路——CPU/MEMと同様、StatusBar常時表示項目は可視性ゲート対象外
    /// (P487方針、33-36行目コメント参照)。
    var onSpeedUpdate: ((Double) -> Void)?

    func start() {
        guard !isRunning else { return }
        isRunning = true
        mx68k_log("[Swift] EmulatorEngine.start() begin")

        #if os(macOS)
        let callback: CVDisplayLinkOutputCallback = { _, _, _, _, _, context in
            let engine = Unmanaged<EmulatorEngine>.fromOpaque(context!).takeUnretainedValue()
            engine.runFrame()
            return kCVReturnSuccess
        }

        CVDisplayLinkCreateWithActiveCGDisplays(&displayLink)
        if let link = displayLink {
            let selfPtr = Unmanaged.passUnretained(self).toOpaque()
            CVDisplayLinkSetOutputCallback(link, callback, selfPtr)
            let result = CVDisplayLinkStart(link)
            mx68k_log(result == kCVReturnSuccess ? "[Swift] CVDisplayLinkStart OK" : "[Swift] CVDisplayLinkStart FAILED")
        } else {
            mx68k_log("[Swift] CVDisplayLinkCreateWithActiveCGDisplays returned nil")
        }
        #else
        /* P703 — ★CADisplayLink の生成と .add(to:forMode:) はメインスレッドで
         * 行わなければならない(リンクはその実行ループへ登録され、以後そこで
         * コールバックが回る)。本サイクルの start() の唯一の呼び出し元は
         * MX68KiOSViewModel.start() であり、これは SwiftUI の .task / .onAppear、
         * すなわちメインスレッド文脈からしか呼ばれない。よって追加の dispatch は
         * 置いていない —— 将来 start() を別スレッドから呼ぶ経路を足す場合は、
         * ここをメインスレッドへ回す必要がある。 */
        let proxy = DisplayLinkProxy(engine: self)
        let link = CADisplayLink(target: proxy, selector: #selector(DisplayLinkProxy.tick))
        link.add(to: .main, forMode: .common)
        displayLinkProxy = proxy
        displayLink = link
        mx68k_log("[Swift] CADisplayLink started (main run loop)")
        #endif
        mx68k_log("[Swift] EmulatorEngine.start() end")
    }

    #if !os(macOS)
    /// P703 — CADisplayLink の 1 ティック。DisplayLinkProxy からのみ呼ばれる
    /// (`private` だと同一ファイル内の別型からは見えないため `fileprivate`)。
    fileprivate func displayLinkTick() {
        runFrame()
    }
    #endif

    func stop() {
        /* P556: 最初に、必ず、無条件にノーウェイトスレッドを同期的に合流させる。
         * EmulatorViewModel.stopEmulation() は engine.stop() の直後に同期的に
         * mx68k_shutdown() を呼ぶため、ノーウェイト実行中に「エミュレーション停止 /
         * アプリ終了」が起きると、Core 状態が破棄された最中・直後に専用スレッドが
         * mx68k_run_frame() を呼び続ける use-after-free 相当のクラッシュ経路になる。
         * ノーウェイトが非アクティブなら stopNoWaitThread() 内の guard で実質 no-op
         * なので、条件を付けずに毎回呼ぶ(条件分岐こそが漏れの原因になるため)。 */
        stopNoWaitThread()
        #if os(macOS)
        if let link = displayLink {
            CVDisplayLinkStop(link)
            displayLink = nil
        }
        #else
        displayLink?.invalidate()
        displayLink = nil
        displayLinkProxy = nil
        #endif
        isRunning = false
        // P211: clear pacing state so the next start() does not see a huge first delta.
        accumulator = 0
        lastTick = 0
    }

    func pause() {
        pauseLock.withLock { $0 = true }
        let msg = "[P336-PAUSE] frame=\(frameCount)"
        msg.withCString { mx68k_log($0) }
    }

    func resume() {
        pauseLock.withLock { $0 = false }
    }

    /// P674 — mx68k_run_frame()/mx68k_pump_pending() と排他制御が必要な、
    /// 稀に発生する同期的なBridge呼び出し(MOのライブOpen/Eject等、
    /// disk.dcacheのdelete/newを伴う操作)のための公開ラッパー。
    /// requestAutoPause()(pauseLock)は実行中のフレームを同期的に待たないため、
    /// 単独ではこの種の操作の安全性を保証できない——実際の排他は
    /// mx68k_run_frame()を囲むのと同じemulationLockで行う必要がある
    /// (Code Review指摘、P674 Fix Plan参照)。
    ///
    /// ★`withLock` ではなく `withLockUnchecked` を呼ぶ理由(実装時の必須の是正、
    /// Fix Plan §5b のコードそのままでは**コンパイルできない**): SDK の
    /// `OSAllocatedUnfairLock<()>.withLock` は
    /// `withLock<R>(_ body: @Sendable () throws -> R) rethrows -> R where R: Sendable`
    /// と宣言されており、制約の無いジェネリック `T` を渡すと
    /// 「`T` が `Sendable` に適合していない」で弾かれる。
    /// `withLockUnchecked` はロックの挙動自体は完全に同一で、コンパイル時の
    /// `Sendable` 検査を省くだけの API(C 相互運用のための標準的な逃し口)。
    /// 公開シグネチャは Fix Plan §5b のまま維持しているので、呼び出し側
    /// (`EmulatorViewModel.insertMO`/`ejectMO`)は計画どおりのコードで動く。
    func withEmulationLock<T>(_ body: () -> T) -> T {
        emulationLock.withLockUnchecked(body)
    }

    // MARK: - P697: 動画録画(映像のみ)

    /* P703 — このセクション全体は macOS 専用。VideoRecordingService が
     * WindowScaler.baseLogicalSize(AppKit 依存)を参照するため、iOS ターゲットへ
     * そのまま持ち込むと連鎖で AppKit が要求される。iOS 側は上の #else 節に
     * 置いた no-op スタブ(isRecordingVideo / captureRecordingFrameIfNeeded)で
     * 受ける ——★呼び出し側(runFrame() 内とノーウェイトループ内の 2 箇所)は
     * 意図的に無改変のままにしてあり、runFrame() 本体はプラットフォーム間で
     * バイト同一に保たれる。 */
    #if os(macOS)

    /// P697 — 録画を開始する。**メインスレッドから呼ぶこと**(EmulatorViewModel 経由)。
    ///
    /// P699 — 動画の寸法は固定 4:3 面(768×576)であり、開始時点のゲスト解像度に
    /// 一切依存しない。よって開始前に framebuffer サイズを取得するガードも不要になった
    /// (あれは寸法取得専用で、「framebuffer 未初期化のまま録画開始」への保護ではない ——
    /// 毎フレームの `captureRecordingFrameIfNeeded()` が独立に nil/ゼロサイズを弾き、
    /// 取得できないフレームは単にスキップされて録画自体は続く)。
    func startRecording(outputURL: URL) throws {
        try videoRecording.start(outputURL: outputURL)
    }

    /// P697 — 録画を停止してファイルを閉じる。`completion` は AVFoundation の内部
    /// キュー上で呼ばれる(UI 更新は呼び出し側でメインスレッドへ回すこと)。
    func stopRecording(completion: @escaping (Bool) -> Void) {
        videoRecording.stop(completion: completion)
    }

    /* P697 — ゲストフレームが 1 つ進むたびに呼ぶ録画フック。
     * ★呼び出し位置は mx68k_run_frame() の直後・emulationLock 区間の**外側**。
     *   framebuffer の読み取りは P179 の「単一ライタ + atomic 索引」設計により
     *   どのスレッドからも読み取り安全なので、ロック区間を伸ばす必要はない。
     * ★mx68k_get_framebuffer() の戻りポインタは次フレームで上書きされるため
     *   その場で Data 化する(ScreenshotService / X68KRenderer と同じ規約)。
     * ★PTS はこのスレッド上でキャプチャと同期的に取得する。専用キューの中で
     *   取得すると、キューが詰まったときに「取得できた時刻」が PTS になり
     *   映像の時間軸が実時間からずれる(VideoRecordingService のコメント参照)。
     * 非録画時は unfair lock の取得 1 回だけで抜ける。 */
    private func captureRecordingFrameIfNeeded() {
        guard videoRecording.isRecording else { return }
        /* P698 — 音声のドレイン。音声は CoreAudio 実時間スレッドが Bridge のリングへ
         * 直接書いており、こちらは消費側だけを担当する。
         * ★映像側の early return(framebuffer が取れない等)より**前**に置く ——
         *   ドレインが止まるとリングが溢れて音が欠けるため、映像の取得可否とは
         *   独立に毎フレーム必ず実行させる。 */
        videoRecording.drainRecordingAudioIfNeeded()
        var w: Int32 = 0, h: Int32 = 0
        guard let ptr = mx68k_get_framebuffer(&w, &h), w > 0, h > 0 else { return }
        let width = Int(w), height = Int(h)
        let bytesPerRow = width * 4
        let data = Data(bytes: ptr, count: bytesPerRow * height)   // 即時コピー(次フレームで上書きされる)
        let geom = currentDisplayGeometry()
        let pts = CMTimeMakeWithSeconds(CACurrentMediaTime(), preferredTimescale: 600)
        videoRecording.appendFrame(bgra: data, width: width, height: height,
                                   bytesPerRow: bytesPerRow, presentationTime: pts, geom: geom)
    }

    #endif   // os(macOS) — P697 録画セクションここまで(P703)

    /* P699 — 録画中のフレームに対応する表示ジオメトリを取得する。
     * ★呼び出しスレッドは CVDisplayLink スレッド(この関数は
     *   captureRecordingFrameIfNeeded() からのみ呼ばれる)。ライブ表示側の
     *   X68KRenderer.updateGeometry() は MTKView の描画スレッドから同じ Bridge 関数を
     *   呼んでいるが、これは「同じスレッドだから安全」なのではなく、**Bridge 側の実装
     *   (P595、ロックフリーのダブルバッファ・アトミックスナップショット)が任意の
     *   スレッドからの呼び出しに対して安全**だから成立する。framebuffer 本体の読み取り
     *   (P179 の単一ライタ + atomic 索引)と同じ性質。
     * ★取得に失敗した場合や値が妥当でない場合は恒等ジオメトリへフォールバックする
     *   (= 4:3 面いっぱい。標準ラスタと同じ見え方で、録画は途切れない)。 */
    private func currentDisplayGeometry() -> DisplayGeometry {
        var gw: Int32 = 0, gh: Int32 = 0
        var hs: Float = 1.0, vs: Float = 1.0
        var ox: Float = 0.0, oy: Float = 0.0
        guard mx68k_get_framebuffer_geom(&gw, &gh, &hs, &vs, &ox, &oy) != nil else {
            return .identity
        }
        guard hs.isFinite, vs.isFinite, ox.isFinite, oy.isFinite, hs > 0, vs > 0 else {
            return .identity
        }
        return DisplayGeometry(hScale: CGFloat(hs), vScale: CGFloat(vs),
                               offX: CGFloat(ox), offY: CGFloat(oy),
                               geoMode: Int(mx68k_get_display_geo_mode()))
    }

    // MARK: - P748 実行制御(ブレークポイント / ステップ実行)の停止通知

    /// P748 — 実行制御による停止を ViewModel へ伝えるコールバック。
    /// ★停止そのものは C 側(m68000_execute のゲート)で起きる。ここは
    ///   「起きたことを Swift 側の一時停止状態へ反映する」ためだけの配線。
    var onDebuggerStopped: (() -> Void)?

    /// 立ち上がりエッジだけを通知するためのラッチ(停止中は毎フレーム
    /// stopped==true が観測されるので、これが無いと通知が毎フレーム飛ぶ)。
    ///
    /// ★このプロパティへのアクセスは**必ず emulationLock 区間の内側**で行う。
    ///   読み書きするのはエミュレーションスレッド(markDebuggerStopEdge)と
    ///   メインスレッド(resetDebuggerStopLatch)の 2 者であり、ロック外だと
    ///   データ競合になる。
    private var debuggerStopNotified = false

    /// P748 — 停止状態の立ち上がりエッジを検出し、「今回通知すべきか」を返す。
    /// ★呼び出しは必ず emulationLock 区間の内側から(上記ラッチの規約)。
    private func markDebuggerStopEdge(_ stopped: Bool) -> Bool {
        if stopped {
            if debuggerStopNotified { return false }
            debuggerStopNotified = true
            return true
        }
        debuggerStopNotified = false
        return false
    }

    /// P748 — step / continue で C 側の stopped を倒すときに、同じロック区間で
    /// エッジ検出ラッチも倒すための入口。
    ///
    /// ★これが無いと 2 回目以降の停止が通知されない: 停止 → pause の後は
    ///   フレーム自体が回らないため「stopped == false」を観測する機会が無く、
    ///   ラッチが立ったままになる。すると Step を押しても engine が再び
    ///   pause されず、CPU は止まっているのにフレームだけが空転し続け
    ///   (MFP/RTC が進み続け)、UI も "Running" のままになる。
    /// ★呼び出し側は必ず withEmulationLock 区間の内側で呼ぶこと。
    func resetDebuggerStopLatch() {
        debuggerStopNotified = false
    }

    private func runFrame() {
        /* P556: ノーウェイト中は駆動元を専用スレッドへ完全に委譲し、CVDisplayLink 側は
         * 本体(アキュムレータループ・モニタ取得・pending 消費)を一切実行しない。
         * pending 消費もノーウェイトループ側が担当するため、ここで何もしなくても
         * リセット等が滞留することはない。表示は Metal 描画側が独立に
         * mx68k_get_framebuffer() を呼び続けるので、この空回りは影響しない。
         * 読取りは noWaitActiveLock 経由(単純プロパティのクロススレッド参照は避ける)。 */
        if noWaitActiveLock.withLock({ $0 }) { return }

        let paused = pauseLock.withLock { $0 }
        if paused {
            // P503 (c1): 一時停止中(設定シート表示等)でも、キュー済みの
            // フレーム境界操作(SRAM クリア / hard・soft リセット / SASI fd
            // キャッシュ無効化 / ステート save・load)だけは消費する。CPU は
            // 実行しないので一時停止の意味論は変わらず、シートを閉じるまで
            // 操作が滞留して成功トーストだけが先に出る誤誘導が無くなる。
            // P556: ノーウェイト側の一時停止分岐も同じ呼出しを持つため、切替の
            // 瞬間に重なり得る。mx68k_run_frame() と同じ emulationLock で囲む。
            emulationLock.withLock {
                mx68k_pump_pending()
            }
            return
        }

        // P211: fixed-step accumulator. Compute wall-clock delta since the last
        // callback (pause guard is above so paused time never leaks into delta),
        // cap it to avoid a catch-up storm after a hitch, and advance one guest
        // frame per elapsed VSYNC step.
        let now = CFAbsoluteTimeGetCurrent()
        if lastTick == 0 { lastTick = now }
        let delta = min(0.05, now - lastTick)
        lastTick = now
        accumulator += delta

        // Case A: re-read the guest VSYNC rate each tick so 31kHz<->15kHz mode
        // switches are tracked live (accumulator stays continuous across a switch).
        // P556: 計算式は P555 以前と同一(currentBaseStep へ切り出しただけ)。
        let baseStep = currentBaseStep
        /* P555: ターボは「1 ゲストフレームあたりの実時間予算」を 1/N に縮めることで
         * 実現する。turbo == 1 のとき step == baseStep となり、以降の計算は
         * P555 以前と完全に一致する(回帰ゼロの要)。 */
        let turbo = turboMultiplier
        let step = baseStep / Double(turbo)
        /* P555: 1 コールバックあたりの追い付き実行回数の上限(スパイラル・オブ・デス
         * 防止)。P555 以前にはこの明示クランプが無く、上の delta 上限(0.05s)だけが
         * 実質的な歯止めだった。実際の自然上限は
         *   floor((step 未満の残余 + 0.05) / step) ≤ floor(0.05 * vhz * turbo) + 1
         * である。
         *
         * P641(D-65): mx68k_get_vsync_hz() が 55.46 / 61.46 の 2 値だけを返す
         * という前提は失効した——CRTC レジスタ(R00/R04/R20/HRL)から動的に導出
         * されるようになり、連続値を取る。したがって固定の 4 * turbo では、
         * vhz が 80Hz を超える設定で自然上限(floor(0.05*vhz*turbo)+1)を**下回り**、
         * 通常経路でも backlog を捨ててしまう(= コマ送り)。上式をそのまま
         * 実装して自然上限に追従させる。vhz の可動域は Core 側の
         * CRTC_MIN/MAX_FRAME_CLOCKS により 30Hz〜130Hz にクランプ済みなので、
         * turbo == 1 でこの値は最大 7、turbo == 5 でも最大 33 に収まる。
         * vhz が異常値でも下限 4 * turbo を保証し、P555 以前の挙動を割り込まない。 */
        let vhz = 1.0 / baseStep
        let naturalCap = Int((0.05 * vhz * Double(turbo)).rounded(.down)) + 1
        let maxCatchUpFrames = max(4 * turbo, naturalCap)
        var catchUpFrames = 0
        let dumpFrames = [10, 30, 60, 120, 180, 360]
        var advanced = false
        while accumulator >= step {
            if catchUpFrames >= maxCatchUpFrames {
                accumulator = 0   // 追い付き不能なので backlog は捨てる(コマ送り暴走の防止)
                break
            }
            catchUpFrames += 1
            if frameCount == 0 {
                mx68k_log("[Swift] runFrame first call")
            }
            let frameStart = CFAbsoluteTimeGetCurrent()
            /* P556: mx68k_run_frame() の呼出しを emulationLock で囲む。ノーウェイトの
             * 切替瞬間に専用スレッドと重なる窓を排他待ちへ変換するのが目的
             * (定常時は非競合なのでコストは無視できる)。ロック区間は
             * mx68k_run_frame() のみ——この関数の内部で render_begin/end と
             * pending-ops の消費が完結しており、Swift 側にそれらの別呼出しは無い。 */
            /* P748: 停止判定を mx68k_run_frame() と**同一のロック区間**で読む。
             * 別区間で読むと 1 命令ぶんずれた瞬間の状態を見うる。コストは
             * volatile 読み 1 回で、チャンク毎ではなくフレーム毎(約55回/秒)。
             * エッジ検出も同区間で行う(ラッチの同期規約)。UI 通知だけは
             * ロックの外で main へ hop する。 */
            let notifyDebuggerStop: Bool = emulationLock.withLock {
                mx68k_run_frame()
                return markDebuggerStopEdge(mx68k_debug_is_stopped() != 0)
            }
            if notifyDebuggerStop {
                DispatchQueue.main.async { [weak self] in self?.onDebuggerStopped?() }
            }
            let frameElapsed = CFAbsoluteTimeGetCurrent() - frameStart
            /* P556: perf カウンタの加算は共有メソッドへ抽出済み(ノーウェイトループ
             * からも同じ処理を呼ぶため)。countOverBudget の条件は P555 と同一——
             * ターボ中は step が意図的に 1/N へ縮み、そのまま比較すると「予算超過」を
             * 常時計上してパフォーマンスモニタの Over 値が無意味になるため、
             * ターボ OFF(turbo == 1、このとき step == baseStep)のときだけ計上する。 */
            recordFrameExecuted(frameElapsed: frameElapsed, baseStep: baseStep, countOverBudget: turbo == 1)
            /* P697: 録画フック。perf 集計(frameElapsed)より後に置き、録画の
             * コピーコストがパフォーマンスモニタの計測値へ混入しないようにする。 */
            captureRecordingFrameIfNeeded()
            accumulator -= step
            advanced = true
            // frameCount / dumpFrames trigger stay inside the loop so each real
            // emulated frame is counted 1:1 even when catch-up runs several.
            frameCount += 1
            if dumpFrames.contains(frameCount) {
                let msg = "[Swift] runFrame dump framebuffer trigger at frame \(frameCount)"
                msg.withCString { mx68k_log($0) }
                mx68k_dump_framebuffer()
            }
        }

        // No guest frame advanced this refresh: leave display/status untouched
        // (the last framebuffer is redisplayed).
        guard advanced else { return }

        // Display notify and status refresh happen once per advancing refresh
        // (display shows the latest framebuffer; the monitor updates ~1/sec).
        DispatchQueue.main.async { [weak self] in
            self?.onFrame?()
        }

        fetchMonitorsAndPerfStats(now: now)
    }

    /* P556: 1 フレーム実行するたびに必ず呼ばれる perf 集計。CVDisplayLink 側の
     * アキュムレータループとノーウェイトループの両方から呼ぶ(ノーウェイト側が
     * この加算を持たないと、ノーウェイト中はパフォーマンスモニタが常に 0 を表示する)。
     * 呼び出しスレッドはどちらか一方のみが実行中であることが noWaitActiveLock +
     * emulationLock により保証されるため、カウンタは無防備な Int のままでよい。 */
    private func recordFrameExecuted(frameElapsed: Double, baseStep: Double, countOverBudget: Bool) {
        mx68k_diag_set_last_frame_ms(frameElapsed * 1000.0)   // P526
        perfFrameCount += 1
        perfTotalFrameTime += frameElapsed
        if frameElapsed > perfMaxFrameTime { perfMaxFrameTime = frameElapsed }
        if countOverBudget && frameElapsed > baseStep { perfOverBudgetCount += 1 }
    }

    /* P556: モニタ取得(1 秒ゲートの重量系 + 毎フレームの軽量系)と perf 集計の
     * 締めを共有メソッドへ抽出した。CVDisplayLink 側・ノーウェイト側のどちらから
     * 呼ばれても「mx68k_get_*_status() 等は mx68k_run_frame() と同一スレッドから
     * 呼ばれる」という既存の安全性前提(下の P484 のコメント参照)が維持される
     * ——これがモニタ取得だけを CVDisplayLink スレッドに残さなかった理由。
     *
     * `now` を引数化したのは、呼び出し側で時刻取得のタイミングが違うため
     * (CVDisplayLink 側はコールバック冒頭で取得済みの値を再利用、ノーウェイト側は
     * ループ内で都度取得)。★両者は必ず同じ時間基準(CFAbsoluteTimeGetCurrent)で
     * 渡すこと——lastStatusTime を共有しているため、基準の異なる時刻を混ぜると
     * 1 秒ゲートが「二度と発火しない」または「巨大な windowSeconds で 1 度だけ
     * 発火する」という壊れ方をする。
     *
     * baseStep はローカル変数だったものを currentBaseStep(呼ぶ度に再計算する
     * computed property)経由へ変更している。 */
    private func fetchMonitorsAndPerfStats(now: CFAbsoluteTime) {
        let baseStep = currentBaseStep
        if now - lastStatusTime >= 1.0 {
            let windowSeconds = now - lastStatusTime   // ★上書き前に算出(Code Review指摘反映)
            lastStatusTime = now
            var status = MX68KStatus()
            mx68k_get_status(&status)

            var spritePatterns: [Int: CGImage] = [:]   // P327
            if spriteMonitorVisible || spriteTableVisible {
                // P487: Sprite一覧は毎フレーム側へ移動したため、直近値を参照する。
                for entry in latestActiveSprites {
                    var rgba = [UInt8](repeating: 0, count: 16 * 16 * 4)
                    rgba.withUnsafeMutableBufferPointer { buf in
                        mx68k_get_sprite_pattern_rgba(Int32(entry.slot), buf.baseAddress)
                    }
                    let data = Data(rgba)
                    if let provider = CGDataProvider(data: data as CFData) {
                        let bitmapInfo = CGBitmapInfo(rawValue:
                            CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
                        if let img = CGImage(width: 16, height: 16, bitsPerComponent: 8, bitsPerPixel: 32,
                                             bytesPerRow: 16 * 4, space: CGColorSpaceCreateDeviceRGB(),
                                             bitmapInfo: bitmapInfo, provider: provider, decode: nil,
                                             shouldInterpolate: false, intent: .defaultIntent) {
                            spritePatterns[Int(entry.slot)] = img
                        }
                    }
                }
            }

            var pw: Int32 = 0, ph: Int32 = 0
            var previewImage: CGImage? = nil
            if rendererVisible {
                if let ptr = mx68k_get_framebuffer(&pw, &ph), pw > 0, ph > 0 {
                    let width = Int(pw), height = Int(ph)
                    let bytesPerRow = width * 4
                    let data = Data(bytes: ptr, count: bytesPerRow * height)   // ptrは次フレームで上書きされるため即コピー(ScreenshotServiceと同じ規約)
                    if let provider = CGDataProvider(data: data as CFData) {
                        let bitmapInfo = CGBitmapInfo(rawValue:
                            CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
                        previewImage = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                                               bytesPerRow: bytesPerRow, space: CGColorSpaceCreateDeviceRGB(),
                                               bitmapInfo: bitmapInfo, provider: provider, decode: nil,
                                               shouldInterpolate: false, intent: .defaultIntent)
                    }
                }
            }
            var textPlaneImage: CGImage? = nil   // P343
            if textPlaneVisible {
                do {
                    var rgba = [UInt8](repeating: 0, count: 1024 * 1024 * 4)
                    rgba.withUnsafeMutableBufferPointer { buf in
                        mx68k_get_text_plane_rgba(buf.baseAddress)
                    }
                    let data = Data(rgba)
                    if let provider = CGDataProvider(data: data as CFData) {
                        let bitmapInfo = CGBitmapInfo(rawValue:
                            CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
                        textPlaneImage = CGImage(width: 1024, height: 1024, bitsPerComponent: 8, bitsPerPixel: 32,
                                                 bytesPerRow: 1024 * 4, space: CGColorSpaceCreateDeviceRGB(),
                                                 bitmapInfo: bitmapInfo, provider: provider, decode: nil,
                                                 shouldInterpolate: false, intent: .defaultIntent)
                    }
                }
            }
            var bgPageImages: [Int: CGImage] = [:]   // P348
            if bgPageVisible {
                for page in 0...1 {
                    var rgba = [UInt8](repeating: 0, count: 1024 * 1024 * 4)
                    var size: Int32 = 0
                    rgba.withUnsafeMutableBufferPointer { buf in
                        mx68k_get_bg_page_rgba(Int32(page), buf.baseAddress, &size)
                    }
                    guard size > 0 else { continue }
                    let dim = Int(size)
                    let data = Data(rgba)
                    if let provider = CGDataProvider(data: data as CFData) {
                        let bitmapInfo = CGBitmapInfo(rawValue:
                            CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
                        if let img = CGImage(width: dim, height: dim, bitsPerComponent: 8, bitsPerPixel: 32,
                                             bytesPerRow: 1024 * 4, space: CGColorSpaceCreateDeviceRGB(),
                                             bitmapInfo: bitmapInfo, provider: provider, decode: nil,
                                             shouldInterpolate: false, intent: .defaultIntent) {
                            bgPageImages[page] = img
                        }
                    }
                }
            }

            var grpPageImages: [Int: CGImage] = [:]   // P348
            var grpPageUsesNemesis = false   // P690
            if grpPageVisible {
                grpPageUsesNemesis = mx68k_get_grp_page_uses_nemesis()
                // P690: ページ数は色モード依存(16色=4 / 256色=2 / 65536色=1 / 非対応=0)。
                for page in 0..<Int(mx68k_get_grp_page_count()) {
                    var rgba = [UInt8](repeating: 0, count: 512 * 512 * 4)
                    let ok: Int32 = rgba.withUnsafeMutableBufferPointer { buf in
                        mx68k_get_grp_page_rgba(Int32(page), buf.baseAddress)
                    }
                    guard ok != 0 else { continue }
                    let data = Data(rgba)
                    if let provider = CGDataProvider(data: data as CFData) {
                        let bitmapInfo = CGBitmapInfo(rawValue:
                            CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
                        if let img = CGImage(width: 512, height: 512, bitsPerComponent: 8, bitsPerPixel: 32,
                                             bytesPerRow: 512 * 4, space: CGColorSpaceCreateDeviceRGB(),
                                             bitmapInfo: bitmapInfo, provider: provider, decode: nil,
                                             shouldInterpolate: false, intent: .defaultIntent) {
                            grpPageImages[page] = img
                        }
                    }
                }
            }

            var bgspCompositeImage: CGImage? = nil   // P365
            if bgspCompositeVisible {
                do {
                    var w: Int32 = 0, h: Int32 = 0
                    var rgba = [UInt8](repeating: 0, count: 1024 * 1024 * 4)
                    rgba.withUnsafeMutableBufferPointer { buf in
                        mx68k_get_bgsp_composite_rgba(buf.baseAddress, &w, &h)
                    }
                    if w > 0 && h > 0 {
                        // C側memcpyは s_render_disp_w をストライドとして詰めるため、
                        // bytesPerRow は Int(w)*4(BG/GRP Page の固定 1024/512 とは違い可変)。
                        let bytesPerRow = Int(w) * 4
                        let data = Data(rgba)
                        if let provider = CGDataProvider(data: data as CFData) {
                            let bitmapInfo = CGBitmapInfo(rawValue:
                                CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
                            bgspCompositeImage = CGImage(width: Int(w), height: Int(h), bitsPerComponent: 8, bitsPerPixel: 32,
                                                         bytesPerRow: bytesPerRow, space: CGColorSpaceCreateDeviceRGB(),
                                                         bitmapInfo: bitmapInfo, provider: provider, decode: nil,
                                                         shouldInterpolate: false, intent: .defaultIntent)
                        }
                    }
                }
            }

            let perf = EmulatorPerfStatus(
                measuredFPS: windowSeconds > 0 ? Double(perfFrameCount) / windowSeconds : 0,
                avgFrameTimeMs: perfFrameCount > 0 ? (perfTotalFrameTime / Double(perfFrameCount)) * 1000 : 0,
                maxFrameTimeMs: perfMaxFrameTime * 1000,
                /* P555: 予算はゲスト VSYNC 由来の baseStep を使う(ターボ中に縮んだ
                 * step を表示すると「予算」の意味が変わり、モニタの読みが過去の
                 * 記録と比較できなくなる)。turbo == 1 では step == baseStep なので
                 * P555 以前と同一の値。 */
                budgetMs: baseStep * 1000,
                overBudgetFrameCount: perfOverBudgetCount,
                framesThisWindow: perfFrameCount
            )
            perfFrameCount = 0
            perfTotalFrameTime = 0
            perfMaxFrameTime = 0
            perfOverBudgetCount = 0
            // P487: perfの集計・リセットは無変更のまま、ディスパッチ(UI通知)のみを
            // 独立させて可視性ゲートを掛ける。パネルが閉じていても平均値の計算自体は
            // 継続する(再度開いた時に不連続な値を出さないため)。
            if perfVisible {
                DispatchQueue.main.async { [weak self] in self?.onPerfUpdate?(perf) }
            }
            // P658: 実行速度%をStatusBar常時表示用に配信する。perfVisible(Performance
            // モニタ表示中)には依存しない——既存のonPerfUpdate配信(perfVisible gated)
            // とは完全に独立した経路として追加し、Performanceモニタパネルの既存挙動を
            // 一切変更しない。
            let vhz = 1.0 / baseStep
            let speedPercent = (perf.measuredFPS / vhz) * 100.0
            DispatchQueue.main.async { [weak self] in self?.onSpeedUpdate?(speedPercent) }
            DispatchQueue.main.async { [weak self] in
                self?.onStatusUpdate?(status)
                self?.onSpritePatternsUpdate?(spritePatterns)   // P327
                self?.onPreviewFrameUpdate?(previewImage)
                self?.onTextPlaneUpdate?(textPlaneImage)   // P343
                self?.onBGPageUpdate?(bgPageImages)   // P348
                self?.onGrpPageUpdate?(grpPageImages, grpPageUsesNemesis)   // P348/P690
                self?.onBGSPCompositeUpdate?(bgspCompositeImage)   // P365
            }
        }

        // P487: 軽量モニタ(CRTC/VC/BG/Sprite一覧/SpriteTable)の毎フレーム更新。
        // P484b/P486と同じ設計: 既存の呼び出しスタック内、新規Timer/DispatchQueueなし、
        // 可視性フラグでゲート。
        if crtcVisible {
            var crtcStatus = MX68KCRTCStatus()
            mx68k_get_crtc_status(&crtcStatus)
            DispatchQueue.main.async { [weak self] in self?.onCRTCStatusUpdate?(crtcStatus) }
        }
        // P742: DMACレジスタモニタ。CRTC等と同型(可視時のみ、同一フレーム内で
        // エミュレーションスレッド自身がスナップショットし main へディスパッチ)。
        if dmacVisible {
            var dmacStatus = MX68KDMACStatus()
            mx68k_get_dmac_status(&dmacStatus)
            DispatchQueue.main.async { [weak self] in self?.onDMACStatusUpdate?(dmacStatus) }
        }
        // P743: 割込み系レジスタモニタ(MFP/IOC/システムポート/IRQLine)。
        // CRTC/DMAC と同型(可視時のみ、同一フレーム内でエミュレーションスレッド
        // 自身がスナップショットし main へディスパッチ)。
        if intRegsVisible {
            var intRegsStatus = MX68KIntRegsStatus()
            mx68k_get_int_regs_status(&intRegsStatus)
            DispatchQueue.main.async { [weak self] in self?.onIntRegsStatusUpdate?(intRegsStatus) }
        }
        if vcVisible {
            var vcStatus = MX68KVCStatus()
            mx68k_get_vc_status(&vcStatus)
            DispatchQueue.main.async { [weak self] in self?.onVCStatusUpdate?(vcStatus) }
        }
        if bgVisible {
            var bgStatus = MX68KBGStatus()
            mx68k_get_bg_status(&bgStatus)
            DispatchQueue.main.async { [weak self] in self?.onBGStatusUpdate?(bgStatus) }
        }
        // P550: パレットモニタ。CRTC/VC/BG と同じ毎フレーム更新+専用可視性ゲート。
        if paletteVisible {
            var paletteStatus = MX68KPaletteStatus()
            mx68k_get_palette_status(&paletteStatus)
            DispatchQueue.main.async { [weak self] in self?.onPaletteStatusUpdate?(paletteStatus) }
        }
        /* P692: ストレージモニタ(SCSI ID0-6 のライブ装着状態)。CRTC/VC/BG/Palette と
         * 同じ「可視時のみ毎フレーム更新」パターンだが、★ロックの扱いだけが違う。
         *
         * 他の軽量モニタ(mx68k_get_crtc_status 等)はフラットなレジスタ構造体を
         * コピーするだけで、読み出し中に解放され得る C++ オブジェクトが無いため
         * emulationLock 無しで安全。対して mx68k_scsi_live_* は SPC 内部の生
         * ポインタ disk[id](Disk*)を逆参照する。MO(ID5)/CD(ID6)のライブ
         * Insert/Eject(EmulatorViewModel.insertMO/ejectMO/insertCD/ejectCD)は
         * メインスレッドから engine.withEmulationLock 越しに delete/new するため
         * (P674/P676 で確立済みの既定)、ここをロック無しで読むと使用中解放に
         * なり得る —— ストレージモニタを開いたまま File→MO/CD→Insert/Eject を
         * 行うだけで踏める通常の利用組合せ。よって 4 関数の呼び出し全体を
         * mx68k_run_frame() と同一の emulationLock で囲む(新規の同期プリミティブ
         * ではなく、既存緩和策の横展開)。この関数は runFrame() が emulationLock を
         * 解放した後に呼ばれるため、再帰取得にはならない。
         *
         * ★mx68k_scsi_live_path() は共有の静的バッファを返すので、各 id の戻り値は
         * 次の id を呼ぶ前に String へ変換すること(貯めてから変換すると全件が
         * 最後の値になる)。 */
        if storageVisible {
            let storageStatus: EmulatorStorageStatus = emulationLock.withLockUnchecked {
                var s = EmulatorStorageStatus()
                s.attachedMask = mx68k_scsi_live_attached_mask()
                s.slots.reserveCapacity(7)
                for id in 0...6 {
                    let kind = Int(mx68k_scsi_live_kind(Int32(id)))
                    let ready = mx68k_scsi_live_ready(Int32(id))
                    // ★即時変換(静的バッファ共有のため、次の反復で上書きされる)
                    let path = String(cString: mx68k_scsi_live_path(Int32(id)))
                    s.slots.append(EmulatorStorageSlot(id: id, kind: kind,
                                                       ready: ready, path: path))
                }
                return s
            }
            DispatchQueue.main.async { [weak self] in self?.onStorageStatusUpdate?(storageStatus) }
        }
        /* P693: MIDI モニタ(MIDIボードの配線/送受信カウンタ/YM3802 レジスタ)。
         * CRTC/VC/BG/Palette と同じ「可視時のみ毎フレーム更新」パターン。
         *
         * ★P692 のストレージモニタと違い emulationLock で囲まない。P692 が
         *   ロックを必要としたのは mx68k_scsi_live_* が SPC 内部の生ポインタ
         *   disk[id](Disk*)を逆参照し、メインスレッドからの MO/CD ライブ
         *   Insert/Eject と使用中解放になり得たため。こちらの 4 系統は
         *   いずれもその条件に当たらない:
         *   - mx68k_midi_get_{tx,rx}_* : _Atomic スカラの読み出しのみ
         *     (Bridge/midi_shadow.h、MX68K_AudioBufferStatus と同型)。
         *   - mx68k_get_midi_regs()    : エミュレーションスレッドだけが書く
         *     Core グローバルのコピー。読み手も同じこのスレッド。
         *   - mx68k_midi_is_wired()    : init/ハードリセットでのみ書かれる
         *     ラッチ後確定値。
         *   - デバイス数 getter        : menu_items[][] の走査(MIDI_Init() で
         *     しか書き換わらない)。生ポインタの逆参照なし。 */
        if midiVisible {
            var midiStatus = EmulatorMIDIStatus()
            midiStatus.wired = mx68k_midi_is_wired()
            midiStatus.outputDeviceCount = Int(mx68k_midi_get_output_device_count())
            midiStatus.inputDeviceCount = Int(mx68k_midi_get_input_device_count())
            midiStatus.txMessages = mx68k_midi_get_tx_messages()
            midiStatus.txBytes = mx68k_midi_get_tx_bytes()
            midiStatus.txLast = mx68k_midi_get_tx_last()
            midiStatus.rxMessages = mx68k_midi_get_rx_messages()
            midiStatus.rxBytes = mx68k_midi_get_rx_bytes()
            midiStatus.rxLast = mx68k_midi_get_rx_last()
            mx68k_get_midi_regs(&midiStatus.regs)
            DispatchQueue.main.async { [weak self] in self?.onMidiStatusUpdate?(midiStatus) }
        }
        /* P694: RTC モニタ(RP5C15)。CRTC/VC/BG/Palette と同じ「可視時のみ毎フレーム
         * 更新」パターンで、P693 の MIDI モニタと同じく emulationLock は不要。
         * mx68k_get_rtc_status() が読むのは Core の RTC_Regs[2][16](フラットな
         * uint8 配列)と RTC_Bank だけで、P692 がロックを必要とした条件
         * (メインスレッドから delete/new される生ポインタの逆参照)に当たらない。
         * 書き手は RTC_Read()/RTC_Write()(ゲスト CPU の I/O アクセス)と
         * RTC_Timer() のみで、いずれも mx68k_run_frame() 内 = このスレッド。 */
        if rtcMonitorVisible {
            var rtcStatus = MX68K_RTCStatus()
            mx68k_get_rtc_status(&rtcStatus)
            DispatchQueue.main.async { [weak self] in self?.onRTCMonitorStatusUpdate?(rtcStatus) }
        }
        /* P695: 入力モニタのキーボードロック LED。CRTC/VC/BG/Palette と同じ
         * 「可視時のみ毎フレーム更新」パターンで、P693 の MIDI モニタ・P694 の
         * RTC モニタと同じく emulationLock は不要。mx68k_get_key_led() が読むのは
         * Core のグローバル uint8 変数 keyLED 1 個だけで、P692 がロックを必要と
         * した条件(メインスレッドから delete/new される生ポインタの逆参照)に
         * 当たらない。書き手は mfp.c の MFP_Write()(ゲスト CPU の I/O アクセス)と
         * ステートロード時の state_mfp_block() の 2 箇所のみで、いずれも
         * mx68k_run_frame() 内 = このスレッド(P198/P481 のキュー方式により
         * ステートロードもエミュレーションスレッド上で処理される)。 */
        if inputMonitorVisible {
            let keyLED = mx68k_get_key_led()
            DispatchQueue.main.async { [weak self] in self?.onKeyLEDUpdate?(keyLED) }
        }
        if spriteMonitorVisible || spriteTableVisible {
            var spriteEntries = [MX68KSpriteEntry](repeating: MX68KSpriteEntry(), count: 128)
            let spriteCount = spriteEntries.withUnsafeMutableBufferPointer { buf in
                mx68k_get_sprite_list(buf.baseAddress, Int32(buf.count))
            }
            let activeSprites = Array(spriteEntries.prefix(Int(spriteCount)))
            latestActiveSprites = activeSprites   // spritePatterns(1秒側、既存)が参照
            if spriteMonitorVisible {
                DispatchQueue.main.async { [weak self] in self?.onSpriteListUpdate?(activeSprites) }
            }
        }
        if spriteTableVisible {
            var fullTable = [MX68KSpriteEntry](repeating: MX68KSpriteEntry(), count: 128)
            _ = fullTable.withUnsafeMutableBufferPointer { buf in
                mx68k_get_sprite_table_full(buf.baseAddress, Int32(buf.count))
            }
            DispatchQueue.main.async { [weak self] in self?.onSpriteTableUpdate?(fullTable) }
        }

        // P484 — サウンドモニタ(ADPCM)の高頻度更新。上の1秒ゲートとは別の if 文
        // だが、意図的に同じ関数・同じ呼び出しスタック(CVDisplayLink スレッド)に
        // 置いている: 波形スナップショットの書込みは mx68k_run_frame() 内部で
        // 行われており、ここから読むかぎりクロススレッド競合は発生しない。
        // パネルが開いている間だけ動作し、閉じていれば Bool 比較のみで抜ける。
        // P484b — 時間しきい値(0.1秒)を撤去し毎フレーム(1/60秒)更新へ。新規の
        // Timer / DispatchQueue は作らず、既存の呼び出しスタック内のまま。
        if soundMonitorVisible {
            var adpcmStatus = MX68K_ADPCMStatus()
            mx68k_get_adpcm_status(&adpcmStatus)
            // P485 — OPM(FM 8ch)も同じゲート・同じ呼び出しスタックで取得する
            // (新規 Timer / DispatchQueue は作らない)。
            var opmStatus = MX68K_OPMStatus()
            mx68k_get_opm_status(&opmStatus)
            // P491 — Mercury Unit の FM 部(YMF288)も同じゲート・同じ呼び出しスタックで取得。
            var mercuryOPNStatus = MX68K_MercuryOPNStatus()
            mx68k_get_mercury_opn_status(&mercuryOPNStatus)
            // P635 — Mercury Unit の PCM 部も同じゲート・同じ呼び出しスタックで取得。
            var mercuryPCMStatus = MX68K_MercuryPCMStatus()
            mx68k_get_mercury_pcm_status(&mercuryPCMStatus)
            // P549 — CoreAudio バッファのアンダーラン統計(セッション累積)も
            // 同じゲート・同じ呼び出しスタックで取得する。
            var audioBufferStatus = MX68K_AudioBufferStatus()
            mx68k_get_audio_buffer_status(&audioBufferStatus)
            DispatchQueue.main.async { [weak self] in
                self?.onADPCMStatusUpdate?(adpcmStatus)
                self?.onOPMStatusUpdate?(opmStatus)
                self?.onMercuryOPNStatusUpdate?(mercuryOPNStatus)
                self?.onMercuryPCMStatusUpdate?(mercuryPCMStatus)
                self?.onAudioBufferStatusUpdate?(audioBufferStatus)
            }
        }

        // P631 — OPM シンセサイザーパネル(独立ウィンドウ)。上の soundMonitorVisible
        // ブロックと同型・同じ呼び出しスタック(CVDisplayLink スレッド)だが、
        // ★意図的に別の if 文にしている: 専用フラグ opmSynthVisible だけで動くため、
        // サウンドモニタを閉じても更新が止まらず、逆にこのウィンドウを閉じても
        // サウンドモニタ側は影響を受けない(両方向の独立性)。
        // 閉じている間は Bool 比較のみで抜け、mx68k_get_opm_detail_status() は
        // 一度も呼ばれない(既存の「非表示時コストゼロ」ゲート規律)。
        if opmSynthVisible {
            var opmDetailStatus = MX68K_OPMDetailStatus()
            mx68k_get_opm_detail_status(&opmDetailStatus)
            DispatchQueue.main.async { [weak self] in
                self?.onOPMDetailStatusUpdate?(opmDetailStatus)
            }
        }
    }

    // MARK: - P556: ノーウェイト実行(専用バックグラウンドスレッド)

    /* P703 — このセクションは `#if` で切らず、iOS ターゲットでも**そのまま**
     * コンパイルする(そのまま通る)。iOS にはノーウェイトを有効化する UI が無く
     * `noWaitActive` は常に false のままなので、stopNoWaitThread() のデッドロック
     * 非発生の根拠(「ノーウェイト中は表示リンク側が早期 return して emulationLock を
     * 取らない」)もそのまま成立する。★この前提が崩れるのは「iOS にノーウェイト UI を
     * 足す」将来サイクルであり、そのときは駆動元がメインスレッド(CADisplayLink)で
     * ある点と併せて再検討すること。 */

    /// P556 — ノーウェイト専用スレッドを起動する。**メインスレッドからのみ呼ぶこと。**
    /// 起動済みなら何もしない(冪等)。
    func startNoWaitThread() {
        assert(Thread.isMainThread, "startNoWaitThread() must be called on the main thread")
        guard noWaitThread == nil else { return }
        // 世代を進めてからスレッドを作る。以降、この世代トークンが変わった時点で
        // ループは自発的に終了する(ループの唯一の脱出条件)。
        let myGeneration = noWaitGenerationLock.withLock { (g: inout Int) -> Int in
            g += 1
            return g
        }
        let thread = Thread { [weak self] in
            self?.noWaitLoop(generation: myGeneration)
        }
        thread.qualityOfService = .userInitiated
        thread.name = "MX68K.NoWaitEmulation"
        noWaitThread = thread
        /* ★順序が重要: CVDisplayLink 側を先に黙らせてから専用スレッドを走らせる。
         * (逆順だと両者が同時に mx68k_run_frame() を呼ぶ窓が広がる。なお、既に
         * コールバック本体へ入ってしまっている 1 回分は emulationLock が受け止める。) */
        noWaitActiveLock.withLock { $0 = true }
        thread.start()
    }

    /// P556 — ノーウェイト専用スレッドを停止し、**その実終了を待ってから返る**。
    /// **メインスレッドからのみ呼ぶこと。** 未起動なら何もしない(冪等)。
    func stopNoWaitThread() {
        assert(Thread.isMainThread, "stopNoWaitThread() must be called on the main thread")
        guard noWaitThread != nil else { return }
        // 世代を進めることで、実行中のループの脱出条件を成立させる。
        noWaitGenerationLock.withLock { $0 += 1 }
        // ループ側の defer が signal するまでブロックする。ここで待つのが本質:
        // 「flag を倒して nil にするだけ」の設計では高速な OFF→ON 往復でスレッドが
        // 2 本並走したまま合流できなくなる(単一ライタ保証の破れ + スレッドリーク)。
        // ループは mx68k_run_frame() 1 回分(ミリ秒オーダー)以内に必ず脱出する。
        // デッドロックしない根拠: ノーウェイト中は CVDisplayLink 側が早期 return して
        // emulationLock を取らないため、ループが待たされる相手が居ない。ループが使う
        // DispatchQueue.main.async は非同期であり、メインスレッドの待機で詰まらない。
        noWaitStoppedSemaphore.wait()
        noWaitThread = nil
        /* P556: CVDisplayLink 側の pacing 状態をリセットしてから再開させる。
         * lastTick が古いままだと復帰直後の delta が巨大になり、大きな catch-up
         * バーストが発生する(lastTick = 0 は runFrame() 側で「現在時刻で再シード」
         * を意味する既存の規約 = P211)。
         * ★順序が重要: これらの書込みは noWaitActiveLock を倒す**前**に行う。
         * ロックの release/acquire により、CVDisplayLink スレッドが「非アクティブ」を
         * 観測した時点でリセット済みの値も必ず観測できる。 */
        accumulator = 0
        lastTick = 0
        lastStatusTime = 0   // 1 秒ゲートの基準も再シード(次フレームで即 1 回更新される)
        noWaitActiveLock.withLock { $0 = false }
    }

    /// P556 — ノーウェイトの実行ループ。専用スレッド上でのみ動く。
    /// アキュムレータ方式は一切経由せず、無条件に 1 フレームずつ連続実行する。
    private func noWaitLoop(generation: Int) {
        // 脱出経路がどれであっても必ず 1 回だけ signal する(stopNoWaitThread() の
        // wait と 1:1 で対応する)。
        defer { noWaitStoppedSemaphore.signal() }
        var frameCounter: UInt64 = 0
        while noWaitGenerationLock.withLock({ $0 == generation }) {
            let paused = pauseLock.withLock { $0 }
            if paused {
                /* P503 (c1) と同じ理由でここでも pending を消費する: 一時停止中でも
                 * キュー済みのフレーム境界操作(SRAM クリア / hard・soft リセット /
                 * SASI fd キャッシュ無効化 / ステート save・load)だけは進める。
                 * これを省くと「成功トーストだけが先に出て操作が滞留する」P503 型の
                 * 回帰がノーウェイト経路で再発する。 */
                emulationLock.withLock {
                    mx68k_pump_pending()
                }
                usleep(1000)
                continue
            }
            let frameStart = CFAbsoluteTimeGetCurrent()
            /* P748: ★駆動元は 2 系統あるので、こちらにも同じ停止判定を置く。
             * 片方だけに配線するとノーウェイト/ターボ中だけブレークポイントが
             * 効かないという欠陥になる(Fix Plan 残留リスク R-7 / テスト T-9)。 */
            let notifyDebuggerStop: Bool = emulationLock.withLock {
                mx68k_run_frame()
                return markDebuggerStopEdge(mx68k_debug_is_stopped() != 0)
            }
            if notifyDebuggerStop {
                DispatchQueue.main.async { [weak self] in self?.onDebuggerStopped?() }
            }
            let frameElapsed = CFAbsoluteTimeGetCurrent() - frameStart
            /* countOverBudget: false — ノーウェイトは意図的な最大速度実行であり
             * 「1 フレームあたりの実時間予算」という概念自体が無意味なため、
             * ターボ中と同じ扱いで Over は計上しない(P555 の方針を踏襲)。 */
            recordFrameExecuted(frameElapsed: frameElapsed, baseStep: currentBaseStep, countOverBudget: false)
            /* P697: 録画フック。★通常はここを通らない —— 録画中はターボ/ノーウェイトの
             * 切替自体が EmulatorViewModel.toggleTurbo() で禁止され、逆にターボ/
             * ノーウェイト中は録画開始が拒否されるため、両者は相互排他。それでも
             * 呼び出しを置くのは、フレーム進行元が 2 系統あるという構造の対称性を
             * 保つため(非録画時は unfair lock の取得 1 回で抜ける)。 */
            captureRecordingFrameIfNeeded()
            frameCounter += 1
            /* ★意図的な仕様: ここでは frameCount(既存、pause() のログ行や
             * dumpFrames トリガが参照)を更新しない。ノーウェイト中はそれらのログの
             * 鮮度が落ちるだけで実害は無く、逆に更新すると 1 秒で数千フレーム進んで
             * dumpFrames の意味が壊れるため。 */
            if frameCounter % noWaitMonitorUpdateInterval == 0 {
                // ★時間基準は CVDisplayLink 側(CFAbsoluteTimeGetCurrent)と必ず
                // 揃える。lastStatusTime を共有しているため、基準を混ぜると
                // 1 秒ゲートが壊れる(fetchMonitorsAndPerfStats のコメント参照)。
                fetchMonitorsAndPerfStats(now: CFAbsoluteTimeGetCurrent())
                DispatchQueue.main.async { [weak self] in self?.onFrame?() }
            }
        }
    }
}
