import Foundation
import CoreAudio
import AudioToolbox
import os

class AudioEngine {
    private var audioUnit: AudioUnit?
    private let runningLock = OSAllocatedUnfairLock(initialState: false)

    /// 0.0–1.0. Read on the CoreAudio I/O thread; a torn read is a benign 1-buffer gain glitch.
    var volume: Float = 1.0
    /// false = mute (the ring is still drained to avoid backpressure).
    var enabled: Bool = true

    /// P629 (D-64) — 診断専用ロガー。`initialize()` が投げる各 `OSStatus` と、
    /// `AudioUnitInitialize()` 後に実際に確定したストリームフォーマットを unified log
    /// へ記録する(Console.app / `log show --predicate 'subsystem == "com.mx68k.emulator"'`
    /// で確認できる)。**ログ出力のみで挙動は一切変えない** — AudioUnit の実消費ペースが
    /// 要求サンプルレートに関わらず約 44100Hz 相当へ固着する現象(P629 Code
    /// Investigation 2回目、`[runtime log]`)の「なぜ」を将来判別するための備え。
    /// 従来は OSStatus を一切チェックしていなかったため、要求が実際に受理されたのかを
    /// 原理的に判別できなかった。
    private static let log = Logger(subsystem: "com.mx68k.emulator", category: "AudioEngine")

    func initialize(sampleRate: Double = 44100.0, bufferFrames: UInt32 = 512) {
        var desc = AudioComponentDescription(
            componentType: kAudioUnitType_Output,
            componentSubType: kAudioUnitSubType_DefaultOutput,
            componentManufacturer: kAudioUnitManufacturer_Apple,
            componentFlags: 0,
            componentFlagsMask: 0
        )

        guard let component = AudioComponentFindNext(nil, &desc) else { return }

        var au: AudioUnit?
        let status = AudioComponentInstanceNew(component, &au)
        guard status == noErr, let audioUnit = au else {
            // P629: 従来は無言 return。失敗理由が残らないため記録のみ追加(挙動不変)。
            AudioEngine.log.error(
                "[P629] AudioComponentInstanceNew failed: OSStatus=\(status, privacy: .public)")
            return
        }
        self.audioUnit = audioUnit

        var input = AURenderCallbackStruct(
            inputProc: audioCallback,
            inputProcRefCon: Unmanaged.passUnretained(self).toOpaque()
        )

        let cbStatus = AudioUnitSetProperty(
            audioUnit,
            kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input,
            0,
            &input,
            UInt32(MemoryLayout<AURenderCallbackStruct>.size)
        )
        if cbStatus != noErr {
            AudioEngine.log.error(
                "[P629] AudioUnitSetProperty(SetRenderCallback) failed: OSStatus=\(cbStatus, privacy: .public)")
        }

        var streamFormat = AudioStreamBasicDescription(
            mSampleRate: sampleRate,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: 2,
            mBitsPerChannel: 16,
            mReserved: 0
        )

        let fmtStatus = AudioUnitSetProperty(
            audioUnit,
            kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input,
            0,
            &streamFormat,
            UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        )
        if fmtStatus != noErr {
            AudioEngine.log.error(
                "[P629] AudioUnitSetProperty(StreamFormat) failed: OSStatus=\(fmtStatus, privacy: .public) requested=\(sampleRate, privacy: .public)Hz")
        }

        let initStatus = AudioUnitInitialize(audioUnit)
        if initStatus != noErr {
            // ★挙動不変のため early return はしない(従来も戻り値を見ずに続行していた)。
            AudioEngine.log.error(
                "[P629] AudioUnitInitialize failed: OSStatus=\(initStatus, privacy: .public)")
        } else {
            // P629: 初期化成功後に実際に確定したフォーマットを読み戻す。要求値と食い違って
            // いれば、AudioUnit が本当に要求レートを拒否しているのか、それとも別の理由で
            // 見かけ上 44100 相当になっているだけなのかを将来判別できる。
            var readback = AudioStreamBasicDescription()
            var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
            let getStatus = AudioUnitGetProperty(
                audioUnit,
                kAudioUnitProperty_StreamFormat,
                kAudioUnitScope_Input,
                0,
                &readback,
                &size
            )
            if getStatus != noErr {
                AudioEngine.log.error(
                    "[P629] AudioUnitGetProperty(StreamFormat) failed: OSStatus=\(getStatus, privacy: .public)")
            } else if readback.mSampleRate != sampleRate {
                AudioEngine.log.error(
                    "[P629] StreamFormat mismatch: requested=\(sampleRate, privacy: .public)Hz actual=\(readback.mSampleRate, privacy: .public)Hz bytesPerFrame=\(readback.mBytesPerFrame, privacy: .public) channels=\(readback.mChannelsPerFrame, privacy: .public) bitsPerChannel=\(readback.mBitsPerChannel, privacy: .public)")
            } else {
                AudioEngine.log.info(
                    "[P629] StreamFormat confirmed: \(readback.mSampleRate, privacy: .public)Hz bytesPerFrame=\(readback.mBytesPerFrame, privacy: .public) channels=\(readback.mChannelsPerFrame, privacy: .public)")
            }
        }
    }

    func start() {
        guard let audioUnit = audioUnit else { return }
        let wasRunning = runningLock.withLock { state in
            let old = state
            state = true
            return old
        }
        guard !wasRunning else { return }
        AudioOutputUnitStart(audioUnit)
    }

    func stop() {
        guard let audioUnit = audioUnit else { return }
        let wasRunning = runningLock.withLock { state in
            let old = state
            state = false
            return old
        }
        guard wasRunning else { return }
        AudioOutputUnitStop(audioUnit)
    }

    func teardown() {
        stop()
        if let audioUnit = audioUnit {
            AudioUnitUninitialize(audioUnit)
            AudioComponentInstanceDispose(audioUnit)
            self.audioUnit = nil
        }
    }
}

private func audioCallback(
    inRefCon: UnsafeMutableRawPointer,
    _: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
    _: UnsafePointer<AudioTimeStamp>,
    _: UInt32,
    _: UInt32,
    ioData: UnsafeMutablePointer<AudioBufferList>?
) -> OSStatus {
    let engine = Unmanaged<AudioEngine>.fromOpaque(inRefCon).takeUnretainedValue()
    guard let buffers = ioData else { return noErr }
    let buffer = buffers.pointee.mBuffers
    guard let data = buffer.mData else { return noErr }
    let frames = Int(buffer.mDataByteSize) / 4
    let ptr = data.assumingMemoryBound(to: Int16.self)
    mx68k_audio_read(ptr, Int32(frames))
    let n = frames * 2   // stereo: 2 Int16 per frame
    if !engine.enabled {
        for i in 0..<n { ptr[i] = 0 }
    } else {
        let vol = engine.volume
        if vol < 0.999 {
            for i in 0..<n { ptr[i] = Int16(Float(ptr[i]) * vol) }
        }
    }

    /* P698 — 動画録画への音声同梱。★volume/mute 適用「後」のバッファを渡すので、
     * 録画される音 = 実際に耳に聞こえる音(ミュート中は無音が録画される)。
     *
     * ここは CoreAudio 実時間 I/O スレッドであり、録画中かどうかの判定を Swift 側で
     * 持つと(ロック取得なり別フラグの読み取りなり)実時間スレッドへ余計な同期を
     * 持ち込むことになる。判定は mx68k_rec_audio_write() 内部の atomic load 1 回に
     * 完全に閉じ込め、ここからは**無条件に呼ぶ** —— 非録画時のコストはその
     * atomic load 1 回だけ(P693 MIDI の教訓「実時間スレッドはロック不可」を
     * 録画経路にも厳格適用)。 */
    mx68k_rec_audio_write(ptr, Int32(frames))
    return noErr
}
