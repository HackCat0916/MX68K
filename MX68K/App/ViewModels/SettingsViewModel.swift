import Foundation
import Combine

class SettingsViewModel: ObservableObject {
    @Published var iplromPath: String = ""
    @Published var cgromPath: String = ""
    @Published var iplrom30Path: String = ""
    @Published var scsiExtRomPath: String = ""
    @Published var scsiInRomPath: String = ""
    @Published var scsiMode: String = "none"   // P274: SCSI 利用方法("none"/"internal"/"external")
    @Published var machineType: String = "SCSI"   // P221b: 新 2 値の既定 = SCSI 搭載機
    @Published var memoryMB: Int = 2
    @Published var clockMHz: Int = 16
    @Published var fpuEnabled: Bool = false
    @Published var fpuModel: String = "68882"   // P898: "68881" / "68882"
    // P872: CPUモデル("68000" / "EC030")。config.json では "68000" のときキーを書かない(nil)
    @Published var cpuModel: String = "68000"
    // P887/P889: ハイメモリ(なし / TS-6BE16相当 / 060turbo相当)。排他選択なので1つのenumで持つ。
    // config.json へは X68030 のときだけ該当キーを書く
    enum HighMemorySelection: Hashable {
        case none
        case ts6be16
        case local060(Int)   // 16/32/64/128/256/384/512/768
    }
    @Published var highMemorySelection: HighMemorySelection = .none
    @Published var audioEnabled: Bool = true
    @Published var audioVolume: Double = 1.0
    @Published var audioSampleRate: Int = 44100
    // P512: チップ別音量(Core の ADPCM_SetVolume / OPM_SetVolume と同じ 0-16 スケール)
    @Published var adpcmVolume: Int = 15
    @Published var opmVolume: Int = 16
    @Published var mercuryUnit: Bool = false
    @Published var midiEnabled: Bool = false   // P488: MIDI ボード(CZ-6BM1)
    @Published var externalFDDUnit: Bool = false   // P686 (D-70): 外付け FDD ユニット
    // P490 (MIDI Stage 2)
    @Published var midiResetOnInit: Bool = true
    @Published var midiResetType: Int = 0      // 0=LA, 1=GM, 2=GS, 3=XG
    @Published var midiDelayMs: Int = 0
    @Published var midiOutDeviceIndex: Int = 0
    @Published var midiInDeviceIndex: Int = 0
    // P826: MIDI 出力先。0 = 外部 CoreMIDI, 1 = 内蔵 MT-32(P825), 2 = 内蔵 SC-55(P826)
    @Published var midiOutputDestination: Int = 0
    // P825: 内蔵 MT-32 の ROM
    @Published var mt32ControlRomPath: String = ""
    @Published var mt32PcmRomPath: String = ""
    @Published var mt32PartialCount: Int = 32   // P827: 最大パーシャル数(既定 32 = 実機準拠)
    // P826: 内蔵 SC-55 の ROM
    @Published var sc55Rom1Path: String = ""
    @Published var sc55Rom2Path: String = ""
    @Published var sc55WaveRom1Path: String = ""
    @Published var sc55WaveRom2Path: String = ""
    @Published var sc55WaveRom3Path: String = ""
    @Published var sram64kEnabled: Bool = false   // P493: 内蔵 SRAM 64KB 化(改造相当)
    // P642: Windrv(Mac フォルダのホスト共有)
    @Published var windrvEnabled: Bool = false
    @Published var windrvHostPath: String = ""
    // P647: 書込み許可。windrvEnabled とは独立した第 2 のトグルで既定 OFF。
    @Published var windrvWriteEnabled: Bool = false
    // P555: ターボ ON 時に使う目標倍率(2〜5)。ターボ ON/OFF 自体は永続化しない。
    @Published var turboTargetMultiplier: Int = 3
    // P557: FD アクセス高速化(XM6「フロッピーディスク高速化」相当)。既定 OFF。
    @Published var fdFastAccess: Bool = false

    /// P872: 設定画面の「機種」Picker 用の合成値("SASI" / "SCSI" / "X68030")。
    /// 保存上は machineType(SASI/SCSI)と cpuModel の組合せで、X68030 = SCSI + EC030。
    var machineChoice: String {
        get { cpuModel == "EC030" ? "X68030" : machineType }
        set {
            if newValue == "X68030" {
                machineType = "SCSI"
                cpuModel = "EC030"
            } else {
                machineType = newValue
                cpuModel = "68000"
            }
        }
    }

    func apply(to config: inout EmulatorConfig) {
        config.bios.iplromPath = iplromPath
        config.bios.cgromPath = cgromPath
        config.bios.iplrom30Path = iplrom30Path
        config.bios.scsiExtRomPath = scsiExtRomPath
        config.bios.scsiInRomPath = scsiInRomPath
        config.extensions.scsiMode = scsiMode
        config.hardware.machineType = machineType
        config.hardware.memoryMB = memoryMB
        config.hardware.clockMHz = clockMHz
        config.hardware.fpuEnabled = fpuEnabled
        config.hardware.fpuModel = (fpuModel == "68881") ? 68881 : 68882   // P898
        config.hardware.cpuModel = (cpuModel == "EC030") ? "EC030" : nil   // P872: 68000 はキーを書かない
        // P887/P889: 68000系機種ではキーを書かない
        let isX68030 = (machineChoice == "X68030")
        switch highMemorySelection {
        case .none:
            config.hardware.highMemoryMB = nil
            config.hardware.highMemory060MB = nil
        case .ts6be16:
            config.hardware.highMemoryMB = isX68030 ? 16 : nil
            config.hardware.highMemory060MB = nil
        case .local060(let mb):
            config.hardware.highMemoryMB = nil
            config.hardware.highMemory060MB = isX68030 ? mb : nil
        }
        config.audio.enabled = audioEnabled
        config.audio.volume = audioVolume
        config.audio.sampleRate = audioSampleRate
        config.audio.adpcmVolume = adpcmVolume   // P512
        config.audio.opmVolume   = opmVolume     // P512
        config.extensions.mercuryUnit = mercuryUnit
        config.extensions.midiEnabled = midiEnabled
        config.extensions.externalFDDUnit = externalFDDUnit   // P686
        config.extensions.midiResetOnInit = midiResetOnInit
        config.extensions.midiResetType = midiResetType
        config.extensions.midiDelayMs = midiDelayMs
        config.extensions.midiOutDeviceIndex = midiOutDeviceIndex
        config.extensions.midiInDeviceIndex = midiInDeviceIndex
        config.extensions.midiOutputDestination = midiOutputDestination         // P826
        config.extensions.mt32ControlRomPath = mt32ControlRomPath               // P825
        config.extensions.mt32PcmRomPath = mt32PcmRomPath                       // P825
        config.extensions.mt32PartialCount = mt32PartialCount                   // P827
        config.extensions.sc55Rom1Path = sc55Rom1Path                           // P826
        config.extensions.sc55Rom2Path = sc55Rom2Path                           // P826
        config.extensions.sc55WaveRom1Path = sc55WaveRom1Path                   // P826
        config.extensions.sc55WaveRom2Path = sc55WaveRom2Path                   // P826
        config.extensions.sc55WaveRom3Path = sc55WaveRom3Path                   // P826
        config.extensions.sram64kEnabled = sram64kEnabled
        config.extensions.windrvEnabled  = windrvEnabled     // P642
        config.extensions.windrvHostPath = windrvHostPath    // P642
        config.extensions.windrvWriteEnabled = windrvWriteEnabled   // P647
        config.performance.turboTargetMultiplier = turboTargetMultiplier   // P555
        config.fdd.fdFastAccess = fdFastAccess   // P557
    }

    func load(from config: EmulatorConfig) {
        iplromPath = config.bios.iplromPath
        cgromPath = config.bios.cgromPath
        iplrom30Path = config.bios.iplrom30Path
        scsiExtRomPath = config.bios.scsiExtRomPath
        scsiInRomPath = config.bios.scsiInRomPath
        scsiMode = config.extensions.scsiMode
        machineType = MachineTypeMigration.normalize(config.hardware.machineType)  // P221b: 旧値を canonical 化
        memoryMB = config.hardware.memoryMB
        clockMHz = config.hardware.clockMHz
        fpuEnabled = config.hardware.fpuEnabled
        fpuModel = (config.hardware.fpuModel == 68881) ? "68881" : "68882"   // P898: nil・不正値は 68882
        cpuModel = (config.hardware.cpuModel == "EC030") ? "EC030" : "68000"   // P872: nil は 68000
        // P887/P889: 不正値はなしへ正規化。両方指定時は TS-6BE16相当を優先(Bridge側と同じ)
        if config.hardware.highMemoryMB == 16 {
            highMemorySelection = .ts6be16
        } else if let mb = config.hardware.highMemory060MB, [16, 32, 64, 128, 256, 384, 512, 768].contains(mb) {
            highMemorySelection = .local060(mb)
        } else {
            highMemorySelection = .none
        }
        audioEnabled = config.audio.enabled
        audioVolume = config.audio.volume
        audioSampleRate = config.audio.sampleRate
        adpcmVolume = config.audio.adpcmVolume   // P512
        opmVolume   = config.audio.opmVolume     // P512
        mercuryUnit = config.extensions.mercuryUnit
        midiEnabled = config.extensions.midiEnabled
        externalFDDUnit = config.extensions.externalFDDUnit   // P686
        midiResetOnInit = config.extensions.midiResetOnInit
        midiResetType = config.extensions.midiResetType
        midiDelayMs = config.extensions.midiDelayMs
        midiOutDeviceIndex = config.extensions.midiOutDeviceIndex
        midiInDeviceIndex = config.extensions.midiInDeviceIndex
        midiOutputDestination = config.extensions.midiOutputDestination         // P826
        mt32ControlRomPath = config.extensions.mt32ControlRomPath               // P825
        mt32PcmRomPath = config.extensions.mt32PcmRomPath                       // P825
        mt32PartialCount = config.extensions.mt32PartialCount                   // P827
        sc55Rom1Path = config.extensions.sc55Rom1Path                           // P826
        sc55Rom2Path = config.extensions.sc55Rom2Path                           // P826
        sc55WaveRom1Path = config.extensions.sc55WaveRom1Path                   // P826
        sc55WaveRom2Path = config.extensions.sc55WaveRom2Path                   // P826
        sc55WaveRom3Path = config.extensions.sc55WaveRom3Path                   // P826
        sram64kEnabled = config.extensions.sram64kEnabled
        windrvEnabled  = config.extensions.windrvEnabled     // P642
        windrvHostPath = config.extensions.windrvHostPath    // P642
        windrvWriteEnabled = config.extensions.windrvWriteEnabled   // P647
        // P556: Picker の tag 集合(2/3/4/5 + ノーウェイト = -1)から外れた値が
        // 入ると Picker が空表示になるため、他の 3 箇所と同じヘルパーを通す。
        // 現状 config 側は decode 時にクランプ済みなので値は変わらない(防御的)。
        turboTargetMultiplier = clampTurboMultiplier(config.performance.turboTargetMultiplier)   // P555 / P556
        fdFastAccess = config.fdd.fdFastAccess   // P557
    }
}
