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
