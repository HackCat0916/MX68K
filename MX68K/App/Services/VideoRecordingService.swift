import Accelerate          // P699: vImageScale_ARGB8888(録画キャンバスへの幾何スケール)
import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import QuartzCore
import os

/// P697 — 動画録画。`AVAssetWriter` +
/// `AVAssetWriterInputPixelBufferAdaptor` を薄くラップする。
/// P698 で音声トラックを追加(映像のみ MVP → 映像+音声)。
/// P699 で録画キャンバスを **固定 4:3 面(768×576)** へ変更(録画開始時点のゲスト
/// 解像度で固定するのをやめ、ライブ表示と同じ矩形計算でスケール配置する)。
///
/// 【所有者】`EmulatorEngine`(ゲストフレームが 1 つ進む場所と同じオブジェクトが持つ)。
/// `ScreenshotService` が `enum`(状態を持たない)なのに対し、こちらは録画セッション
/// という内部状態を持つため `class`。
///
/// 【スレッド規約(Code Review 2 往復を経て確定した設計)】
///
/// 1. フレームバッファのコピーと PTS の取得は **呼び出し元**(`EmulatorEngine` の
///    フレーム進行箇所 = CVDisplayLink スレッド)が、キャプチャと同期的に行う。
///    PTS をこの専用キューの中で計算すると、キューがバックプレッシャーで遅延した
///    ときに「フレームを取得した時刻」ではなく「キューが処理できた時刻」が PTS に
///    なってしまい、映像の時間軸が実時間からずれる。
///
/// 2. `appendFrame` の enqueue と `stop()` の状態遷移 + enqueue は、**単一の
///    `recordingLock` の臨界区間内**にまとめる。「録画中かを確認してから
///    `videoQueue.async` する」までに非ゼロの窓があると、その隙に `stop()` が先に
///    終端処理を enqueue し、遅れて古い `appendFrame` が enqueue されて
///    「終端処理済みの writer に append する」クラッシュ経路が復活し得る。
///    `EmulatorEngine` の `noWaitActiveLock` / `emulationLock` が既に採用している
///    「チェックと副作用を同一ロック臨界区間に含める」パターンと同型。
///    ロックを保持したまま `.async()` を呼ぶのは、enqueue 自体が短時間の
///    非ブロッキング操作のため許容範囲(既存の `emulationLock` 区間と同種)。
///
/// 3. `recordingLock` の状態値そのものが「録画中フラグ」を兼ねる —— CVDisplayLink
///    スレッドから毎フレーム安価に読み書きできる(`noWaitActiveLock` と同じ設計)。
///
/// 4. エンコーダが詰まっている(`isReadyForMoreMediaData == false`)ときは、待たずに
///    そのフレームを**ドロップ**する。リアルタイムスレッド(CVDisplayLink)を
///    絶対にブロックしないため(P693 MIDI の教訓と同型)。
///
/// 【P698 — 音声トラックのスレッド規約(上の 1〜4 と同じ原則の適用)】
///
/// 5. 音声の生成側は **CoreAudio 実時間 I/O スレッド**(`AudioEngine.audioCallback`)。
///    そこからは Bridge の `mx68k_rec_audio_write()` を無条件に呼ぶだけで、Swift 側の
///    ロックもフラグ読み取りも一切介在させない —— 録画中かの判定は Bridge 内部の
///    atomic load 1 回に閉じ込めてある。実時間スレッドではログ出力もできないため
///    (`EmulatorBridge.c:106-108` の P464 設計原則)、リング溢れは Bridge 側の
///    atomic カウンタに積み、**この専用キュー上で** get-and-reset して記録する。
///
/// 6. 音声の消費側(`drainRecordingAudioIfNeeded`)は映像と同じ CVDisplayLink
///    スレッドで、映像フレーム取得の直後に呼ばれる。PTS はここでリング読み出しと
///    同期して取得する(規約 1 と同じ理由)。
///
/// 7. 映像と音声は **同一の時間原点**(`basePTSLock`)を共有する。片方だけが自前の
///    原点を持つと、その原点確定時刻の差(録画開始直後の数十 ms)がそのまま
///    恒久的な音ズレになる。最初にサンプルを出したトラックが原点を決め、
///    もう一方はそれに従う。
///
/// 8. 終端処理は **音声キュー → 映像キュー** の順に鎖状に繋ぐ。両キューとも直列
///    (FIFO)なので、これで「先に enqueue 済みの append がすべて終わってから
///    `markAsFinished()` / `finishWriting()` が走る」ことが両トラックで保証される。
///    映像側だけを先に終端すると、まだ音声キューに残っている append が
///    終端済み writer に届く経路(規約 2 と同種のクラッシュ)が復活する。
final class VideoRecordingService {

    /// 録画開始時に失敗し得る事由。`EmulatorViewModel` 側はトーストで
    /// 「録画を開始できませんでした」と出すだけなので、詳細は debug ログ用。
    enum RecordingError: LocalizedError {
        case alreadyRecording
        case invalidFrameSize(Int, Int)
        case writerCreationFailed(String)
        case writerStartFailed(String)

        var errorDescription: String? {
            switch self {
            case .alreadyRecording:
                return "A recording is already in progress."
            case .invalidFrameSize(let w, let h):
                return "Invalid frame size \(w)x\(h)."
            case .writerCreationFailed(let detail):
                return "Failed to create the movie writer: \(detail)"
            case .writerStartFailed(let detail):
                return "Failed to start writing: \(detail)"
            }
        }
    }

    /// CMTime の timescale。600 は業界標準の慣習値(24/25/30/60fps の公倍数)。
    private static let timescale: CMTimeScale = 600

    /* P699 — 録画キャンバスの固定寸法。
     * ★録画開始時点のゲスト解像度で固定してはならない —— 途中で画面モードが変わると
     *   映像が縮んで(等倍のまま左上へ貼られて)しまう(P698 hands-on で発見)。
     * X68000 の標準的な画面モード(256×256 / 512×512 / 768×512 等)はいずれも物理
     * モニタ上で同一の 4:3 矩形を占める(テクニカルデータブック 表2-10/表2-12、
     * P592/P595 で `[一次資料]` として確定)。ウィンドウ表示側は既にこの事実に基づき
     * 固定 4:3 面へ描いており(`WindowScaler.baseLogicalSize` = P212)、録画も同じ
     * 面を使うことで「録画された映像 = ライブ表示で見えている絵」になる。 */
    private static let canvasSize = WindowScaler.baseLogicalSize

    /// エンコード専用のシリアルキュー。writer/adaptor 系の状態はすべてこの
    /// キュー上でのみ触る(唯一の例外は `start()` で、`videoQueue.sync` により
    /// 同じ直列化に乗せる)。
    private let videoQueue = DispatchQueue(label: "com.mx68k.videorecording", qos: .userInitiated)

    /// P698 — 音声エンコード専用のシリアルキュー。映像とはキューを分けることで、
    /// H.264 エンコードのバックプレッシャーが音声の取り込みを巻き添えにしない。
    private let audioEncodeQueue = DispatchQueue(label: "com.mx68k.videorecording.audio",
                                                 qos: .userInitiated)

    /// 「録画中か」を保持するロック。状態値そのものがフラグを兼ねる(上記 3)。
    private let recordingLock = OSAllocatedUnfairLock(initialState: false)

    /// P698 — 映像/音声が共有する時間原点(上記 7)。最初にサンプルを出した
    /// トラックが確定させ、以降両トラックがこれを 0 とする相対 PTS を書く。
    private let basePTSLock = OSAllocatedUnfairLock<CMTime?>(initialState: nil)

    /// 1 回のドレインで取り出す音声フレーム数の上限。ドレイン周期(約 18ms
    /// @55.46Hz)分は 44.1kHz で約 794 フレームなので 4096 は 5 倍以上の余裕がある。
    /// 取り切れなかった分は Bridge 側リング(8192 フレーム)に残り次回に回る。
    private static let audioDrainCapacityFrames = 4096

    // MARK: - videoQueue 上でのみアクセスする状態

    private var writer: AVAssetWriter?
    private var videoInput: AVAssetWriterInput?
    private var adaptor: AVAssetWriterInputPixelBufferAdaptor?
    private var outputURL: URL?
    private var frameWidth = 0
    private var frameHeight = 0
    /// 直前に append した相対 PTS。AVAssetWriter は狭義単調増加を要求するため、
    /// 追い付き実行で 2 フレームが同一時刻になった場合はここを基準に 1 tick 進める。
    private var lastPTS: CMTime = .invalid
    private var appendedFrames = 0
    private var droppedFrames = 0

    /* P699 — ログ方針。
     * 録画キャンバスは固定 768×576、ソースはゲストの画面モード(256×256 / 512×512 /
     * 768×512 …)なので「キャンバスとソースの寸法が違う」ことは**録画の標準動作**で
     * あり異常イベントではない(P697/P698 の `sizeMismatchLogged` が持っていた
     * 「mid-recording changed = 警告」という意味論は P699 で撤去した)。
     * 記録するのは (a) セッション最初のフレームでの初期状態と、(b) **直前フレームと
     * 実際に変わった**ときの変化のみ —— (b) は真にゲストの画面モードが切り替わった
     * 稀なイベントで、1 変化につき 1 行だけ出る。 */
    private var lastSourceWidth = 0
    private var lastSourceHeight = 0
    private var lastGeom: DisplayGeometry?
    /// スケール処理の失敗ログは 1 セッション 1 回だけ(ログストーム防止、P256-DIAG の教訓)。
    private var scaleErrorLogged = false

    /* P699 — サブ矩形の外側(レターボックス黒帯)を埋めるためのゼロ埋め済みスクラッチ。
     * `setUpWriter()` で 1 回だけ確保し `tearDownWriterState()` で解放する ——
     * 毎フレーム確保すると videoQueue 上に恒常的な malloc/free が乗る。
     * 大きさは録画キャンバス 1 枚分(frameWidth × frameHeight × 4 バイト)。 */
    private var blackScratch: UnsafeMutableRawPointer?

    // MARK: - P698: audioEncodeQueue 上でのみアクセスする状態
    //
    // ★これらは `setUpWriter()`(videoQueue.sync、録画中フラグを立てる**前**)で
    //   書き込まれ、以降は audioEncodeQueue からのみ読まれる。録画中フラグが立つ
    //   前に音声処理が enqueue されることはないため、セットアップの書き込みは
    //   すべての読み取りに happens-before する。破棄は終端処理の鎖(規約 8)により
    //   音声キューの仕事が尽きた後に videoQueue 上で行われるので、これも競合しない。

    private var audioInput: AVAssetWriterInput?
    private var audioFormat: CMAudioFormatDescription?
    /// 音声用の狭義単調増加カウンタ(映像の `lastPTS` とは独立)。
    private var lastAudioPTS: CMTime = .invalid
    private var appendedAudioFrames = 0
    private var droppedAudioChunks = 0
    /// Bridge 側リングが溢れて捨てられた書込み回数の累計(セッション通算)。
    private var ringDropTotal: UInt64 = 0
    /// リング溢れログのスロットリング用。溢れ続けている間に毎回ログを出すと
    /// ログストーム自体が新たな負荷になる(P256-DIAG の教訓)。
    private var audioChunksSinceDropLog = 0

    // MARK: - 公開 API

    /// 録画中か。CVDisplayLink スレッドから毎フレーム呼ばれる(非競合の
    /// unfair lock の取得 1 回だけなのでコストは無視できる)。
    var isRecording: Bool { recordingLock.withLock { $0 } }

    /// 録画を開始する。**メインスレッドから呼ぶこと**(`EmulatorViewModel` 経由)。
    /// セットアップは `videoQueue.sync` で行い、writer 系の状態を最初から
    /// 専用キューに閉じ込める。成功して初めて録画中フラグを立てるため、
    /// 準備途中のフレームが append されることはない。
    ///
    /// P699 — 動画の寸法はゲストの現在解像度に一切依存しない(固定 4:3 面)ので、
    /// 開始時に framebuffer を読む必要がなくなった。
    func start(outputURL: URL) throws {
        guard !isRecording else { throw RecordingError.alreadyRecording }
        var setupError: Error?
        videoQueue.sync {
            do {
                try setUpWriter(outputURL: outputURL)
            } catch {
                setupError = error
                tearDownWriterState()
            }
        }
        if let setupError { throw setupError }
        recordingLock.withLock { $0 = true }
        let canvas = Self.canvasSize
        log("[P699-REC] start canvas \(Int(canvas.width))x\(Int(canvas.height))"
            + " audio=AAC@\(mx68k_get_audio_sample_rate())Hz -> \(outputURL.lastPathComponent)")
    }

    /// 1 ゲストフレーム分の BGRA イメージを録画へ渡す。
    ///
    /// - Parameters:
    ///   - bgra: **呼び出し元で即時コピー済み**の framebuffer(`mx68k_get_framebuffer()`
    ///     の戻りポインタは次フレームで上書きされるため、ここへ渡る時点でコピー必須 ——
    ///     `ScreenshotService` / `X68KRenderer` と同じ規約)。
    ///   - presentationTime: **呼び出し元がキャプチャと同期的に取得した壁時計時刻**
    ///     (`CACurrentMediaTime()`)。ゲストの vsync 周期には依存させない ——
    ///     実際に録画できた実時間をそのまま動画の時間軸にする。
    ///   - geom: P699 — **同じフレームと同一スナップショットの表示ジオメトリ**
    ///     (`mx68k_get_framebuffer_geom()` / `mx68k_get_display_geo_mode()`)。
    ///     4:3 面の中で実際の映像が占める部分矩形を決める(P595/D-55)。標準ラスタでは
    ///     `.identity` で、そのときサブ矩形はキャンバス全体になる。
    func appendFrame(bgra: Data, width: Int, height: Int, bytesPerRow: Int,
                     presentationTime: CMTime, geom: DisplayGeometry) {
        /* ★チェック(録画中か)と副作用(enqueue)を同一の臨界区間に入れる。
         * 分けると stop() との間に窓ができ、終端処理の後に古いフレームが
         * enqueue され得る(クラス冒頭のスレッド規約 2)。
         * `withLockUnchecked` を使う理由は EmulatorEngine.withEmulationLock() の
         * コメントと同じ —— `withLock` は R: Sendable 制約付きで、非 Sendable な
         * self / Data をキャプチャするクロージャでは検査に引っ掛かる。
         * ロックの挙動自体は完全に同一。 */
        recordingLock.withLockUnchecked { active in
            guard active else { return }
            videoQueue.async { [weak self] in
                self?.encodeAndAppend(bgra: bgra, width: width, height: height,
                                      bytesPerRow: bytesPerRow,
                                      presentationTime: presentationTime, geom: geom)
            }
        }
    }

    /// P698 — Bridge の録画専用リングに溜まった音声を取り出して音声トラックへ回す。
    ///
    /// **呼び出し元は `EmulatorEngine` のフレーム進行箇所(CVDisplayLink スレッド)**で、
    /// 映像フレームの取得直後に毎フレーム呼ぶ。非録画時は unfair lock の取得 1 回だけで
    /// 抜ける(`appendFrame` と同じコスト特性)。
    ///
    /// リング読み出しと PTS 取得をこのスレッド上で同期的に済ませるのは映像側と同じ理由 ——
    /// 専用キューの中で時刻を取ると、キューが詰まったときに「処理できた時刻」が
    /// PTS になり実時間からずれる(クラス冒頭の規約 1 / 6)。
    func drainRecordingAudioIfNeeded() {
        /* チェック(録画中か)と副作用(enqueue)を同一の臨界区間に入れる —— 映像側の
         * `appendFrame` とまったく同じ理由(規約 2)。分けると stop() の終端処理の後に
         * 遅れて音声が enqueue され得る。リング読み出しは上限 4096 フレーム
         * (= 16KB)の memcpy で有界なので、この区間に含めても保持時間は問題にならない。 */
        recordingLock.withLockUnchecked { active in
            guard active else { return }

            let capacity = Self.audioDrainCapacityFrames
            var pcm = [Int16](repeating: 0, count: capacity * 2)
            let frames = pcm.withUnsafeMutableBufferPointer { buf -> Int in
                guard let base = buf.baseAddress else { return 0 }
                return Int(mx68k_rec_audio_read(base, Int32(capacity)))
            }
            guard frames > 0 else { return }

            // 実際に読めた分だけを渡す(末尾の未使用領域を無音として書かないため)。
            if frames < capacity { pcm.removeLast((capacity - frames) * 2) }

            let now = CMTimeMakeWithSeconds(CACurrentMediaTime(),
                                            preferredTimescale: Self.timescale)
            audioEncodeQueue.async { [weak self] in
                self?.encodeAndAppendAudio(pcm: pcm, frames: frames, presentationTime: now)
            }
        }
    }

    /// 録画を停止し、ファイルを閉じる。`completion` は成否を伴って **videoQueue 以外の
    /// 内部キュー**(AVAssetWriter の完了ハンドラ)上で呼ばれるため、UI 更新は
    /// 呼び出し側でメインスレッドへ回すこと。
    ///
    /// 既に enqueue 済みの `appendFrame` は直列キューの FIFO 順で必ず終端処理より
    /// 前に処理される —— これがロック設計(規約 2)の狙い。
    func stop(completion: @escaping (Bool) -> Void) {
        recordingLock.withLockUnchecked { active in
            guard active else {
                completion(false)
                return
            }
            active = false
            /* P698: 生成側(CoreAudio 実時間スレッド)の書き込みをここで止める。
             * 以降 mx68k_rec_audio_write() は atomic load 1 回で戻るだけになる。
             * リングに残った最大 1 ドレイン周期分(約 18ms)の音声は破棄される ——
             * その分の壁時計 PTS は取得されておらず、無理に付けると時間軸が崩れるため。
             * 18ms は聴感で判別できず、録画末尾の実害はない。 */
            mx68k_rec_audio_set_enabled(false)
            /* ★終端処理は「音声キュー → 映像キュー」の鎖にする(規約 8)。
             * 両キューとも直列なので、この時点で既に enqueue 済みの append は
             * それぞれのキュー上で必ず終端処理より前に処理される。 */
            audioEncodeQueue.async { [weak self] in
                guard let self else {
                    completion(false)
                    return
                }
                self.finishAudioInput()
                self.videoQueue.async {
                    self.finalizeWriting(completion: completion)
                }
            }
        }
    }

    // MARK: - videoQueue 上でのみ実行される内部処理

    private func setUpWriter(outputURL: URL) throws {
        /* P699 — 録画キャンバスは固定 4:3 面(768×576)。ゲストの現在解像度は見ない。
         * H.264 はマクロブロック単位で符号化するため偶数寸法へ丸める(768/576 は
         * いずれも偶数なので実質 no-op だが、将来 canvasSize が変わった場合の防御と
         * してそのまま残す)。 */
        let canvas = Self.canvasSize
        let w = Int(canvas.width) & ~1
        let h = Int(canvas.height) & ~1
        guard w > 0, h > 0 else {
            throw RecordingError.invalidFrameSize(Int(canvas.width), Int(canvas.height))
        }

        // 同名ファイルが残っていると AVAssetWriter の生成自体が失敗する。
        try? FileManager.default.removeItem(at: outputURL)

        let assetWriter: AVAssetWriter
        do {
            assetWriter = try AVAssetWriter(outputURL: outputURL, fileType: .mp4)
        } catch {
            throw RecordingError.writerCreationFailed(error.localizedDescription)
        }

        let videoSettings: [String: Any] = [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: w,
            AVVideoHeightKey: h,
        ]
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: videoSettings)
        /* ★リアルタイム入力であることを明示する。これを立てないと AVAssetWriter は
         * 「オフライン変換」前提で入力を待たせにかかり、isReadyForMoreMediaData が
         * 落ちやすくなる = ドロップが増える。 */
        input.expectsMediaDataInRealTime = true

        let sourceAttributes: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: w,
            kCVPixelBufferHeightKey as String: h,
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
        ]
        let bufferAdaptor = AVAssetWriterInputPixelBufferAdaptor(
            assetWriterInput: input, sourcePixelBufferAttributes: sourceAttributes)

        guard assetWriter.canAdd(input) else {
            throw RecordingError.writerCreationFailed("canAdd(videoInput) == false")
        }
        assetWriter.add(input)

        // --- P698: 音声トラック ---
        let sampleRate = Double(mx68k_get_audio_sample_rate())
        let audio = AVAssetWriterInput(mediaType: .audio,
                                       outputSettings: Self.audioOutputSettings(sampleRate: sampleRate))
        audio.expectsMediaDataInRealTime = true
        guard assetWriter.canAdd(audio) else {
            throw RecordingError.writerCreationFailed("canAdd(audioInput) == false")
        }
        assetWriter.add(audio)

        /* 取り込む生データ(Bridge のリングから来る PCM)のフォーマット記述。
         * ★これは `audioOutputSettings` が指定する**出力**コーデックとは別物 ——
         *   こちらは常に「Bridge が生成する Int16 リトルエンディアン・ステレオ・
         *   インターリーブ PCM」を表す。AVAssetWriter が必要に応じて出力側へ変換する。 */
        var asbd = AudioStreamBasicDescription(
            mSampleRate: sampleRate,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: 2,
            mBitsPerChannel: 16,
            mReserved: 0)
        var format: CMAudioFormatDescription?
        let fmtStatus = CMAudioFormatDescriptionCreate(
            allocator: kCFAllocatorDefault, asbd: &asbd,
            layoutSize: 0, layout: nil, magicCookieSize: 0, magicCookie: nil,
            extensions: nil, formatDescriptionOut: &format)
        guard fmtStatus == noErr, let format else {
            throw RecordingError.writerCreationFailed(
                "CMAudioFormatDescriptionCreate failed (OSStatus=\(fmtStatus))")
        }

        guard assetWriter.startWriting() else {
            throw RecordingError.writerStartFailed(
                assetWriter.error?.localizedDescription ?? "startWriting() == false")
        }
        assetWriter.startSession(atSourceTime: .zero)

        writer = assetWriter
        videoInput = input
        adaptor = bufferAdaptor
        self.outputURL = outputURL
        frameWidth = w
        frameHeight = h
        basePTSLock.withLockUnchecked { $0 = nil }
        lastPTS = .invalid
        appendedFrames = 0
        droppedFrames = 0
        lastSourceWidth = 0
        lastSourceHeight = 0
        lastGeom = nil
        scaleErrorLogged = false

        /* P699 — 黒帯用のゼロ埋めスクラッチを 1 回だけ確保する(`calloc` はゼロ初期化
         * 済みの領域を返すので、以降このバッファの内容は書き換えない = 常にゼロ)。
         * 確保に失敗しても録画は続行できる(`fillBlack()` が `memset` へフォールバック)。 */
        free(blackScratch)
        blackScratch = calloc(1, w * h * 4)

        audioInput = audio
        audioFormat = format
        lastAudioPTS = .invalid
        appendedAudioFrames = 0
        droppedAudioChunks = 0
        ringDropTotal = 0
        audioChunksSinceDropLog = 0

        /* ★順序が重要: リングを初期化してから生成側を有効化する。逆にすると
         * 有効化〜初期化の隙間に書き込まれたデータを初期化が消してしまい、
         * さらに生成側が動作中に索引をゼロへ戻すことで SPSC の不変条件が壊れる。 */
        mx68k_rec_audio_reset()
        mx68k_rec_audio_set_enabled(true)
    }

    /* P698 — 音声トラックの出力設定。
     *
     * ★AAC を選んだ理由(実測に基づく計画からの逸脱、報告済み):
     *   当初計画は「Bridge が既に Int16 PCM を持っているので変換コスト最小」を根拠に
     *   LPCM(非圧縮)を指定していた。しかし .mp4 コンテナへ LPCM を書くと、
     *   AVAssetWriter は version 1 の 'chnl' box を出力し、**FFmpeg 系のツールが
     *   ファイル全体を読めなくなる**(ffprobe 8.1.1 で
     *   "Unsupported 'chnl' box with version 1" → "Invalid data found"、
     *   音声だけでなく映像トラックごと読めない)。VLC・Discord・ブラウザ・多くの
     *   編集ソフトは FFmpeg 系であり、本機能の目的である「動作の共有・リモートでの
     *   サウンド確認」がそのまま達成できなくなる(再生できるのは QuickTime 等
     *   AVFoundation 系のみ)。AAC なら同じ入力経路のまま ffprobe が両トラックを
     *   正常に認識し、ファイルサイズも実測で約 1/33 になる。
     *   エンコード負荷は専用シリアルキュー上の 44.1kHz ステレオ AAC であり軽微。
     *   LPCM へ戻す場合はこの関数の戻り値を差し替えるだけでよい(他の変更は不要)。 */
    private static func audioOutputSettings(sampleRate: Double) -> [String: Any] {
        [
            AVFormatIDKey: kAudioFormatMPEG4AAC,
            AVSampleRateKey: sampleRate,
            AVNumberOfChannelsKey: 2,
            AVEncoderBitRateKey: 192_000,
        ]
    }

    /// 映像/音声で共有する時間原点を解決する(クラス冒頭の規約 7)。
    /// 最初の呼び出しが `candidate` を原点として確定させ、以降は常にそれを返す。
    private func resolveBasePTS(_ candidate: CMTime) -> CMTime {
        basePTSLock.withLockUnchecked { state in
            if let existing = state { return existing }
            state = candidate
            return candidate
        }
    }

    private func encodeAndAppend(bgra: Data, width: Int, height: Int,
                                 bytesPerRow: Int, presentationTime: CMTime,
                                 geom: DisplayGeometry) {
        guard let adaptor, let videoInput, let writer else { return }
        // 書き込み中に writer が失敗状態へ落ちた場合(ディスク満杯等)は静かに捨てる。
        guard writer.status == .writing else {
            droppedFrames += 1
            return
        }
        /* エンコーダが詰まっているならフレームを捨てる。ここで待つ設計にすると
         * 直列キューが伸び、最終的に呼び出し元(CVDisplayLink スレッド)の
         * enqueue 先が肥大化してメモリを食う。 */
        guard videoInput.isReadyForMoreMediaData else {
            droppedFrames += 1
            return
        }

        /* --- PTS: 最初のサンプルを 0 とする相対時刻へ変換し、狭義単調増加を保証する ---
         * P698: 原点は音声トラックと共有する(クラス冒頭の規約 7)。 */
        var pts = CMTimeSubtract(presentationTime, resolveBasePTS(presentationTime))
        if pts < .zero { pts = .zero }
        if lastPTS.isValid && pts <= lastPTS {
            /* 追い付き実行(1 コールバックで複数ゲストフレーム)や壁時計の
             * 解像度により同一時刻が来ることがある。AVAssetWriter は狭義単調増加を
             * 要求するので 1 tick だけ進める。 */
            pts = CMTimeAdd(lastPTS, CMTime(value: 1, timescale: Self.timescale))
        }

        guard let pixelBuffer = makePixelBuffer(adaptor: adaptor) else {
            droppedFrames += 1
            return
        }

        copyIntoPixelBuffer(pixelBuffer, bgra: bgra, width: width, height: height,
                            bytesPerRow: bytesPerRow, geom: geom)

        if adaptor.append(pixelBuffer, withPresentationTime: pts) {
            lastPTS = pts
            appendedFrames += 1
        } else {
            droppedFrames += 1
            let detail = writer.error?.localizedDescription ?? "unknown"
            log("[P697-REC] append failed (status=\(writer.status.rawValue)): \(detail)")
        }
    }

    /// pixelBufferPool から 1 枚確保する。pool が使えない場合(まだ生成されていない/
    /// 枯渇)は単発確保にフォールバックする。
    private func makePixelBuffer(adaptor: AVAssetWriterInputPixelBufferAdaptor) -> CVPixelBuffer? {
        var pixelBuffer: CVPixelBuffer?
        if let pool = adaptor.pixelBufferPool {
            let status = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixelBuffer)
            if status == kCVReturnSuccess, let pixelBuffer { return pixelBuffer }
        }
        let attrs: [String: Any] = [
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
        ]
        let status = CVPixelBufferCreate(kCFAllocatorDefault, frameWidth, frameHeight,
                                         kCVPixelFormatType_32BGRA, attrs as CFDictionary,
                                         &pixelBuffer)
        return status == kCVReturnSuccess ? pixelBuffer : nil
    }

    /// P699 — ソースの BGRA バイト列を、固定 4:3 の録画キャンバスへ**スケール配置**する。
    ///
    /// P697/P698 の「等倍のまま左上へ貼り付け、残りを黒で埋める」設計を全面置換したもの。
    /// 旧設計では録画開始時と違う解像度モードのフレームが届くと映像が縮んで見えた
    /// (P698 hands-on でユーザーが発見した不具合)。
    ///
    /// 手順:
    ///   1. `DisplayViewport.fitMetal()` でキャンバス内のサブ矩形を求める。これは
    ///      **ライブ表示(`X68KRenderer.draw()` の viewport)およびマウス写像と同一の
    ///      単一情報源**(P196/P595)であり、録画側で矩形を別計算しないことで
    ///      「録画とライブ表示がずれる」類の回帰を構造的に防ぐ。
    ///      標準ラスタ(`geom == .identity`)ではサブ矩形 = キャンバス全体、非標準
    ///      CRTC 構成(D-54/D-55/D-59/D-68)では 4:3 面の中の部分矩形になる。
    ///   2. サブ矩形の外側は黒で埋める(ライブ表示のレターボックス黒帯と同じ見え方)。
    ///   3. ソースをサブ矩形の寸法へ幾何スケールして、その位置へ書き込む。
    ///
    /// ★チャンネル順序についての設計メモ: `vImageScale_ARGB8888` は BGRA と ARGB を
    ///   **区別しない**。関数名に ARGB とあるが、実体は「8bit × 4 チャンネル・
    ///   インターリーブ」という幾何構造だけに依存する純粋なスケール処理で、どの
    ///   チャンネルが何色かを一切解釈しない(4 チャンネルすべてに同一の補間が掛かる)。
    ///   よって BGRA8888 バッファへそのまま適用してよい —— Accelerate の標準的な用法で
    ///   あり、チャンネル入れ替えは発生しない。
    ///
    /// ★完全一致の高速パス(ソースが偶然サブ矩形と同寸のときの直接コピー)は複雑化を
    ///   避けるため設けていない。等倍のスケールは事実上のコピーとして処理される。
    private func copyIntoPixelBuffer(_ pixelBuffer: CVPixelBuffer, bgra: Data,
                                     width: Int, height: Int, bytesPerRow: Int,
                                     geom: DisplayGeometry) {
        CVPixelBufferLockBaseAddress(pixelBuffer, [])
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, []) }
        guard let dstBase = CVPixelBufferGetBaseAddress(pixelBuffer) else { return }
        let dstBytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
        let dstWidth = CVPixelBufferGetWidth(pixelBuffer)
        let dstHeight = CVPixelBufferGetHeight(pixelBuffer)

        /* --- 1. キャンバス全面を黒で埋める ---
         * pixelBufferPool から回ってきたバッファには前フレームの内容が残っているため、
         * サブ矩形の外側だけでなく毎回全面を埋める(サブ矩形は直後に上書きされる)。 */
        fillBlack(dstBase, dstBytesPerRow: dstBytesPerRow, dstHeight: dstHeight)

        // --- 2. サブ矩形(ライブ表示と同一の計算) ---
        let rect = DisplayViewport.fitMetal(
            container: CGSize(width: frameWidth, height: frameHeight), geom: geom)
        var dx = Int(rect.x.rounded())
        var dy = Int(rect.y.rounded())
        var dw = Int(rect.w.rounded())
        var dh = Int(rect.h.rounded())
        // 想定外の geom が来てもキャンバス外へ書かない(はみ出し分は切り詰める)。
        if dx < 0 { dw += dx; dx = 0 }
        if dy < 0 { dh += dy; dy = 0 }
        dw = min(dw, dstWidth  - dx)
        dh = min(dh, dstHeight - dy)

        logGeometryIfChanged(sourceWidth: width, sourceHeight: height, geom: geom,
                             x: dx, y: dy, w: dw, h: dh)

        guard dw > 0, dh > 0, width > 0, height > 0, bytesPerRow > 0 else { return }

        // --- 3. ソースをサブ矩形へ幾何スケール ---
        bgra.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
            // 呼び出し元の即時コピーが短ければ(理論上あり得ないが)何もしない。
            guard let srcBase = src.baseAddress, bytesPerRow * height <= src.count else { return }
            /* vImage は src を読み取るだけだが、`vImage_Buffer.data` が可変ポインタ型で
             * あるため const を外して渡す(Accelerate の標準的な使い方)。 */
            var srcBuffer = vImage_Buffer(data: UnsafeMutableRawPointer(mutating: srcBase),
                                          height: vImagePixelCount(height),
                                          width: vImagePixelCount(width),
                                          rowBytes: bytesPerRow)
            var dstBuffer = vImage_Buffer(data: dstBase.advanced(by: dy * dstBytesPerRow + dx * 4),
                                          height: vImagePixelCount(dh),
                                          width: vImagePixelCount(dw),
                                          rowBytes: dstBytesPerRow)
            let err = vImageScale_ARGB8888(&srcBuffer, &dstBuffer, nil,
                                           vImage_Flags(kvImageNoFlags))
            if err != kvImageNoError && !scaleErrorLogged {
                scaleErrorLogged = true
                log("[P699-REC] vImageScale_ARGB8888 failed: err=\(err)"
                    + " src=\(width)x\(height) dst=\(dw)x\(dh)")
            }
        }
    }

    /// キャンバスを黒(全チャンネル 0)で埋める。ソースは `setUpWriter()` で 1 回だけ
    /// 確保したゼロ埋めスクラッチ(毎フレームの確保を避けるため)。
    /// 宛先の stride(`dstBytesPerRow`)はキャンバス幅より大きいことがあるので行単位で埋める。
    private func fillBlack(_ dstBase: UnsafeMutableRawPointer,
                           dstBytesPerRow: Int, dstHeight: Int) {
        guard let scratch = blackScratch else {
            // スクラッチ確保に失敗していた場合の予備経路(録画は続行させる)。
            memset(dstBase, 0, dstBytesPerRow * dstHeight)
            return
        }
        let rowBytes = min(frameWidth * 4, dstBytesPerRow)
        guard rowBytes > 0 else { return }
        for row in 0..<dstHeight {
            memcpy(dstBase.advanced(by: row * dstBytesPerRow), scratch, rowBytes)
        }
    }

    /// P699 — セッション最初のフレーム、および**直前フレームから実際に変化した**
    /// ときだけ、ソース解像度とサブ矩形を情報ログとして記録する。
    /// 「キャンバスとソースの寸法が違う」こと自体は録画の標準動作であり、
    /// 警告として扱わない(P697 の `sizeMismatchLogged` の意味論は撤去済み)。
    private func logGeometryIfChanged(sourceWidth: Int, sourceHeight: Int,
                                      geom: DisplayGeometry,
                                      x: Int, y: Int, w: Int, h: Int) {
        let first = (lastSourceWidth == 0 && lastSourceHeight == 0)
        guard first || sourceWidth != lastSourceWidth || sourceHeight != lastSourceHeight
                || geom != lastGeom else { return }
        let previous: String? = first ? nil : "\(lastSourceWidth)x\(lastSourceHeight)"
        lastSourceWidth = sourceWidth
        lastSourceHeight = sourceHeight
        lastGeom = geom
        let from = previous.map { "\($0) -> " } ?? ""
        log("[P699-REC] canvas \(frameWidth)x\(frameHeight), source \(from)"
            + "\(sourceWidth)x\(sourceHeight) geoMode=\(geom.geoMode)"
            + " -> subrect (\(x),\(y),\(w),\(h))")
    }

    // MARK: - P698: audioEncodeQueue 上でのみ実行される内部処理

    private func encodeAndAppendAudio(pcm: [Int16], frames: Int, presentationTime: CMTime) {
        /* Bridge 側のリング溢れ回数をここで回収する。実時間スレッドではログを
         * 出せないため、記録はこの非リアルタイムキューの担当(get-and-reset 方式)。
         * ★リングから 1 度も読めなかった回は encodeAndAppendAudio 自体が呼ばれない
         *   ので、回収はこの 1 箇所で足りる(溢れているならデータは来ている)。 */
        pollRingDropCount()

        guard let audioInput, let audioFormat, let writer else { return }
        guard writer.status == .writing else {
            droppedAudioChunks += 1
            return
        }
        guard audioInput.isReadyForMoreMediaData else {
            droppedAudioChunks += 1
            return
        }

        // --- PTS: 映像と共有する原点からの相対時刻 + 狭義単調増加の保証 ---
        var pts = CMTimeSubtract(presentationTime, resolveBasePTS(presentationTime))
        if pts < .zero { pts = .zero }
        if lastAudioPTS.isValid && pts <= lastAudioPTS {
            pts = CMTimeAdd(lastAudioPTS, CMTime(value: 1, timescale: Self.timescale))
        }

        guard let sampleBuffer = makeAudioSampleBuffer(pcm: pcm, frames: frames,
                                                       pts: pts, format: audioFormat) else {
            droppedAudioChunks += 1
            return
        }

        if audioInput.append(sampleBuffer) {
            lastAudioPTS = pts
            appendedAudioFrames += frames
        } else {
            droppedAudioChunks += 1
            let detail = writer.error?.localizedDescription ?? "unknown"
            log("[P698-REC] audio append failed (status=\(writer.status.rawValue)): \(detail)")
        }
    }

    /// Bridge の Int16 インターリーブ PCM から `CMSampleBuffer` を 1 つ組み立てる。
    private func makeAudioSampleBuffer(pcm: [Int16], frames: Int, pts: CMTime,
                                       format: CMAudioFormatDescription) -> CMSampleBuffer? {
        let bytesPerFrame = 4                       // Int16 × 2ch
        let byteCount = frames * bytesPerFrame

        var blockBuffer: CMBlockBuffer?
        var status = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: byteCount,
            blockAllocator: kCFAllocatorDefault, customBlockSource: nil,
            offsetToData: 0, dataLength: byteCount, flags: 0, blockBufferOut: &blockBuffer)
        guard status == kCMBlockBufferNoErr, let blockBuffer else { return nil }

        status = pcm.withUnsafeBytes { raw -> OSStatus in
            guard let base = raw.baseAddress else { return OSStatus(-1) }
            return CMBlockBufferReplaceDataBytes(with: base, blockBuffer: blockBuffer,
                                                 offsetIntoDestination: 0, dataLength: byteCount)
        }
        guard status == kCMBlockBufferNoErr else { return nil }

        /* duration は 1 サンプル分。sampleCount と合わせて「このバッファは
         * frames サンプル分」を表す(タイムスケールはサンプルレートそのもの)。 */
        let rate = CMTimeScale(format.audioStreamBasicDescription?.mSampleRate ?? 44100)
        var timing = CMSampleTimingInfo(duration: CMTime(value: 1, timescale: rate),
                                        presentationTimeStamp: pts, decodeTimeStamp: .invalid)
        var sampleSize = bytesPerFrame
        var sampleBuffer: CMSampleBuffer?
        status = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault, dataBuffer: blockBuffer, formatDescription: format,
            sampleCount: frames, sampleTimingEntryCount: 1, sampleTimingArray: &timing,
            sampleSizeEntryCount: 1, sampleSizeArray: &sampleSize, sampleBufferOut: &sampleBuffer)
        guard status == noErr else { return nil }
        return sampleBuffer
    }

    /// Bridge のリング溢れカウンタを回収し、必要ならログへ出す。
    /// 溢れ続けている間に毎回ログすると、ログ出力自体が新たな負荷になって
    /// 症状を悪化させる(P256-DIAG の教訓)ため、頻度を絞る。
    private func pollRingDropCount() {
        let drops = UInt64(mx68k_rec_audio_get_and_reset_drop_count())
        guard drops > 0 else {
            audioChunksSinceDropLog += 1
            return
        }
        let firstDrop = (ringDropTotal == 0)
        ringDropTotal += drops
        audioChunksSinceDropLog += 1
        // 最初の 1 回は必ず出し、以降は約 5 秒(≒300 ドレイン)に 1 回へ間引く。
        if firstDrop || audioChunksSinceDropLog >= 300 {
            audioChunksSinceDropLog = 0
            log("[P698-REC] audio ring overrun: dropped writes total=\(ringDropTotal) "
                + "(maxWriteFrames=\(mx68k_rec_audio_get_max_write_frames()))")
        }
    }

    /// 音声トラックを閉じる。**audioEncodeQueue 上で、既存の append がすべて
    /// 処理された後に実行される**(規約 8 の鎖の 1 段目)。
    private func finishAudioInput() {
        pollRingDropCount()
        let frames = appendedAudioFrames
        let drops = droppedAudioChunks
        log("[P698-REC] audio stop: frames=\(frames) droppedChunks=\(drops) "
            + "ringDrops=\(ringDropTotal) maxWriteFrames=\(mx68k_rec_audio_get_max_write_frames())")
        // writer が既に失敗状態なら markAsFinished は呼ばない(映像側と同じ判断)。
        if let audioInput, let writer, writer.status == .writing {
            audioInput.markAsFinished()
        }
    }

    private func finalizeWriting(completion: @escaping (Bool) -> Void) {
        guard let writer, let videoInput else {
            tearDownWriterState()
            completion(false)
            return
        }
        let frames = appendedFrames
        let drops = droppedFrames
        let url = outputURL
        log("[P697-REC] stop: appended=\(frames) dropped=\(drops)")

        guard writer.status == .writing else {
            let detail = writer.error?.localizedDescription ?? "status=\(writer.status.rawValue)"
            log("[P697-REC] finalize skipped: \(detail)")
            tearDownWriterState()
            completion(false)
            return
        }

        videoInput.markAsFinished()
        writer.finishWriting { [weak self] in
            let ok = (writer.status == .completed) && frames > 0
            if !ok {
                let detail = writer.error?.localizedDescription ?? "status=\(writer.status.rawValue)"
                self?.log("[P697-REC] finishWriting failed: \(detail)")
            } else if let url {
                self?.log("[P697-REC] saved: \(url.lastPathComponent)")
            }
            /* 状態の後片付けも videoQueue 上で行い、writer 系プロパティの
             * アクセスを最後まで 1 本のキューに閉じ込める。 */
            self?.videoQueue.async { self?.tearDownWriterState() }
            completion(ok)
        }
    }

    private func tearDownWriterState() {
        /* ★不変条件: writer 状態を破棄したなら、録画用リングへの書き込みは必ず
         * 止まっている。stop() 経由なら既に false だが、`start()` のセットアップ
         * 失敗経路もここを通るため、その取りこぼしをここで塞ぐ(冪等)。 */
        mx68k_rec_audio_set_enabled(false)
        writer = nil
        videoInput = nil
        adaptor = nil
        outputURL = nil
        frameWidth = 0
        frameHeight = 0
        basePTSLock.withLockUnchecked { $0 = nil }
        lastPTS = .invalid
        /* P699 — 黒帯用スクラッチを解放する。`fillBlack()` は videoQueue 上でのみ
         * 呼ばれ、この後片付けも同じ videoQueue 上なので解放後アクセスは起こらない
         * (writer 系プロパティと同じ規約)。 */
        free(blackScratch)
        blackScratch = nil
        lastSourceWidth = 0
        lastSourceHeight = 0
        lastGeom = nil
        audioInput = nil
        audioFormat = nil
        lastAudioPTS = .invalid
    }

    private func log(_ text: String) {
        text.withCString { mx68k_log($0) }
    }
}
