import AVFoundation
import Foundation

/// P826: 内蔵 MIDI 音源 SC-55(Nuked-SC55)の専用ワーカー。iOS/macOS 共通(P831 で iOS ターゲットにも追加)。
///
/// Nuked-SC55 コアは Load/Reset/Send/Render の全呼び出しの直列化を要求するため、コアへの
/// アクセス(Bridge/sc55_bridge.c の「ワーカー専用」関数)はすべてこのクラスの直列キュー
/// 上でだけ行う。エミュレーションスレッドとは入力リング、CoreAudio 実時間スレッドとは
/// 出力リングでつながる(どちらもロック無しの SPSC、実体は sc55_bridge.c)。
///
/// ★ループ構造(Fix Plan §3 C1): 1 反復 = 1 回の `async`/`asyncAfter` とし、各反復の末尾で
///   次の反復を自分で再投入する。1 つのブロック内で `while` を回すと `queue.sync` が永久に
///   待たされ停止時にハングするため、そうしない。反復と反復の合間に `stop()` の
///   `queue.sync` が割り込める。
final class SC55Worker {
    struct Settings: Equatable {
        var enabled: Bool
        var rom1Path: String
        var rom2Path: String
        var waveRom1Path: String
        var waveRom2Path: String
        var waveRom3Path: String
        /// AudioUnit に実際に設定済みの出力レート(`AudioEngine.currentSampleRate`)。
        var sampleRate: Double
    }

    /// 診断フック。値 N(整数)が設定されていると、最初の反復で不正パスの読込と、合成
    /// ファームウェアによる Load→Send→Render→Convert→push の N チャンク(最低 100)完走を
    /// 試し、結果を debug.log へ出す。
    static var selfTestRequested: Bool {
        guard let env = ProcessInfo.processInfo.environment["MX68K_TEST_SC55_RECONF"],
              let n = Int(env) else { return false }
        return n > 0
    }

    private static let nativeRate: Double = 64_000          // Nuked-SC55 の出力レート(固定)
    private static let renderFrames = 512
    /// 1 チャンクが表す実時間(512/64000 秒 = 8ms)。出力レートに依存しない。
    private static let chunkSeconds = Double(renderFrames) / nativeRate
    /// 出力リングの占有率がこれ以上なら、次の反復を 1 チャンク分遅らせる(フロー制御)。
    private static let throttleOccupancy = 0.75
    private static let pumpMaxBytes: Int32 = 8192
    private static let diagIntervalSeconds = 5.0

    /// QoS は `.userInteractive` 必須(background/utility だと E コアで約 4.3 倍遅くなり
    /// 実時間を割ることを P826 性能実測で確認済み)。
    private let queue = DispatchQueue(label: "MX68K.SC55Worker", qos: .userInteractive)

    // メインスレッド → ワーカーの受け渡し(最新の要求だけを保持する)。
    private let pendingLock = NSLock()
    private var pending: Settings?

    // ---- 以下はワーカーキュー上でのみ触る ----
    private var active = false
    private var loopRunning = false
    /// `stop()` で進める。古い反復の再投入チェーンを確実に止めるための世代番号。
    private var generation = 0
    private var iterations: UInt64 = 0
    private var selfTestDone = false

    private let inFormat = AVAudioFormat(standardFormatWithSampleRate: SC55Worker.nativeRate,
                                         channels: 2)!
    private let inBuffer: AVAudioPCMBuffer
    private var converter: AVAudioConverter?
    private var converterRate: Double = 0
    private var outBuffer: AVAudioPCMBuffer?
    /// 1 チャンクの変換で出てくると見込むフレーム数(ceil(512 × rate / 64000))。
    private var expectedOutFrames = 0
    private var mixBuffer: [Int16] = []
    private var lastDiagTime = DispatchTime.now()

    init() {
        inBuffer = AVAudioPCMBuffer(pcmFormat: inFormat,
                                    frameCapacity: AVAudioFrameCount(SC55Worker.renderFrames))!
    }

    // MARK: - メインスレッドから呼ぶ API

    /// 再構成を要求する(fire-and-forget)。実際の読込・warm-up はワーカーが行う。
    func requestReconfigure(_ settings: Settings) {
        pendingLock.lock()
        pending = settings
        pendingLock.unlock()
        mx68k_sc55_clear_stop_request()   // 電源 OFF → ON の再開に備える
        queue.async { [weak self] in self?.kick() }
    }

    /// 停止する。停止要求フラグを立ててから `queue.sync` で直近の反復が終わるのを待つ。
    /// ★`mx68k_shutdown()`(debug.log を閉じる)より前に呼ぶこと。
    func stop() {
        mx68k_sc55_shutdown()
        queue.sync {
            generation &+= 1
            loopRunning = false
            pendingLock.lock()
            pending = nil
            pendingLock.unlock()
            if active {
                mx68k_sc55_reset()
                active = false
            }
            logDiagnostics(force: true)
            mx68k_log_marker("[P826-SC55-WORKER] iterations=\(iterations)")
        }
    }

    // MARK: - ワーカーキュー

    private func kick() {
        if loopRunning { return }
        loopRunning = true
        iterate(generation)
    }

    private func schedule(_ gen: Int, after delay: Double) {
        if delay <= 0 {
            queue.async { [weak self] in self?.iterate(gen) }
        } else {
            queue.asyncAfter(deadline: .now() + delay) { [weak self] in self?.iterate(gen) }
        }
    }

    private func takePending() -> Settings? {
        pendingLock.lock()
        defer { pendingLock.unlock() }
        let s = pending
        pending = nil
        return s
    }

    private func iterate(_ gen: Int) {
        guard gen == generation else { return }
        if mx68k_sc55_stop_requested() {
            loopRunning = false
            return
        }
        iterations &+= 1

        if let settings = takePending() {
            runSelfTestIfRequested(sampleRate: settings.sampleRate)
            apply(settings)
        }
        guard active, converter != nil else {
            loopRunning = false   // 無効中はループを止める。次の requestReconfigure で再開する。
            return
        }
        logDiagnostics(force: false)

        // 事前チェック: 変換後に出てくる分の空きが無ければ生成自体を見送る(捨てるための
        // Render+Convert を避ける)。1 チャンク分待って再試行。
        if Int(mx68k_sc55_output_free_frames()) < expectedOutFrames + 16 {
            schedule(gen, after: Self.chunkSeconds)
            return
        }
        _ = mx68k_sc55_pump_input(Self.pumpMaxBytes)
        _ = renderConvertPush()

        let capacity = Int(MX68K_SC55_OUTPUT_RING_FRAMES) - 1
        let occupancy = Double(capacity - Int(mx68k_sc55_output_free_frames())) / Double(capacity)
        schedule(gen, after: occupancy >= Self.throttleOccupancy ? Self.chunkSeconds : 0)
    }

    private func apply(_ s: Settings) {
        guard s.enabled else {
            mx68k_sc55_reset()   // 入力停止・出力リング破棄・コアのリセット(status=0)
            if active { mx68k_log_marker("[P826-SC55] disabled") }
            active = false
            return
        }
        if converter == nil || converterRate != s.sampleRate {
            makeConverter(sampleRate: s.sampleRate)
        } else {
            converter?.reset()   // 再読込前の音の残りを持ち越さない
        }
        guard converter != nil else {
            mx68k_sc55_reset()
            active = false
            return
        }
        let loaded = load(s.rom1Path, s.rom2Path, s.waveRom1Path, s.waveRom2Path, s.waveRom3Path)
        guard loaded else {
            active = false   // 失敗理由は Bridge 側が debug.log と status へ出している
            return
        }
        let t0 = DispatchTime.now()
        mx68k_sc55_warmup()
        let warmMs = Double(DispatchTime.now().uptimeNanoseconds - t0.uptimeNanoseconds) / 1_000_000
        active = true
        mx68k_log_marker(String(format: "[P826-SC55] running rate=%.0f warmup_ms=%.0f", s.sampleRate, warmMs))
    }

    private func load(_ p1: String, _ p2: String, _ w1: String, _ w2: String, _ w3: String) -> Bool {
        p1.withCString { c1 in
            p2.withCString { c2 in
                w1.withCString { c3 in
                    w2.withCString { c4 in
                        w3.withCString { c5 in
                            mx68k_sc55_load(c1, c2, c3, c4, c5)
                        }
                    }
                }
            }
        }
    }

    /// converter は出力レートが変わったとき(ハードリセットでの再構成)だけ作り直す。
    /// チャンクごとに作り直すとリサンプラの内部状態が切れ、境界でプチノイズになる。
    private func makeConverter(sampleRate: Double) {
        converter = nil
        outBuffer = nil
        guard sampleRate > 0,
              let outFormat = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 2),
              let conv = AVAudioConverter(from: inFormat, to: outFormat) else {
            mx68k_log_marker("[P826-SC55] converter creation failed rate=\(sampleRate)")
            return
        }
        expectedOutFrames = Int((Double(Self.renderFrames) * sampleRate / Self.nativeRate).rounded(.up))
        let capacity = AVAudioFrameCount(expectedOutFrames + 64)
        guard let buf = AVAudioPCMBuffer(pcmFormat: outFormat, frameCapacity: capacity) else {
            mx68k_log_marker("[P826-SC55] converter buffer allocation failed rate=\(sampleRate)")
            return
        }
        converter = conv
        converterRate = sampleRate
        outBuffer = buf
        mixBuffer = [Int16](repeating: 0, count: Int(capacity) * 2)
    }

    /// 1 チャンク(64kHz × 512 フレーム)を生成 → 出力レートへ変換 → 出力リングへ積む。
    /// 戻り値: (Render 成否, 変換後フレーム数, 積んだフレーム数)。
    @discardableResult
    private func renderConvertPush() -> (renderOK: Bool, outFrames: Int, pushed: Int) {
        guard let conv = converter, let out = outBuffer,
              let inL = inBuffer.floatChannelData?[0], let inR = inBuffer.floatChannelData?[1] else {
            return (false, 0, 0)
        }
        let ok = mx68k_sc55_render(inL, inR, Int32(Self.renderFrames))
        inBuffer.frameLength = AVAudioFrameCount(Self.renderFrames)

        // ストリーミング(pull 型)API。1 回の呼出しで今回の 512 フレームだけを渡し、以降は
        // .noDataNow を返す(ストリーム終端ではないので、リサンプラの状態は次回へ続く)。
        var supplied = false
        var error: NSError?
        let status = conv.convert(to: out, error: &error) { _, inputStatus in
            if supplied {
                inputStatus.pointee = .noDataNow
                return nil
            }
            supplied = true
            inputStatus.pointee = .haveData
            return self.inBuffer
        }
        guard status != .error, let outL = out.floatChannelData?[0],
              let outR = out.floatChannelData?[1] else {
            return (ok, 0, 0)
        }
        let n = Int(out.frameLength)
        if n == 0 { return (ok, 0, 0) }
        for i in 0..<n {
            mixBuffer[i * 2]     = Self.toInt16(outL[i])
            mixBuffer[i * 2 + 1] = Self.toInt16(outR[i])
        }
        let pushed = mixBuffer.withUnsafeBufferPointer { p in
            Int(mx68k_sc55_output_push(p.baseAddress, Int32(n)))
        }
        return (ok, n, pushed)
    }

    private static func toInt16(_ x: Float) -> Int16 {
        let v = (x * 32768).rounded()
        guard v.isFinite else { return 0 }   // Int16(Float) は NaN/∞ でトラップするため
        return Int16(max(-32768, min(32767, v)))
    }

    private func logDiagnostics(force: Bool) {
        let now = DispatchTime.now()
        let elapsed = Double(now.uptimeNanoseconds - lastDiagTime.uptimeNanoseconds) / 1_000_000_000
        guard force || elapsed >= Self.diagIntervalSeconds else { return }
        lastDiagTime = now
        let inDrop = mx68k_sc55_get_and_reset_input_drop_bytes()
        let outDrop = mx68k_sc55_get_and_reset_output_drop_chunks()
        let underruns = mx68k_sc55_get_and_reset_underrun_callbacks()
        if force || inDrop != 0 || outDrop != 0 || underruns != 0 {
            mx68k_log_marker("[P826-SC55] diag in_drop_bytes=\(inDrop) out_drop_chunks=\(outDrop) underrun_callbacks=\(underruns)")
        }
    }

    // MARK: - 診断フック(MX68K_TEST_SC55_RECONF)

    private func runSelfTestIfRequested(sampleRate: Double) {
        guard !selfTestDone,
              let env = ProcessInfo.processInfo.environment["MX68K_TEST_SC55_RECONF"],
              let requested = Int(env), requested > 0 else { return }
        selfTestDone = true
        let chunks = min(max(requested, 100), 100_000)
        mx68k_log_marker("[P826-SC55-TEST] enabled chunks=\(chunks) rate=\(Int(sampleRate))")

        // (1) 存在しないパスでの読込は失敗し、status が負値になること。
        let badOK = load("/nonexistent/sc55_rom1.bin", "/nonexistent/sc55_rom2.bin",
                         "/nonexistent/sc55_waverom1.bin", "/nonexistent/sc55_waverom2.bin",
                         "/nonexistent/sc55_waverom3.bin")
        mx68k_log_marker("[P826-SC55-TEST] badpath load=\(badOK ? 1 : 0) status=\(mx68k_sc55_get_status())")

        // (2) 合成ファームウェア(実在の Roland ROM ではない): rom1 のリセットベクタを 0x0100 に
        //     向け、そこに BRA -2(0x20 0xFE)のビジーループを置く。rom2 と波形 ROM はゼロ埋め。
        //     P826 性能スパイクの構成 B と同じ。
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("mx68k_p826_sc55_test", isDirectory: true)
        let rom1URL = dir.appendingPathComponent("rom1.bin")
        let rom2URL = dir.appendingPathComponent("rom2.bin")
        let waveURL = dir.appendingPathComponent("wave.bin")
        defer { try? FileManager.default.removeItem(at: dir) }
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            var rom1 = Data(count: 0x8000)
            rom1[2] = 0x01
            rom1[0x100] = 0x20
            rom1[0x101] = 0xFE
            try rom1.write(to: rom1URL)
            try Data(count: 0x40000).write(to: rom2URL)
            try Data(count: 0x100000).write(to: waveURL)
        } catch {
            mx68k_log_marker("[P826-SC55-TEST] synth setup failed: \(error.localizedDescription)")
            return
        }
        let ok = load(rom1URL.path, rom2URL.path, waveURL.path, waveURL.path, waveURL.path)
        guard ok else {
            mx68k_log_marker("[P826-SC55-TEST] synth load=0 status=\(mx68k_sc55_get_status())")
            mx68k_sc55_reset()
            return
        }
        makeConverter(sampleRate: sampleRate)
        guard converter != nil else {
            mx68k_sc55_reset()
            return
        }
        mx68k_sc55_warmup()
        let noteOnOff: [UInt8] = [0x90, 60, 100, 0x80, 60, 0]
        let injected = noteOnOff.withUnsafeBufferPointer { p in
            mx68k_sc55_test_inject_midi(p.baseAddress, Int32(p.count))
        }

        var inFrames = 0, outFrames = 0, pushedFrames = 0, renderOK = 0
        for _ in 0..<chunks {
            let r = renderConvertPush()
            inFrames += Self.renderFrames
            outFrames += r.outFrames
            pushedFrames += r.pushed
            if r.renderOK { renderOK += 1 }
        }
        let dropped = mx68k_sc55_get_and_reset_output_drop_chunks()
        mx68k_log_marker("[P826-SC55-TEST] synth chunks=\(chunks) in_frames=\(inFrames) out_frames=\(outFrames) rate=\(Int(sampleRate)) render_ok=\(renderOK) pushed_frames=\(pushedFrames) dropped_chunks=\(dropped) inject=\(injected ? 1 : 0)")

        // 後始末: 合成ファームウェアの音を残さず、実設定の適用(直後の apply)へ引き継ぐ。
        mx68k_sc55_reset()
        converter?.reset()
    }
}
