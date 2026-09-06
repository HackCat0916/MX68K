import Foundation
import Combine
import CoreGraphics
import AppKit
import SwiftUI          // P204: withAnimation
import UniformTypeIdentifiers

/// P204 — 電源ボタンの状態機械。`.on`=通常動作 / `.poweringOff`=電源OFF 遷移中
/// (4.0s の点滅+黒フェード演出中) / `.off`=停止(黒画面維持)。
enum PowerState {
    case on
    case poweringOff
    case off
}

class EmulatorViewModel: ObservableObject {
    @Published var isRunning = false
    @Published var isPaused = false
    /// P555 — ターボ(高速実行)が現在有効か。永続化しない(起動時は常に OFF)。
    @Published var isTurboActive = false
    /// P697 — 動画録画中か。永続化しない(起動時は常に OFF)。
    /// メニュー側は EmulatorViewModel を観測できない(D-10/D-49)ため、
    /// `EmulatorRunState.shared.isRecording` へ軽量ミラーする。
    @Published var isRecording = false
    /// P737 — クイックセーブ/ロード操作の実行中か。⌘⇧S の連打で保存操作が
    /// 多重キューされるのを防ぐ。isRecording と同型で
    /// `EmulatorRunState.shared.isStateOperationPending` へ軽量ミラーする。
    @Published var isStateOperationPending: Bool = false
    /// P555 — ターボ ON 時に使う目標倍率(2〜5)。config.json から復元される。
    /// P556 — `kTurboNoWaitMultiplier`(= -1)のときは「ノーウェイト」
    /// (倍率上限なし、専用スレッドでホスト CPU が許す限り高速実行)を意味する。
    @Published var turboTargetMultiplier = 3
    /// P204 — 電源状態。startEmulation 成功で .on、powerOff()/powerOn() で遷移する。
    @Published var powerState: PowerState = .off
    /// P204 — 黒オーバーレイの不透明度(0=通常, 1=黒)。電源OFF で 1、ON で 0 へアニメ。
    @Published var displayDimming: Double = 0.0
    /// P204 — 電源OFF 押下時刻。ランプ点滅の遅→速フェーズ計算の基準。
    var powerOffStart: Date? = nil
    /// P204 — 電源ON(cold boot)時に呼ばれる。EmulatorView が config/fdd を注入する
    /// (onFDDMounted 等と同型 = ViewModel が ConfigManager に依存しない)。
    var onPowerOn: (() -> Void)?
    @Published var statusText = "Ready"
    /// P189 — 下部ステータスバーに数秒だけ一時表示するメッセージ（スクショ完了等）。
    /// nil のとき非表示。showTransientMessage() で世代管理付きに自動クリアする。
    @Published var transientMessage: String? = nil
    @Published var status = MX68KStatus()
    @Published var crtcStatus = MX68KCRTCStatus()   // P286
    @Published var vcStatus = MX68KVCStatus()        // P286
    @Published var bgStatus = MX68KBGStatus()        // P286
    /// P550 — パレットモニタ(テキスト面256色/グラフィック面256色/コントラスト)。
    /// パネル表示中のみ毎フレーム更新される(EmulatorEngine.paletteVisible)。
    @Published var paletteStatus = MX68KPaletteStatus()   // P550
    /// P692 — ストレージモニタ(SCSI ID0-6 のライブ装着状態)。パネル表示中のみ
    /// 毎フレーム更新される(EmulatorEngine.storageVisible)。SASI 8 ユニットと
    /// busy ランプは既存の `status` を共有するため、この型には含めない。
    @Published var storageStatus = EmulatorStorageStatus()   // P692
    /// P693 — MIDI モニタ(MIDIボード CZ-6BM1 / YM3802 の配線・送受信カウンタ・
    /// レジスタ生値)。パネル表示中のみ毎フレーム更新される
    /// (EmulatorEngine.midiVisible)。デバイス *名* は既存の
    /// midiOutputDeviceNames / midiInputDeviceNames を共有するため含まない。
    @Published var midiStatus = EmulatorMIDIStatus()   // P693
    /// P694 — RTC モニタ(RP5C15 の CLKOUT/ADJ/アラーム/12-24h/うるう年カウンタ +
    /// ホスト時計スナップショット)。パネル表示中のみ毎フレーム更新される
    /// (EmulatorEngine.rtcMonitorVisible)。★日時 7 フィールドと
    /// leapYearEffective はゲスト内部状態ではなくホスト時計由来 —— 詳細は
    /// Bridge/EmulatorBridge.h の MX68K_RTCStatus 定義コメントを参照。
    @Published var rtcMonitorStatus = MX68K_RTCStatus()   // P694
    /// P695 — 入力モニタのキーボードロック LED(`mx68k_get_key_led()` の生バイト)。
    /// パネル表示中のみ毎フレーム更新される(EmulatorEngine.inputMonitorVisible)。
    /// ★負論理: ビットが 0 で点灯、1 で消灯。既定値 0xFF は「全消灯」——
    /// SoftKeyboardState.guestLED の既定値と同じ規約。
    /// bit0=かな bit1=ローマ字 bit2=コード入力 bit3=CAPS bit4=INS bit5=ひらがな
    /// bit6=全角(D7 はコマンド識別ビット)。
    @Published var keyLED: UInt8 = 0xFF   // P695
    @Published var spriteList: [MX68KSpriteEntry] = []   // P326
    @Published var spriteTable: [MX68KSpriteEntry] = []   // P328(全128スロット)
    @Published var spritePatterns: [Int: CGImage] = [:]   // P327
    @Published var previewFrame: CGImage? = nil            // P326
    @Published var textPlaneImage: CGImage? = nil          // P343
    @Published var bgPageImages: [Int: CGImage] = [:]      // P348
    @Published var grpPageImages: [Int: CGImage] = [:]     // P348
    /// P690 — CRTC R20 D11(拡張VRAM配置)経路で復号中か。UI注記の表示に使う。
    @Published var grpPageUsesNemesis: Bool = false        // P690
    @Published var bgspCompositeImage: CGImage? = nil      // P365
    @Published var perfStatus = EmulatorPerfStatus()  // P287
    @Published var speedPercent: Double = 100   // P658: StatusBar常時表示用
    /// P484 — サウンドモニタ(ADPCM section)。パネル表示中のみ 0.1 秒間隔で更新される。
    @Published var adpcmStatus = MX68K_ADPCMStatus()   // P484
    /// P485 — サウンドモニタ(OPM = FM 8ch section)。ADPCM と同じゲートで更新される。
    /// 一時停止時の特別なリセットは行わない(表示内容がレジスタの「設定値」であり、
    /// ADPCM の Peak Level のように「今鳴っている」という誤解を招く要素が無いため)。
    @Published var opmStatus = MX68K_OPMStatus()       // P485
    /// P491 — サウンドモニタ(Mercury Unit の FM 部 = YMF288 section)。
    /// OPM と同じゲートで更新される。Mercury 未装着なら installed=false + 全ゼロ。
    @Published var mercuryOPNStatus = MX68K_MercuryOPNStatus()   // P491
    @Published var mercuryPCMStatus = MX68K_MercuryPCMStatus()   // P635
    /// P549 — サウンドモニタ(CoreAudio バッファのアンダーラン統計 section)。
    /// 他のサウンドモニタ項目と同じゲートで更新される。値はセッション累積で
    /// リセットされない(一時停止中も巻き戻さない)。
    @Published var audioBufferStatus = MX68K_AudioBufferStatus()   // P549
    /// P631 — OPM シンセサイザーパネル(独立ウィンドウ)。サウンドモニタとは
    /// 別の可視性フラグ(engine.opmSynthVisible)で更新されるため、サウンドモニタを
    /// 閉じていても更新され続ける。opmStatus と同じく一時停止時の特別なリセットは
    /// 行わない(表示内容がレジスタの「設定値」であるため)。
    @Published var opmDetailStatus = MX68K_OPMDetailStatus()       // P631
    /// P490 — MIDI 入出力デバイス名一覧。Core 側(midi_darwin.c)が MIDI_Init() 実行時に
    /// 埋め直すため、MIDI 装着 + init/ハードリセット後にのみ中身がある。
    /// 設定画面の .onAppear で refreshMidiDeviceList() を呼んで更新する。
    @Published var midiOutputDeviceNames: [String] = []
    @Published var midiInputDeviceNames: [String] = []
    /// P490 — MIDI ボードが配線済みか(ラッチ後の確定値)。デバイス一覧が空のとき、
    /// 「未配線でハードリセット待ち」なのか「配線済みだが Mac に MIDI 出力先が無い」
    /// のかを案内文言で出し分けるために使う。
    @Published var midiWired: Bool = false
    @Published var fdd0Path: String = ""
    @Published var fdd1Path: String = ""
    /// P684 — FD2/FD3。File メニューからのみ操作でき、ツールバー/ステータスバー/D&D
    /// からは到達できない(ユーザーとの合意によるスコープ)。現時点でこれを購読する
    /// View は無いが、fdd0Path/fdd1Path とロジック上の対称性を保つ(将来 UI を足すときの
    /// 再利用性)ため同じく `@Published` にしてある。
    @Published var fdd2Path: String = ""
    @Published var fdd3Path: String = ""
    /// P554 — そのドライブが zip 展開由来の一時ディレクトリからマウントされている場合の
    /// 展開先。eject 時・別ディスクの上書きマウント時・アプリ起動時スイープの 3 経路で
    /// 確実に破棄する(nil = zip 由来ではない)。
    private var fdd0ArchiveTempDir: URL?
    private var fdd1ArchiveTempDir: URL?
    private var fdd2ArchiveTempDir: URL?   // P684
    private var fdd3ArchiveTempDir: URL?   // P684
    /// P599 / P673b — 一時的な強制書込み禁止でトグルを true へ書き換える前の値を保存し、
    /// restoreWriteProtectIfNeeded() で元に戻すためのバックアップ。対象は
    /// (a) zip マウント(mountFDDFromArchive)と
    /// (b) File メニュー「挿入(書込禁止)…」(browseAndMountFDD の forceWriteProtect == true)
    /// の 2 経路で、どちらもその 1 枚に対する一時的な強制であり config へは永続化しない。
    /// どちらも経ていない通常マウント時は nil のままで、復元処理はスキップする
    /// (誤って書き換えない)。変数名は P599 当時の zip 限定の名残(最小 diff 優先で据置)。
    private var fdd0WriteProtectBeforeZip: Bool?
    private var fdd1WriteProtectBeforeZip: Bool?
    private var fdd2WriteProtectBeforeZip: Bool?   // P684
    private var fdd3WriteProtectBeforeZip: Bool?   // P684
    /// P554 — zip 内にディスクイメージが複数あった場合の選択シート提示状態。
    /// 提示は EmulatorView が担当する(ツールバー経路・D&D 経路で共通)。
    @Published var showArchivePicker = false
    @Published var archivePickerImages: [URL] = []
    /// シート表示中の展開先/対象ドライブ。確定・キャンセルのどちらでも消費する。
    private(set) var archivePickerTempDir: URL?
    private(set) var archivePickerDrive: Int = 0
    /// P271 — FDライトプロテクト(トグル)状態。EmulatorView が起動時に config から
    /// 初期化し、変更時は onFDDWriteProtectChanged で config へ永続化する。
    @Published var fdd0WriteProtect: Bool = false
    @Published var fdd1WriteProtect: Bool = false
    /// P684 — FD2/FD3 用。南京錠トグルの UI は無く、File メニューの
    /// 「挿入(書込禁止)…」経路でのみ true になりうる。
    @Published var fdd2WriteProtect: Bool = false
    @Published var fdd3WriteProtect: Bool = false
    /// P190 — エミュ framebuffer の実寸(px)。ウィンドウスケール(⌘1/2/3)計算の基準。
    /// 毎フレームではなく、サイズ変化時のみメインスレッドで更新する。
    @Published var framebufferSize: CGSize = CGSize(width: 768, height: 512)
    /// P595 (D-55) — Bridge が公開する表示ジオメトリ(単位「標準表示窓=1.0」)。
    /// X68KRenderer が毎フレーム取得し、**値が変化したときだけ**メインスレッドで
    /// 更新する(標準ラスタでは恒等のまま = 更新されない)。マウス写像
    /// (InputManager)とモニタ表示がこれを読む。
    @Published var displayGeometry: DisplayGeometry = .identity
    // P192 — metalViewSize は削除。chrome は WindowScaler が AppKit から同期実測する。
    /// P191 — ハードリセット確認ダイアログ。メニュー(⌘R)/ツールバー双方から立てる。
    /// alert の提示は常時ビュー階層に居る ToolbarView が担当する。
    @Published var showHardResetConfirm = false
    /// P454 — SRAM ゼロクリア確認ダイアログ。設定画面 Hardware タブから立てる。
    @Published var showSRAMClearConfirm = false
    // P579 — 旧 `showSettings`(設定シートの表示フラグ)は削除した。設定画面は
    // 専用ウィンドウ(MX68KApp.swift の `Window("Settings", id: "settings")`)に
    // なり、開閉は openWindow / dismiss が担うためフラグ自体が不要になった。

    let engine = EmulatorEngine()
    let audio = AudioEngine()

    var onFDDMounted: ((Int, String) -> Void)?
    /// P191 — mountFDD と対称。イジェクトを config に永続化するために発火する。
    var onFDDEjected: ((Int) -> Void)?
    /// P271 — FDライトプロテクトのトグル変更を config に永続化するために発火する
    /// (第1引数 = ドライブ 0/1, 第2引数 = 保護 ON/OFF)。
    var onFDDWriteProtectChanged: ((Int, Bool) -> Void)?
    /// P200 — HDD(.hdf)の挿入/取り外しを config に永続化するために発火する
    /// (FDD の onFDDMounted/onFDDEjected と対称。path 空 = 取り外し)。
    var onHDDChanged: ((Int, String) -> Void)?
    /// P241 — 外付け SCSI(CZ-6BS1)イメージの挿入/取り外しを config に永続化するために
    /// 発火する(onHDDChanged と対称。第1引数 = SCSI ID 0..6, path 空 = 取り外し)。
    var onSCSIChanged: ((Int, String) -> Void)?
    /// P668 — SCSI MO(ID5 固定スロット)イメージの装着/取り外しを config に
    /// 永続化するために発火する(path 空 = 取り外し)。
    /// ★onSCSIChanged と違い ID 引数を取らない — MO は単一スロットのため。
    var onMOChanged: ((String) -> Void)?
    /// P676 — SCSI CD-ROM(ID6 固定スロット)イメージの装着/取り外しを config に
    /// 永続化するために発火する(path 空 = 取り外し)。onMOChanged と対称。
    var onCDChanged: ((String) -> Void)?

    /// P53 — weak singleton so `MX68KAppDelegate.applicationWillTerminate`
    /// can locate the running ViewModel to call `stopEmulation()` from
    /// outside the SwiftUI environment. `weak` prevents retain cycles and
    /// extension of lifetime; becomes nil on deinit so the AppDelegate
    /// gracefully no-ops if the VM has been torn down.
    /// See /tmp/mx68k_P53_plan.md §3.4 / Edit E.
    static weak var shared: EmulatorViewModel?

    /// P53 — new explicit init (Code Review v2 N-4 fix). The class had no
    /// prior explicit init (Swift synthesised one); @Published defaults
    /// remain inline. Registers `self` as the weak singleton.
    init() {
        EmulatorViewModel.shared = self
    }

    deinit {
        if EmulatorViewModel.shared === self {
            EmulatorViewModel.shared = nil
        }
    }

    // P684: fdd2/fdd3 を追加。既定値 "" は将来の呼び出し忘れ防止の保険としてのみ
    // 機能する(既存の呼び出し元は全て更新済み)。
    func startEmulation(config: EmulatorConfig, fdd0: String = "", fdd1: String = "",
                        fdd2: String = "", fdd3: String = "") {
        guard !isRunning else { return }
        mx68k_log("[Swift] startEmulation begin")

        // P268: 機種による HDD I/F 排他。SCSI 機(=4)では内蔵 SCSI のみ配線され、
        // 内蔵 SASI(.hdf)・外付 CZ-6BS1(.hds)は到達不可とする。外付ループは
        // pushConfig(=mx68k_set_machine_type)より前に走り、この時点で C 側
        // g_machine_type は既定 0(stale)なので、ゲートは Swift 側の config 値で
        // 判定する必要がある(C バックストップは配線確定後の実行時 insert 用)。
        let isSCSIMachine = machineTypeValue(config.hardware.machineType) == 4

        // P246: 保存済み 外付け SCSI(CZ-6BS1)イメージの復元(ID 0..6)を
        // pushConfig より前に行う。pushConfig 内の mx68k_set_scsi_ext_rom_path
        // は Config.SCSIEXHDImage[0](ID0 マウント有無)を見て SCSIIPL[] を
        // ロードするか判定するため、この書込がロード判定より先に確定していな
        // ければ cold boot 経路で恒久的にスキップされてしまう(回帰の意図が
        // 機能しない)。mx68k_scsi_insert は Config 書込のみで mx68k_init() に
        // 依存しないため、この位置での実行は安全。
        let scsiPaths = [
            config.extensions.scsi0Path, config.extensions.scsi1Path,
            config.extensions.scsi2Path, config.extensions.scsi3Path,
            config.extensions.scsi4Path, config.extensions.scsi5Path,
            config.extensions.scsi6Path,
        ]
        // P274: SASI 機で、かつ設定画面で「外付け」を選択している場合のみ復元。
        // 「装着しない」/「内蔵」選択時は保存済みパスがあっても再マウントしない。
        if !isSCSIMachine && config.extensions.scsiMode == "external" {   // P268: SCSI 機では外付 CZ-6BS1 不可(scope)
            for (id, path) in scsiPaths.enumerated() {
                if !path.isEmpty && FileManager.default.fileExists(atPath: path) {
                    _ = path.withCString { mx68k_scsi_insert(Int32(id), $0) }
                }
            }
        }

        pushConfig(config)

        mx68k_log("[Swift] calling mx68k_init()")
        mx68k_init()
        mx68k_log("[Swift] mx68k_init() returned")

        // P68-g3: mount disks before frame loop so IPLROM sees FDD ready on first boot
        if !fdd0.isEmpty && FileManager.default.fileExists(atPath: fdd0) {
            mountFDD(drive: 0, path: fdd0, writeProtect: config.fdd.fdd0WriteProtect)
        }
        // P191: FDD0 依存を外す。FDD0 が(空)でも FDD1 は独立にマウントする
        // (FDD0 空 + FDD1 にディスク は P191 以降 正当な永続状態)。
        if !fdd1.isEmpty && FileManager.default.fileExists(atPath: fdd1) {
            mountFDD(drive: 1, path: fdd1, writeProtect: config.fdd.fdd1WriteProtect)
        }
        // P684: FD2/FD3 も同様に、config.json の保存済みパスから独立に再マウントする。
        if !fdd2.isEmpty && FileManager.default.fileExists(atPath: fdd2) {
            mountFDD(drive: 2, path: fdd2, writeProtect: config.fdd.fdd2WriteProtect)
        }
        if !fdd3.isEmpty && FileManager.default.fileExists(atPath: fdd3) {
            mountFDD(drive: 3, path: fdd3, writeProtect: config.fdd.fdd3WriteProtect)
        }

        // P200: 保存済み SASI HDD(.hdf)イメージを復元。mx68k_hdd_insert が
        // Config.HDImage[unit*2] にパスを書き hard reset を予約 → 初回フレーム境界で
        // IPL が SASI を再 probe し Human68k から追加ドライブとして見える。
        // P455: 2 本ベタ書き → unit 0..7 のループ(分岐は ExtensionsConfig.hddPath に集約)。
        if !isSCSIMachine {   // P268: SCSI 機では内蔵 SASI 不可(内蔵 SCSI のみ配線)
            for unit in 0..<ExtensionsConfig.hddUnitCount {
                let p = config.extensions.hddPath(unit)
                if !p.isEmpty && FileManager.default.fileExists(atPath: p) {
                    _ = p.withCString { mx68k_hdd_insert(Int32(unit), $0) }
                }
            }
        }

        // P241/P246: 外付け SCSI(CZ-6BS1)イメージの復元(ID 0..6)は
        // startEmulation 冒頭(pushConfig より前)へ移動した。SCSIIPL[] の
        // ロード判定が ID0 マウント有無に依存するため。理由は上部コメント参照。

        mx68k_log("[Swift] audio.initialize()")
        // P627: AudioUnit の出力レートは Bridge 側で実際に適用された値
        // (mx68k_get_audio_sample_rate)に追従させる。受理レートのホワイトリストは
        // Bridge 側 mx68k_set_audio_sample_rate() の 1 箇所のみが真実源であり、
        // Swift 側で値集合を複製すると、どちらか一方の更新漏れで
        // 「Picker では選べるのに出力だけ黙って 44100 へ丸められ、Core チップと
        // AudioUnit のピッチが食い違う」不具合(P626 で修正したのと同型)を招く。
        // この行に到達する時点で pushConfig(:188、mx68k_set_audio_sample_rate 呼出しを含む)
        // と mx68k_init(:191)は既に完了しているため、読み戻し値は確定している。
        let sr = Double(mx68k_get_audio_sample_rate())
        audio.initialize(sampleRate: sr)
        audio.enabled = config.audio.enabled
        audio.volume = Float(config.audio.volume)
        // P512: config に永続化されたチップ別音量を Core へ反映する。ここは Core 初期化
        // (mx68k_init)より後なので、Bridge 側の固定初期化(ADPCM_SetVolume(15) 等)を
        // ユーザー設定値で上書きする形になる。既定値は固定初期化値と一致。
        setAdpcmVolume(config.audio.adpcmVolume)
        setOpmVolume(config.audio.opmVolume)
        mx68k_log("[Swift] audio.start()")
        audio.start()

        engine.onStatusUpdate = { [weak self] newStatus in
            self?.status = newStatus
            self?.reconcileFDDMedia(newStatus)   // P443 (D-7)
        }
        engine.onCRTCStatusUpdate = { [weak self] s in   // P286
            self?.crtcStatus = s
        }
        engine.onVCStatusUpdate = { [weak self] s in
            self?.vcStatus = s
        }
        engine.onBGStatusUpdate = { [weak self] s in
            self?.bgStatus = s
        }
        engine.onPaletteStatusUpdate = { [weak self] s in   // P550
            self?.paletteStatus = s
        }
        engine.onStorageStatusUpdate = { [weak self] s in   // P692
            self?.storageStatus = s
        }
        engine.onMidiStatusUpdate = { [weak self] s in   // P693
            self?.midiStatus = s
        }
        engine.onRTCMonitorStatusUpdate = { [weak self] s in   // P694
            self?.rtcMonitorStatus = s
        }
        engine.onKeyLEDUpdate = { [weak self] led in   // P695
            self?.keyLED = led
        }
        engine.onSpriteListUpdate = { [weak self] entries in   // P326
            self?.spriteList = entries
        }
        engine.onSpriteTableUpdate = { [weak self] table in   // P328
            self?.spriteTable = table
        }
        engine.onSpritePatternsUpdate = { [weak self] patterns in   // P327
            self?.spritePatterns = patterns
        }
        engine.onPreviewFrameUpdate = { [weak self] image in   // P326
            self?.previewFrame = image
        }
        engine.onTextPlaneUpdate = { [weak self] image in   // P343
            self?.textPlaneImage = image
        }
        engine.onBGPageUpdate = { [weak self] images in   // P348
            self?.bgPageImages = images
        }
        engine.onGrpPageUpdate = { [weak self] images, usesNemesis in   // P348/P690
            self?.grpPageImages = images
            self?.grpPageUsesNemesis = usesNemesis
        }
        engine.onADPCMStatusUpdate = { [weak self] s in   // P484
            guard let self else { return }
            var s = s
            if self.isPaused { s.peak_level = 0 }   /* P484d: 一時停止中に届く更新もPeak Levelを0で維持 */
            self.adpcmStatus = s
        }
        engine.onOPMStatusUpdate = { [weak self] s in   // P485
            self?.opmStatus = s
        }
        engine.onMercuryOPNStatusUpdate = { [weak self] s in   // P491
            self?.mercuryOPNStatus = s
        }
        engine.onMercuryPCMStatusUpdate = { [weak self] s in   // P635
            self?.mercuryPCMStatus = s
        }
        engine.onAudioBufferStatusUpdate = { [weak self] s in   // P549
            self?.audioBufferStatus = s
        }
        engine.onOPMDetailStatusUpdate = { [weak self] s in   // P631
            self?.opmDetailStatus = s
        }
        engine.onBGSPCompositeUpdate = { [weak self] image in   // P365
            self?.bgspCompositeImage = image
        }
        engine.onPerfUpdate = { [weak self] newStatus in   // P287
            self?.perfStatus = newStatus
        }
        engine.onSpeedUpdate = { [weak self] pct in   // P658
            self?.speedPercent = pct
        }
        mx68k_log("[Swift] engine.start()")
        engine.start()

        isRunning = true
        EmulatorRunState.shared.isRunning = isRunning        // P546: メニューゲート用の軽量ミラー
        powerState = .on           // P204: 起動成功で電源 ON。
        EmulatorRunState.shared.powerState = powerState      // P546: メニューゲート用の軽量ミラー
        // P204-fix: displayDimming は powerOff()/powerOn() が管理する。ここで 0 に
        // しない — cold boot(powerOn)経路では黒幕を維持したまま最初の数フレームを
        // 描かせ、旧フレームのちらつきを防ぐ(新規起動時は既定値 0.0 で画面は可視)。
        statusText = "Running"
        /* P556: ターボ状態の再適用。startEmulation 冒頭の pushConfig(178 行)も
         * `if isTurboActive { applyTurboState() }` を通るが、その時点ではまだ
         * isRunning == false なので、applyTurboState の isRunning ガード
         * (Core 未 init のまま専用スレッドを走らせない安全策)によりノーウェイトが
         * 起動しない。電源OFF→ON(powerOff は isTurboActive を保持したまま
         * stopEmulation する)で「ターボ ON のはずなのに効かない」状態にならないよう、
         * isRunning が立った後にもう一度反映する。ターボ OFF なら何もしない。 */
        if isTurboActive { applyTurboState() }
        mx68k_log("[Swift] startEmulation end")
    }

    // MARK: - Power (P204)

    /// 電源OFF 押下 — 実機の電源OFF シーケンスを host 側で近似する:
    /// 電源ランプ点滅(遅→速・計 4.0s)+ 画面を 4.0s かけて黒へフェード。
    /// SRAM $ED0029(FD 排出フラグ)が立っていれば両ドライブを排出してから停止。
    /// 音はフェードしない(実機忠実 — 鳴っていた音は stopEmulation まで継続)。
    func powerOff() {
        guard powerState == .on else { return }
        powerState = .poweringOff
        EmulatorRunState.shared.powerState = powerState      // P546: メニューゲート用の軽量ミラー
        powerOffStart = Date()
        // P204: SRAM 有効なうち(stop 前)に FD 排出フラグを読む。(A)実機忠実 =
        // ejectFDD が config パスも空にする(次回 cold boot で復活しない)。
        if mx68k_sram_eject_on_poweroff() {
            ejectFDD(drive: 0)
            ejectFDD(drive: 1)
        }
        withAnimation(.easeIn(duration: 4.0)) {
            displayDimming = 1.0
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 4.0) { [weak self] in
            guard let self = self, self.powerState == .poweringOff else { return }
            self.stopEmulation()
            self.powerState = .off      // 黒幕は維持(displayDimming = 1.0)。
            EmulatorRunState.shared.powerState = self.powerState      // P546: メニューゲート用の軽量ミラー
        }
    }

    /// 電源ON 押下(OFF 状態から)— cold boot(startEmulation 相当)。
    func powerOn() {
        guard powerState == .off else { return }
        onPowerOn?()                                // startEmulation → powerState=.on
        InputManager.shared.requestMouseHoming()    // P204/M2: cold boot はゲストポインタを失う(P196 再発防止)。
        // P204-fix: 黒幕を cold boot 最初の ~0.6s 保持してから淡出する。旧フレーム
        // (電源OFF 直前の画面)の上に新しいブート描画が乗るまで隠す = ちらつき防止。
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak self] in
            withAnimation(.easeOut(duration: 0.5)) { self?.displayDimming = 0.0 }
        }
    }

    /// 電源ボタン — 状態でトグル。遷移中(.poweringOff)は無視。
    func togglePower() {
        switch powerState {
        case .on:  powerOff()
        case .off: powerOn()
        case .poweringOff: break
        }
    }

    /// 起動時 config プッシュ（挙動不変で startEmulation から抽出・applySettings と共有）。
    /// mx68k_set_* グローバルは mx68k_init()/reset のみが消費する。
    private func pushConfig(_ config: EmulatorConfig) {
        config.bios.iplromPath.withCString { ipl in
            config.bios.cgromPath.withCString { cg in
                mx68k_set_bios_path(ipl, cg)
            }
        }
        if !config.bios.iplrom30Path.isEmpty {
            config.bios.iplrom30Path.withCString { path in
                mx68k_set_bios_path_030(path)
            }
        }
        // P241 Stage A: 外付け SCSI(CZ-6BS1)IPL ROM。任意 — 空なら外付け SCSI 起動のみ無効。
        if !config.bios.scsiExtRomPath.isEmpty {
            config.bios.scsiExtRomPath.withCString { path in
                mx68k_set_scsi_ext_rom_path(path)
            }
        }

        // P251 Stage 2c: 内蔵 SCSI IPL ROM(P244 で追加されたが pushConfig からの
        // 呼出しが漏れていた空洞を解消)。二重ゲートの rom_loaded 条件が正しく
        // 評価されるよう、ROM ロードを確定させる。
        if !config.bios.scsiInRomPath.isEmpty {
            config.bios.scsiInRomPath.withCString { path in
                mx68k_set_scsi_in_rom_path(path)
            }
        }

        // P274 / P450 (D-30 の後半): 内蔵 SCSI HD ディスクイメージ(ID0-6、共有
        // scsi0Path〜scsi6Path)。二重ゲート成立時に scsi_real_install_construct が
        // Reset()->Construct() 前へ流す。
        // ★従来は `scsiMode == "internal"` で丸ごと skip し、さらに
        // `where !path.isEmpty` で空パスを除外していたため、Bridge シャドウ
        // s_scsi_in_disk_path[] を **空に戻す経路が存在しなかった**。
        // 結果、P450 (E) で毎リセット再構築するようにしても「取り外し」と
        // 「scsiMode を none/external へ変更」は反映されない。
        // 対策は P447 の SASI シャドウと同じ — 「push しない」ではなく
        // 「空文字を明示的に push する」。ID0-6 の全 7 本を必ず push する。
        //
        // scsiMode ゲート自体は Swift 側に残さざるを得ない(Bridge は
        // scsiMode を知らない)。P447 の「機種判定は Bridge が一手に担う」方針とは
        // 矛盾しない — 欠陥はゲートの存在ではなく、ゲートが *クリア* ではなく
        // *スキップ* だった点にある。
        //
        // 実在チェックは下の hdd0Shadow と同じ理由で掛ける(config に残って
        // いるが実体が消えたパスをシャドウへ入れない)。
        let useInternalSCSI = (config.extensions.scsiMode == "internal")
        let internalPaths = [
            config.extensions.scsi0Path, config.extensions.scsi1Path,
            config.extensions.scsi2Path, config.extensions.scsi3Path,
            config.extensions.scsi4Path, config.extensions.scsi5Path,
            config.extensions.scsi6Path,
        ]
        for (id, path) in internalPaths.enumerated() {
            let shadow = (useInternalSCSI && !path.isEmpty
                          && FileManager.default.fileExists(atPath: path)) ? path : ""
            shadow.withCString { mx68k_set_scsi_in_disk_path(Int32(id), $0) }
        }

        // P668: MO(ID5 固定スロット)のパスを Bridge シャドウへ push する。
        // ★scsiMode ゲートは付けない。
        //   すぐ上の HD パスループが useInternalSCSI ゲートを持つ理由は、
        //   scsi0Path〜scsi6Path が内蔵/外付けで **共有** フィールドであり、
        //   どちらの経路へ流すかを Swift 側でしか決められないため — 配列共有の
        //   曖昧性の解消であって、一般原則としての機種ゲートではない。
        //   MO は専用の moPath フィールドを持ち共有されないため、この曖昧性が
        //   そもそも存在せず、ゲートは不要である。SPC を構築するか否かの判定は
        //   Bridge 側(P510 の s_scsi_mode ゲート + SCSI::Construct() の
        //   scsi.type==0 早期 return)が一手に担う。
        // ★空文字も必ず push する(P450 の教訓: skip ではなく明示的に clear)。
        // ★実在チェックだけは掛ける(config に残っているが実体が消えたパスを
        //   シャドウへ入れない — 上の SCSI/SASI ループと同じ理由)。
        let moShadow = (!config.extensions.moPath.isEmpty
                        && FileManager.default.fileExists(atPath: config.extensions.moPath))
                     ? config.extensions.moPath : ""
        moShadow.withCString { mx68k_set_mo_path($0) }

        // P676: CD-ROM(ID6 固定スロット)のパスを Bridge シャドウへ push する。
        // 構造・理由づけはすぐ上の MO と完全に同一(専用フィールドなので
        // scsiMode ゲートは不要、空文字も必ず push、実在チェックのみ掛ける)。
        let cdShadow = (!config.extensions.cdPath.isEmpty
                        && FileManager.default.fileExists(atPath: config.extensions.cdPath))
                     ? config.extensions.cdPath : ""
        cdShadow.withCString { mx68k_set_cd_path($0) }

        // P447 (C2'): 内蔵 SASI HDD(.hdf)パスを Bridge のシャドウへ push する。
        // 上の内蔵 SCSI ディスクパスのループと対称。機種切替(SCSI→SASI)後の
        // ハードリセットで Config.HDImage[] を復元するための「保存値」を、
        // まだ一度も mx68k_hdd_insert が呼ばれていない起動直後でも確定させる。
        // ★機種ゲートは付けない — 機種判定は Bridge 側(mx68k_reset_hard の
        // 再適用ロジック)が一手に担う。ゲートをここに置くと、pushConfig だけが
        // 機種を知っていて Bridge が知らないという非対称が再発する。
        // 空文字も渡す(= そのユニットは未装着、とシャドウへ明示的に伝える)。
        //
        // 実在チェックだけは掛ける — startEmulation:155-160 の復元ループが従来から
        // fileExists で絞っており、それを外すと「config に残っているが実体が消えた
        // パス」が HDImage[] へ復元されてしまう。sasi.c:426 のデバイス在席判定は
        // パス文字列が非空かどうかだけを見るため(File_Open は後段で失敗する)、
        // 存在しないドライブが「在席するが全リードが失敗する」状態になる。
        // これは機種ゲートではないので「機種判定は Bridge が一手に担う」方針と
        // 矛盾しない。
        //
        // P455: 2 本ベタ書き → unit 0..7 のループ。上記 3 点(機種ゲート無し・
        // 空文字も必ず push・fileExists のみ)は 8 本すべてで維持する。
        for unit in 0..<ExtensionsConfig.hddUnitCount {
            let p = config.extensions.hddPath(unit)
            let shadow = FileManager.default.fileExists(atPath: p) ? p : ""
            shadow.withCString { mx68k_set_hdd_path(Int32(unit), $0) }
        }

        mx68k_set_machine_type(Int32(machineTypeValue(config.hardware.machineType)))
        // P450: メモリスイッチ自動更新(XM6「メモリスイッチ自動更新」相当)。
        mx68k_set_memsw_auto_update(config.extensions.memSwitchAutoUpdate)
        // P450: XM6 Memory::GetMemType()==SCSIExt 相当の「ボードが装着されているか」。
        // ★ディスク在席や ROM ロード有無ではなく構成そのものを渡すこと。
        // Bridge 側の scsi_present 判定(mx68k_reset_hard)がこれを OR の第2項に使う。
        mx68k_set_scsi_ext_board_installed(config.extensions.scsiMode == "external")
        mx68k_set_memory_size(Int32(config.hardware.memoryMB))
        mx68k_set_clock(Int32(config.hardware.clockMHz))
        mx68k_set_fpu_enabled(config.hardware.fpuEnabled)
        // P483: Mercury Unit(MK-MU1 / $ECC000)の装着設定。値は Bridge 側で
        // 「設定値」として保持され、配線はハードリセット（⌘R）で確定する。
        mx68k_set_mercury_enabled(config.extensions.mercuryUnit)
        // P686 (D-70): 外付け FDD ユニット。設定値のみ push し、配線は
        // init / ハードリセットで確定する(Mercury と同型)。
        mx68k_set_ext_fdd_enabled(config.extensions.externalFDDUnit)
        // P488: MIDI ボード(CZ-6BM1 / $EAE000)の装着設定。Mercury と同じく
        // 値は Bridge 側で「設定値」として保持され、配線はハードリセット（⌘R）で確定する。
        mx68k_set_midi_enabled(config.extensions.midiEnabled)
        // P490: MIDI Stage 2。リセット送信・音源種別・デバイス選択は Bridge 側で
        // 「設定値」として保持され、配線はハードリセットで確定する。送信遅延のみ
        // フレームループが毎回読むため即時反映される。
        mx68k_set_midi_reset_enabled(config.extensions.midiResetOnInit)
        mx68k_set_midi_reset_type(Int32(config.extensions.midiResetType))
        mx68k_set_midi_delay_ms(Int32(config.extensions.midiDelayMs))
        mx68k_midi_set_output_device(Int32(config.extensions.midiOutDeviceIndex))
        mx68k_midi_set_input_device(Int32(config.extensions.midiInDeviceIndex))
        // P493: 内蔵 SRAM 64KB 化。Mercury/MIDI と同じく値は Bridge 側で「設定値」として
        // 保持され、配線はハードリセット（⌘R）で確定する。
        mx68k_set_sram_64k_enabled(config.extensions.sram64kEnabled)
        // P642: Windrv(Mac フォルダのホスト共有)。Mercury/MIDI/SRAM64K と同じく
        // 値は Bridge 側で「設定値」として保持され、配線($E9F000/$E9F001 の応答開始と
        // マウントルートの realpath 正規化)はハードリセット(⌘R)で確定する。
        // ★パスは常に push する(空文字も含む)—— 空を push しないと、利用者が
        //   フォルダ選択を解除しても Bridge 側に前回のパスが残り続ける。
        mx68k_set_windrv_enabled(config.extensions.windrvEnabled)
        config.extensions.windrvHostPath.withCString { mx68k_set_windrv_host_path($0) }
        // P647: 書込み許可は共有トグルとは独立した第 2 の設定値。配線
        // (g_windrv_write_wired)は windrv_init() が
        // `installed && write_enabled` としてラッチする。
        mx68k_set_windrv_write_enabled(config.extensions.windrvWriteEnabled)
        // P626 (D-62): 音声サンプルレート。従来この設定値は Swift 側の AudioUnit 出力
        // レート(startEmulation の audio.initialize)にしか渡っておらず、Core 側の
        // ADPCM / OPM / Mercury は常に 44100 固定で初期化されていたため、22050Hz を
        // 選ぶと生成量が消費量の 2 倍になり常時バースト音切れになっていた。
        // Mercury / MIDI / SRAM64K と同じく値は Bridge 側で「設定値」として保持され、
        // チップへの反映はハードリセット（⌘R）/ 起動時の mx68k_init で確定する。
        mx68k_set_audio_sample_rate(Int32(config.audio.sampleRate))
        // P555: ターボの目標倍率を config から復元する。ターボ ON/OFF 自体は
        // 永続化しない(起動時は常に OFF)ため、ここで反映されるのは「次に ON に
        // したときの倍率」。既に ON の状態で設定を適用した場合だけ即時反映する。
        // P556: クランプは共通ヘルパーへ集約(ノーウェイトのセンチネル -1 を許可する)。
        turboTargetMultiplier = clampTurboMultiplier(config.performance.turboTargetMultiplier)
        if isTurboActive { applyTurboState() }
        // P557: FD アクセス高速化。クロック速度と同じく Bridge 側が毎回読み直す
        // 単純グローバルなので即時反映(リセット不要)。既定 false = 従来挙動。
        mx68k_set_fd_fast_access(config.fdd.fdFastAccess ? 1 : 0)
    }

    /// P490 — Core が保持する MIDI 入出力デバイス名一覧を Swift 側へ取り込む。
    /// 一覧は MIDI_Init()(init / ハードリセット)でしか更新されないため、
    /// 設定画面を開いたタイミングで読み直せば足りる。未装着時は count=0 で空になる。
    func refreshMidiDeviceList() {
        midiWired = mx68k_midi_is_wired()
        func names(count: Int32, fetch: (Int32, UnsafeMutablePointer<CChar>, Int32) -> Bool) -> [String] {
            var result: [String] = []
            var buf = [CChar](repeating: 0, count: 256)
            for i in 0..<count {
                let ok = buf.withUnsafeMutableBufferPointer { p -> Bool in
                    fetch(i, p.baseAddress!, Int32(p.count))
                }
                guard ok else { break }
                result.append(String(cString: buf))
            }
            return result
        }
        midiOutputDeviceNames = names(count: mx68k_midi_get_output_device_count()) {
            mx68k_midi_get_output_device_name($0, $1, $2)
        }
        midiInputDeviceNames = names(count: mx68k_midi_get_input_device_count()) {
            mx68k_midi_get_input_device_name($0, $1, $2)
        }
    }

    /// 設定変更を適用（値は pushConfig で Bridge へ反映）。
    func applySettings(_ config: EmulatorConfig) {
        guard isRunning else { return }   // 初回（エミュ未起動）は config 保存のみ。startEmulation が起動時に適用。
        pushConfig(config)
        // P238: 「適用」時の強制ハードリセットを廃止。クロックは毎フレーム
        // 反映されるので即時、機種/メモリ/FPU/BIOS は次回の手動リセット（⌘R）で
        // 反映される（各タブのキャプションで告知済み）。homing はリセット時のみ
        // 意味があるため、リセットしないこの経路では呼ばない。
    }

    // P221b: 新 2 値(SASI/SCSI)を C 側 enum 値へ。旧 6 値の config.json も
    // normalize してから解決する(write-back 前の初回起動でも正しく届く)。
    // mx68k_set_machine_type は現状 get_status 報告のみ(cosmetic)だが、旧
    // X68000=0 / X68000_XVI=4 の意味を保持し get_status の連続性を保つ。
    private func machineTypeValue(_ type: String) -> Int {
        switch MachineTypeMigration.normalize(type) {
        case "SASI": return 0
        case "SCSI": return 4
        default: return 4
        }
    }

    func stopEmulation() {
        // P53 — Swift-layer idempotency (Plan v2 §3.5). Combined with the
        // C-side `s_p53_shutdown_done` guard, double-call from .onDisappear
        // + applicationWillTerminate is safe in any order.
        guard isRunning else { return }
        /* P697: 録画中に電源OFF/アプリ終了が来た場合、ここでファイルを閉じておかないと
         * moov atom が書かれず再生できない mp4 が残る。終端処理の完了を(最大 5 秒)
         * 待つのは、直後の mx68k_shutdown()/プロセス終了に追い越されないため。
         * 非録画時は guard で即 return するので既存経路のコストは変わらない。 */
        stopRecording(waitForCompletion: true)
        InputManager.shared.forceReleaseMouseCaptureIfNeeded()   // P237: 停止時にカーソル非表示を引きずらない
        engine.stop()
        audio.stop()
        audio.teardown()
        mx68k_shutdown()
        isRunning = false
        EmulatorRunState.shared.isRunning = isRunning        // P546: メニューゲート用の軽量ミラー
        statusText = "Stopped"
    }

    func setSoundEnabled(_ on: Bool) { audio.enabled = on }
    func setVolume(_ v: Double)      { audio.volume = Float(max(0.0, min(1.0, v))) }
    // P512: チップ別音量。上の setVolume(_:) が Swift 側 AudioEngine のポストミックス
    // ゲインを操作するだけなのに対し、こちらは Bridge 経由で Core の音量ステート
    // (ADPCM_VolumeShift / fmvolume)を直接書き換える別レイヤー。
    func setAdpcmVolume(_ v: Int) { mx68k_set_adpcm_volume(Int32(max(0, min(16, v)))) }
    func setOpmVolume(_ v: Int)   { mx68k_set_opm_volume(Int32(max(0, min(16, v)))) }

    func hardReset() {
        // P628: サンプルレート設定はCore側チップ(ADPCM/OPM/Mercury)だけでなく
        // AudioEngine(ホスト出力)側の再構成も必要——他の「ハードリセットで反映」
        // 設定(機種/メモリ/FPU/BIOS)と異なり、Core内部状態だけでは完結しない。
        // これを怠ると「Core側は新レートで生成・AudioUnitは旧レートのまま消費」
        // という構図になり、生成/消費レート比に応じた再生遅延+リング圧迫による
        // プチノイズが発生する(ユーザーの実地報告、2026-08-18)。
        // g_audio_sample_rate_hz は設定変更時の pushConfig() で既に同期的に
        // 更新済みのため、mx68k_reset_hard() の実際の実行(フレーム境界まで遅延)を
        // 待たずに読み出せる。
        audio.stop()
        audio.teardown()
        audio.initialize(sampleRate: Double(mx68k_get_audio_sample_rate()))
        audio.start()
        mx68k_schedule_hard_reset()   // フレーム境界で安全にリセット(直接 mx68k_reset_hard は run_frame とレース)。
        InputManager.shared.requestMouseHoming()   // P196: reset でゲストポインタ位置が失われるため原点合わせ。
        showTransientMessage(String(localized: "Hard Reset"))
    }

    func softReset() {
        mx68k_schedule_soft_reset()   // フレーム境界で安全にリセット(直接 mx68k_reset_soft は run_frame とレース)。
        InputManager.shared.requestMouseHoming()   // P196: reset でゲストポインタ位置が失われるため原点合わせ。
        showTransientMessage(String(localized: "Soft Reset"))
    }

    /// P454 — バッテリバックアップ SRAM をゼロクリアする(実機のバックアップ電池を
    /// 外す操作に相当)。破壊的操作のため呼出し側で確認ダイアログを経由すること。
    func clearSRAM() {
        mx68k_schedule_sram_clear()   // フレーム境界で安全にクリア(直接 mx68k_sram_clear は run_frame とレース)。
        showTransientMessage(String(localized: "SRAM Cleared"))
    }

    func nmi() {
        mx68k_nmi()
        showTransientMessage(String(localized: "Interrupt"))
    }

    /// P189 — 一時メッセージ機構。連続呼び出し時、後発メッセージが先発タイマーで
    /// 早期に消されないよう世代カウンタで管理する（クロージャが捕捉した世代と
    /// 現在の世代が一致するときだけ nil にクリア）。
    private var transientMessageGeneration = 0
    func showTransientMessage(_ text: String) {
        transientMessageGeneration += 1
        let generation = transientMessageGeneration
        transientMessage = text
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in
            guard let self = self, self.transientMessageGeneration == generation else { return }
            self.transientMessage = nil
        }
    }

    func takeScreenshot() {
        if let url = ScreenshotService.capture() {
            showTransientMessage(String(localized: "Screenshot saved: \(url.lastPathComponent)"))
        } else {
            showTransientMessage(String(localized: "Screenshot failed"))
        }
    }

    // MARK: - P697: 動画録画(映像のみ MVP)

    /// P697 — 録画中の出力先。停止時のトースト(ファイル名表示)に使う。
    private var recordingURL: URL?

    /// P697 — 録画の保存先ディレクトリ。設定値(UserDefaults "videoRecordingDirectory")が
    /// 空/無ければ ~/Movies/MX68K(ScreenshotService.screenshotDirectory() と同型)。
    func videoRecordingDirectory() -> URL {
        if let p = UserDefaults.standard.string(forKey: "videoRecordingDirectory"), !p.isEmpty {
            return URL(fileURLWithPath: p, isDirectory: true)
        }
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Movies/MX68K", isDirectory: true)
    }

    /// P697 — メニュー(Start/Stop Recording)からのワンタッチ切替。
    func toggleRecording() {
        if isRecording {
            stopRecording()
        } else {
            startRecording()
        }
    }

    /// P700 — 録画中はウィンドウタイトルバーへ常時表示する。トースト
    /// (`showTransientMessage`)は一定時間で消えるため、録画中かどうかを
    /// いつでも一目で確認できる常設の手がかりを用意する。
    /// アプリ名はハードコードせず `CFBundleName` から取得する。
    private func updateWindowTitleForRecordingState() {
        let appName = Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String ?? "MX68K"
        EmulatorMTKView.hostWindow?.title = isRecording
            ? "\(appName) (\(String(localized: "Recording")))"
            : appName
    }

    /// P697 — 録画開始。ターボ/ノーウェイト中は開始しない(タイムスタンプ同期の
    /// 問題を設計で回避する — 逆向きのガードは toggleTurbo() 側にある)。
    func startRecording() {
        guard !isRecording else { return }
        guard isRunning, powerState == .on else {
            showTransientMessage(String(localized: "Recording failed to start"))
            return
        }
        guard !isTurboActive else {
            showTransientMessage(String(localized:
                "Cannot start recording while Turbo is on. Turn Turbo off first."))
            return
        }
        let dir = videoRecordingDirectory()
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let fmt = DateFormatter()
        fmt.locale = Locale(identifier: "en_US_POSIX")   // ★数値固定 format 保証(ScreenshotService と同型)
        fmt.dateFormat = "yyyyMMdd_HHmmss"
        let url = dir.appendingPathComponent("mx68k_\(fmt.string(from: Date())).mp4")
        do {
            try engine.startRecording(outputURL: url)
            recordingURL = url
            isRecording = true
            EmulatorRunState.shared.isRecording = true   // P546 と同型のメニューゲート用ミラー
            updateWindowTitleForRecordingState()         // P700
            showTransientMessage(String(localized: "Recording started: \(url.lastPathComponent)"))
        } catch {
            let msg = "[P697-REC] start failed: \(error.localizedDescription)"
            msg.withCString { mx68k_log($0) }
            showTransientMessage(String(localized: "Recording failed to start"))
        }
    }

    /// P697 — 録画停止。ファイルの終端処理は非同期(AVAssetWriter)なので、
    /// 完了トーストはコールバックでメインスレッドへ戻してから出す。
    ///
    /// - Parameter waitForCompletion: 真のとき、終端処理の完了を最大 5 秒だけ
    ///   同期的に待つ。アプリ終了/電源OFF 経路(stopEmulation)専用 —— 待たずに
    ///   プロセスが終わると mp4 の moov atom が書かれず再生不能なファイルが残る。
    func stopRecording(waitForCompletion: Bool = false) {
        guard isRecording else { return }
        isRecording = false
        EmulatorRunState.shared.isRecording = false
        updateWindowTitleForRecordingState()            // P700
        let url = recordingURL
        recordingURL = nil
        let semaphore = waitForCompletion ? DispatchSemaphore(value: 0) : nil
        engine.stopRecording { [weak self] success in
            // ★main.async は非同期(ここでブロックしない)ので、下の semaphore.wait()
            //   がメインスレッドを止めていてもデッドロックしない。
            DispatchQueue.main.async {
                guard let self = self else { return }
                if success, let url = url {
                    self.showTransientMessage(String(localized: "Recording saved: \(url.lastPathComponent)"))
                } else {
                    self.showTransientMessage(String(localized: "Recording failed"))
                }
            }
            semaphore?.signal()
        }
        if let semaphore = semaphore {
            _ = semaphore.wait(timeout: .now() + 5.0)
        }
    }

    // MARK: - State Save / Load (Phase 2 #4)

    /// ステート保存先ディレクトリ ~/Documents/MX68K/states/(無ければ作成)。
    private func statesDirectory() -> URL {
        let dir = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Documents/MX68K/states", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    private static let mxStateType: UTType = UTType(filenameExtension: "mxstate") ?? .data

    /// ⌘⌥S — NSSavePanel で保存先を選び、エミュ状態をスナップショット保存する。
    func saveState() {
        // P481 (D-42) — 手動一時停止(⌘P)中は run_frame が回らず、キューした保存が
        // 永久に実行されない。パネルを開く前に事前拒否する(2秒のタイムアウトを
        // 待たせてから曖昧なエラーを出すより、原因を直接伝えられる)。
        guard !isPaused else {
            presentStateError(String(localized: "Cannot save state while paused. Resume first."))
            return
        }
        // P475 — ファイル選択パネル(runModal)表示中は CVDisplayLink が走り続ける
        // ため自動一時停止し、OK/Cancel いずれの経路でも defer で確実に再開する。
        // (selectDisk() と同型、P229 の既存機構をそのまま流用)
        requestAutoPause()
        defer { releaseAutoPause() }

        let dir = statesDirectory()
        let fmt = DateFormatter()
        fmt.locale = Locale(identifier: "en_US_POSIX")
        fmt.dateFormat = "yyyyMMdd_HHmmss"
        let panel = NSSavePanel()
        panel.directoryURL = dir
        panel.nameFieldStringValue = "mx68k_\(fmt.string(from: Date())).mxstate"
        panel.allowedContentTypes = [Self.mxStateType]
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        // P481 (D-42) — mx68k_save_state() は「キューした」ことしか返さない。実際の
        // 結果はエミュスレッドがフレーム境界で処理した後に seq の変化として届くので、
        // ここでは同期的に成否を判定せず awaitStateOp() の完了待ちへ委ねる
        // (この時点ではまだ defer 未実行=一時停止中のため、同期待ちは必ず固まる)。
        let seq0 = mx68k_state_op_seq()
        guard mx68k_save_state(url.path) == 0 else {
            presentStateError(String(localized: "Failed to save state."))
            return
        }
        showTransientMessage(String(localized: "Saving state…"))
        awaitStateOp(seq0, isLoad: false, name: url.lastPathComponent)
    }

    /// ⌘⌥O — NSOpenPanel でファイルを選び、エミュ状態を復元する。
    func loadState() {
        // P481 (D-42) — 手動一時停止(⌘P)中は run_frame が回らず、キューしたロードが
        // 永久に実行されない。パネルを開く前に事前拒否する(saveState() と同型)。
        guard !isPaused else {
            presentStateError(String(localized: "Cannot load state while paused. Resume first."))
            return
        }
        // P475 — ファイル選択パネル(runModal)表示中は CVDisplayLink が走り続ける
        // ため自動一時停止し、OK/Cancel いずれの経路でも defer で確実に再開する。
        // (selectDisk() と同型、P229 の既存機構をそのまま流用)
        requestAutoPause()
        defer { releaseAutoPause() }

        let panel = NSOpenPanel()
        panel.directoryURL = statesDirectory()
        panel.allowedContentTypes = [Self.mxStateType]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        // P481 (D-42) — saveState() と同型。旧バージョンのステートファイルはここでは
        // 弾かれず(キュー成功)、実際の拒否は do_load_state() の rc として
        // awaitStateOp() 経由で届く。
        let seq0 = mx68k_state_op_seq()
        guard mx68k_load_state(url.path) == 0 else {
            presentStateError(String(localized: "Failed to load state."))
            return
        }
        showTransientMessage(String(localized: "Loading state…"))
        awaitStateOp(seq0, isLoad: true, name: url.lastPathComponent)
    }

    /// ⌘⇧S — パネルを介さず、タップ一つで自動命名保存する(iOS版P725の逆移植)。
    func quickSaveState() {
        guard !isPaused else {
            presentStateError(String(localized: "Cannot save state while paused. Resume first."))
            return
        }
        let dir = statesDirectory()
        let fmt = DateFormatter()
        fmt.locale = Locale(identifier: "en_US_POSIX")
        fmt.dateFormat = "yyyyMMdd_HHmmss"
        let name = "mx68k_\(fmt.string(from: Date())).mxstate"
        let url = dir.appendingPathComponent(name)
        let seq0 = mx68k_state_op_seq()
        guard mx68k_save_state(url.path) == 0 else {
            presentStateError(String(localized: "Failed to save state."))
            return
        }
        showTransientMessage(String(localized: "Saving state…"))
        awaitStateOp(seq0, isLoad: false, name: name)
    }

    /// ⌘⇧O — 直近に保存されたステートを問答無用で即座に読み込む
    /// (XM6のAlt+F1相当。ただしMXはスロット上書きではなくタイムスタンプ命名の
    /// 履歴を保持するため、「最新のファイルを選ぶ」処理で疑似的に同じ体験を作る)。
    func quickLoadState() {
        guard !isPaused else {
            presentStateError(String(localized: "Cannot load state while paused. Resume first."))
            return
        }
        guard let url = listSavedStates().first else {
            presentStateError(String(localized: "No saved states"))
            return
        }
        let seq0 = mx68k_state_op_seq()
        guard mx68k_load_state(url.path) == 0 else {
            presentStateError(String(localized: "Failed to load state."))
            return
        }
        showTransientMessage(String(localized: "Loading state…"))
        awaitStateOp(seq0, isLoad: true, name: url.lastPathComponent)
    }

    func listSavedStates() -> [URL] {
        let dir = statesDirectory()
        do {
            let files = try FileManager.default.contentsOfDirectory(
                at: dir, includingPropertiesForKeys: nil)
            return files
                .filter { $0.pathExtension == "mxstate" }
                .sorted { $0.lastPathComponent > $1.lastPathComponent }
        } catch {
            mx68k_log("[Swift][P737-STATE] listSavedStates contentsOfDirectory failed: \(error)")
            return []
        }
    }

    func deleteState(url: URL) {
        do {
            try FileManager.default.removeItem(at: url)
            mx68k_log("[Swift][P737-STATE] deleteState name=\(url.lastPathComponent) result=ok")
        } catch {
            mx68k_log("[Swift][P737-STATE] deleteState name=\(url.lastPathComponent) result=fail error=\(error)")
        }
    }

    /// P481 (D-42) — キューした save/load の完了をポーリングで待ち、実際の rc に
    /// 応じて成功トースト / エラーダイアログを出す。saveState()/loadState() の
    /// `defer { releaseAutoPause() }` が走って再開した後にタイマーが回るため、
    /// 一時停止したままポーリングし続けることはない。
    private var stateOpTimer: Timer?

    private func awaitStateOp(_ seq0: UInt32, isLoad: Bool, name: String) {
        stateOpTimer?.invalidate()
        isStateOperationPending = true
        EmulatorRunState.shared.isStateOperationPending = true   // P546 と同型のメニューゲート用ミラー
        let deadline = Date().addingTimeInterval(2.0)
        stateOpTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] t in
            guard let self = self else { t.invalidate(); return }
            if mx68k_state_op_seq() != seq0 {
                t.invalidate()
                self.stateOpTimer = nil
                self.isStateOperationPending = false
                EmulatorRunState.shared.isStateOperationPending = false
                let rc = mx68k_last_state_rc()
                if rc == 0 {
                    self.showTransientMessage(isLoad
                        ? String(localized: "State loaded: \(name)")
                        : String(localized: "State saved: \(name)"))
                } else {
                    self.presentStateError(Self.stateErrorText(rc: rc, isLoad: isLoad))
                }
            } else if Date() >= deadline {
                t.invalidate()
                self.stateOpTimer = nil
                self.isStateOperationPending = false
                EmulatorRunState.shared.isStateOperationPending = false
                self.presentStateError(String(localized:
                    "State operation did not complete (emulation stopped or paused)."))
            }
        }
    }

    /// P481 (D-42) — do_save_state()/do_load_state() の rc をユーザー向け文言へ。
    /// -11 は「旧バージョンのステートファイル」専用(P481 でメモリサイズ不正を
    /// -15 へ分離し二義を解消済み)。
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

    private func presentStateError(_ message: String) {
        // P481 (D-42) — NSAlert.runModal() 表示中も CVDisplayLink は走り続けるため、
        // P475 がファイル選択パネルに施したのと同型の自動一時停止をここにも施す。
        requestAutoPause()
        defer { releaseAutoPause() }
        let alert = NSAlert()
        alert.messageText = message
        alert.alertStyle = .warning
        alert.addButton(withTitle: String(localized: "OK"))
        alert.runModal()
    }

    // MARK: - Blank image creation
    //
    // P578 — P574 の `createBlankFDImage()`(Emulator メニュー直結の FD 専用経路)は
    // ここから撤去した。FD/SASI/SCSI の 3 種は Tools メニュー「イメージ作成」の
    // 専用ダイアログ(`BlankImageDialogs.swift`)へ一本化されており、生成そのものは
    // 従来どおり `BlankImageService` の static 関数が担う(ロジック無変更)。

    // MARK: - Auto-pause (P229)

    /// P229 — モーダル UI(設定シート・FDD 選択ダイアログ等)が同時に開放要求
    /// されても安全に扱うための参照カウント。
    private var autoPauseRequests = 0
    private var didAutoPause = false

    /// モーダル UI(設定シート・ファイル選択ダイアログ等)を開く直前に呼ぶ。
    func requestAutoPause() {
        autoPauseRequests += 1
        guard isRunning, powerState == .on, !isPaused, !didAutoPause else { return }
        engine.pause()
        statusText = "Paused"
        isPaused = true
        adpcmStatus.peak_level = 0   // P484c: 一時停止中はサウンドモニタの Peak Level を表示上 0 にする
        didAutoPause = true
    }

    /// モーダル UI を閉じた直後に呼ぶ(defer 推奨)。全ての要求が解放され、
    /// かつ requestAutoPause() が実際に一時停止させていた場合のみ再開する。
    func releaseAutoPause() {
        autoPauseRequests = max(0, autoPauseRequests - 1)
        guard autoPauseRequests == 0, didAutoPause else { return }
        didAutoPause = false
        engine.resume()
        statusText = "Running"
        isPaused = false
    }

    func togglePause() {
        didAutoPause = false   // P229: 手動トグルは自動一時停止の追跡から所有権を奪う。
        if isPaused {
            engine.resume()
            statusText = "Running"
        } else {
            engine.pause()
            statusText = "Paused"
            adpcmStatus.peak_level = 0   // P484c: 一時停止中はサウンドモニタの Peak Level を表示上 0 にする
        }
        isPaused.toggle()
    }

    /// P555 — ターボ(等倍 ⇔ 目標倍率)のワンタッチ切替。WebX68k の方式に倣い、
    /// 倍率そのものは設定画面で選んでおき、実行中はこのトグルだけを使う。
    /// P624 — 固定倍率(2x〜5x)では音声をピッチシフトして鳴らし続ける
    /// (WebX68k 相当の線形補間可変レート方式)。ノーウェイトのみ従来どおりミュート。
    func toggleTurbo() {
        /* P697: 録画中はターボ/ノーウェイトを禁止する。録画の PTS は壁時計基準
         * なので、実行速度が変わると「実時間 1 秒あたりのゲストフレーム数」だけが
         * 変動し、再生時に早送り/コマ落ちの混ざった映像になる。メニュー項目の
         * `.disabled` だけではキーボードショートカット経由の発火を防げないため、
         * 唯一のターボ活性化エントリポイントであるこの関数にもガードを置く
         * (setTurboTargetMultiplier は isTurboActive が既に true のときだけ
         * 再適用する設計なので、OFF から新規に有効化する経路にはならない)。 */
        guard !isRecording else {
            showTransientMessage(String(localized:
                "Turbo is unavailable while recording. Stop recording first."))
            return
        }
        isTurboActive.toggle()
        applyTurboState()
        // P556: ノーウェイトは倍率で表現できないため文言を分岐する。
        showTransientMessage(isTurboActive
            ? (turboTargetMultiplier == kTurboNoWaitMultiplier
                ? String(localized: "Turbo: ON (No Wait)")
                : String(localized: "Turbo: ON (\(turboTargetMultiplier)x)"))
            : String(localized: "Turbo: OFF"))
    }

    /// P555 — 現在の isTurboActive / turboTargetMultiplier をフレームループと
    /// Bridge へ反映する。ターボ OFF のときは倍率 1 = P555 以前と完全に同一の挙動。
    ///
    /// P556 — 目標倍率が `kTurboNoWaitMultiplier` のときだけ、駆動元を
    /// CVDisplayLink のアキュムレータから `EmulatorEngine` の専用スレッドへ
    /// 切り替える。P624 以降、音声は固定倍率ならピッチシフト・ノーウェイトなら
    /// ミュートと経路ごとに分かれる(末尾の分岐を参照)。
    /// **メインスレッドから呼ぶこと**(engine 側の start/stopNoWaitThread が
    /// メインスレッド限定のため。呼び出し元は toggleTurbo / setTurboTargetMultiplier /
    /// pushConfig の 3 つで、いずれも UI 由来のメインスレッド実行)。
    private func applyTurboState() {
        // isRunning ガード: エミュレーション未起動(Core 未 init / shutdown 済み)で
        // 専用スレッドを走らせると mx68k_run_frame() が無効な Core 状態を触る。
        let useNoWait = isTurboActive && turboTargetMultiplier == kTurboNoWaitMultiplier && isRunning
        if useNoWait {
            // アキュムレータ側は等倍へ戻しておく(ノーウェイト中は runFrame() が
            // 早期 return するので実効はしないが、OFF 復帰時に倍率が残らないよう明示)。
            engine.setTurboMultiplier(1)
            engine.startNoWaitThread()
        } else {
            // ★先に専用スレッドを確実に合流させてから倍率を反映する(ノーウェイト →
            // 通常倍率ターボへの直接切替もこの経路を通る)。未起動なら実質 no-op。
            engine.stopNoWaitThread()
            engine.setTurboMultiplier(isTurboActive ? turboTargetMultiplier : 1)
        }
        // P624 — 音声の扱い。固定倍率ターボ(2x〜5x)は Bridge 側の消費側デシメーションで
        // ピッチシフト再生する(倍率を Q16 固定小数で渡す)。ターボ OFF は等倍 =
        // P624 以前と同一経路。
        // P625 — ノーウェイトは実速度がホスト負荷依存で固定倍率に落とせないため、
        // 予約値 MX68K_TURBO_AUDIO_RATE_AUTO を渡し、Bridge 側(mx68k_audio_read)が
        // リング充填率のフィードバックから再生レートを自動推定してピッチシフト再生する
        // (P624 までのミュートを置換)。
        // ★ノーウェイト判定にここで `useNoWait` を使わないこと: `useNoWait` は
        //   `isRunning` も条件に含むため、未起動状態でノーウェイト設定のまま
        //   ターボ ON にすると等倍扱いになり、負の倍率(-1)を渡してしまう。
        //   音声の扱いは実行状態と無関係に「ノーウェイト設定なら常に AUTO」でよい。
        if !isTurboActive {
            // P629(D-64): AudioUnitの実消費ペースが要求レートに関わらず約44100Hz相当に
            // 固着する現象が実測で確認された(P629 Code Investigation2回目)。44100Hz超の
            // 設定では等倍(UNITY、無加工パススルー)だと生成が実消費より速くなりリング
            // 恒常満杯→バースト破棄(プチノイズ)を招く。P625で実装済みのAUTOデシメーション
            // (リング充填率フィードバックで実効比率を自動収束)は「なぜ実消費が44100相当に
            // 固着するか」を問わず対処できるため、44100Hz超の設定では通常速度でもAUTOを
            // 使う。★AUTOの比率は1.0以上にしか振れない設計(Bridge/EmulatorBridge.c:
            // 8204-8215、消費を「速める」方向専用)のため、44100Hz以下(22050Hz含む)は
            // 対象外——生成が実消費より遅い場合はAUTOが無力(比率が1.0に頭打ちになるだけで
            // 現行UNITYと数学的に同一、Round 1 Code Review指摘)。44100Hz(既定・大多数の
            // ユーザー体験)・22050Hzは従来通りUNITY(バイト単位で無加工)のまま。
            let rate = mx68k_get_audio_sample_rate()
            mx68k_set_turbo_audio_rate(Int32(
                rate > 44100 ? MX68K_TURBO_AUDIO_RATE_AUTO : MX68K_TURBO_AUDIO_RATE_UNITY))
        } else if turboTargetMultiplier == kTurboNoWaitMultiplier {
            mx68k_set_turbo_audio_rate(Int32(MX68K_TURBO_AUDIO_RATE_AUTO))
        } else {
            mx68k_set_turbo_audio_rate(Int32(turboTargetMultiplier) << 16)
        }
    }

    /// P555 — 設定画面の目標倍率 Picker から呼ばれる。クロック速度が「毎フレーム
    /// 読み直される」設計に倣い、ターボが現在 ON なら即座に反映する(リセット不要)。
    func setTurboTargetMultiplier(_ multiplier: Int) {
        // P556: クランプは共通ヘルパーへ集約。ここを直さないと Picker で
        // 「ノーウェイト」を選んだ瞬間に 2 へ引き戻され、機能自体が成立しない。
        turboTargetMultiplier = clampTurboMultiplier(multiplier)
        if isTurboActive { applyTurboState() }
    }

    /// P557 — 設定画面の「FD アクセス高速化」トグルから呼ばれる。クロック速度や
    /// ターボ倍率と同じ即時反映(リセット不要)。Bridge 側 g_fd_fast_access は
    /// fdd.c の usleep 経路が毎回読み直す単純グローバル。
    func setFDFastAccess(_ enabled: Bool) {
        mx68k_set_fd_fast_access(enabled ? 1 : 0)
    }

    // MARK: - P684: ドライブ番号 → 状態プロパティの写像
    //
    // P684 で FDD が 2 台 → 4 台になったため、それまで `if drive == 0 {...} else {...}`
    // という 2 値分岐で書かれていた 10 箇所前後を、この 8 個のヘルパ経由へ統一する。
    // 分岐を 1 か所へ集約する方針は ExtensionsConfig.hddPath/setHDDPath(P455 の
    // SASI 2 → 8 台化)と同じ — 分岐の書き写しは片方だけ間違える典型的な事故源。
    // ★純粋なリファクタリングであり、ドライブ 0/1 についての挙動は完全に不変。

    private func fddPath(_ drive: Int) -> String {
        switch drive {
        case 0: return fdd0Path
        case 1: return fdd1Path
        case 2: return fdd2Path
        case 3: return fdd3Path
        default: return ""
        }
    }

    private func setFddPath(_ drive: Int, _ path: String) {
        switch drive {
        case 0: fdd0Path = path
        case 1: fdd1Path = path
        case 2: fdd2Path = path
        case 3: fdd3Path = path
        default: break
        }
    }

    private func fddWriteProtect(_ drive: Int) -> Bool {
        switch drive {
        case 0: return fdd0WriteProtect
        case 1: return fdd1WriteProtect
        case 2: return fdd2WriteProtect
        case 3: return fdd3WriteProtect
        default: return false
        }
    }

    private func setFddWriteProtect(_ drive: Int, _ v: Bool) {
        switch drive {
        case 0: fdd0WriteProtect = v
        case 1: fdd1WriteProtect = v
        case 2: fdd2WriteProtect = v
        case 3: fdd3WriteProtect = v
        default: break
        }
    }

    private func fddWriteProtectBeforeZip(_ drive: Int) -> Bool? {
        switch drive {
        case 0: return fdd0WriteProtectBeforeZip
        case 1: return fdd1WriteProtectBeforeZip
        case 2: return fdd2WriteProtectBeforeZip
        case 3: return fdd3WriteProtectBeforeZip
        default: return nil
        }
    }

    private func setFddWriteProtectBeforeZip(_ drive: Int, _ v: Bool?) {
        switch drive {
        case 0: fdd0WriteProtectBeforeZip = v
        case 1: fdd1WriteProtectBeforeZip = v
        case 2: fdd2WriteProtectBeforeZip = v
        case 3: fdd3WriteProtectBeforeZip = v
        default: break
        }
    }

    private func fddArchiveTempDir(_ drive: Int) -> URL? {
        switch drive {
        case 0: return fdd0ArchiveTempDir
        case 1: return fdd1ArchiveTempDir
        case 2: return fdd2ArchiveTempDir
        case 3: return fdd3ArchiveTempDir
        default: return nil
        }
    }

    private func setFddArchiveTempDir(_ drive: Int, _ v: URL?) {
        switch drive {
        case 0: fdd0ArchiveTempDir = v
        case 1: fdd1ArchiveTempDir = v
        case 2: fdd2ArchiveTempDir = v
        case 3: fdd3ArchiveTempDir = v
        default: break
        }
    }

    /// P673 — FD イメージの選択→マウント導線(ツールバーの「フォルダ」ボタンと
    /// File メニューの両方から呼ばれる共通処理)。ToolbarView.selectDisk() の
    /// 中身をそのまま移設したもの(挙動は無変更)。
    ///
    /// - Parameter forceWriteProtect: nil なら通常挿入
    ///   (resolvedWriteProtectForNewMount(drive:) の値を使う、既存 selectDisk と同じ)。
    ///   true を渡すと強制的に書込み禁止でマウントする
    ///   (File メニュー「挿入(書込禁止)…」用。zip 経由は既存仕様どおり常に
    ///   書込み禁止のため forceWriteProtect の値によらない)。
    ///   P673b — この強制は **この 1 枚に対する一時的なもの**で、config へは
    ///   永続化しない(zip マウントと同じ方式)。イジェクト/別ディスクの
    ///   マウント時に元のトグル値へ自動復元される。
    func browseAndMountFDD(drive: Int, forceWriteProtect: Bool? = nil) {
        // P229 — ファイル選択パネル(runModal)表示中は CVDisplayLink が走り続ける
        // ため自動一時停止し、OK/Cancel いずれの経路でも defer で確実に再開する。
        requestAutoPause()
        defer { releaseAutoPause() }

        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.canChooseFiles = true

        // P554: zip 圧縮イメージも選べるようにする(展開先を一時ディレクトリへ置き、
        // 強制的に書込み禁止でマウントする — ArchiveMountService / mountFDDFromArchive 参照)。
        let extensions = ["xdf", "dim", "d88", "hdm", "2hd", "img", "zip"]
        var contentTypes: [UTType] = []
        for ext in extensions {
            if let ut = UTType(filenameExtension: ext) {
                contentTypes.append(ut)
            }
        }
        if !contentTypes.isEmpty {
            panel.allowedContentTypes = contentTypes
        }

        guard panel.runModal() == .OK, let url = panel.url else { return }

        if url.pathExtension.lowercased() == "zip" {
            // P554: 展開 → 単一なら自動マウント / 複数なら選択シート / 失敗ならメッセージ。
            handleArchiveSelection(drive: drive, zipPath: url.path)
        } else {
            // P599: zip 由来の一時的な書込み禁止が残っていれば先に復元してから読む。
            let wp = forceWriteProtect ?? resolvedWriteProtectForNewMount(drive: drive)
            mountFDD(drive: drive, path: url.path, writeProtect: wp)
            if forceWriteProtect == true {
                // P673b — zip 経由の一時退避(mountFDDFromArchive)と同じ方式に統一。
                // config へは永続化しない(この1回の挿入だけを強制するもので、
                // ユーザーの恒久設定を書き換えない)。イジェクト時に releaseArchiveTempDir
                // 経由で restoreWriteProtectIfNeeded が必ず呼ばれ、元の値へ戻る。
                setFddWriteProtectBeforeZip(drive, fddWriteProtect(drive))
                setFddWriteProtect(drive, true)
            }
        }
    }

    func mountFDD(drive: Int, path: String, writeProtect: Bool) {
        // P554 — 「ディスク選択」ボタンはメディア挿入済みでも無効化されず、Core 側
        // (fdc.c FDD_SetFD)も新規マウント時に内部で自動 eject する。つまり明示的な
        // eject を経ずに zip 由来のドライブへ別ディスクを上書きマウントできるため、
        // 通常ファイル経路も含めた **すべてのマウント経路の入口**で、そのドライブの
        // 旧一時ディレクトリを破棄する(mountFDDFromArchive もここを通る)。
        releaseArchiveTempDir(drive: drive)
        path.withCString { cPath in
            let result = mx68k_fdd_insert(Int32(drive), cPath)
            if result == 0 {
                // P271: 挿入直後に、トグルが ON ならライトプロテクトを反映する
                // (Core FDD_SetReadOnly は一方向セット。詳細は Bridge 実装コメント)。
                if writeProtect {
                    mx68k_fdd_set_write_protect(Int32(drive), 1)
                }
                setFddPath(drive, (path as NSString).lastPathComponent)
                onFDDMounted?(drive, path)
                // P185: only reboot on the INITIAL launch mount (isRunning still false
                // during startEmulation → the IPL re-reads the disk on reset to boot it).
                // Runtime mounts (user swapping disks mid-game, isRunning true) do NOT
                // reset — real hardware doesn't reset on floppy insert, and multi-disk
                // games swap mid-run; the user resets manually to boot a new disk.
                if !isRunning {
                    mx68k_schedule_hard_reset()
                }
            }
        }
    }

    /// P673c — 「実際にイジェクトされた」(ユーザー操作 ejectFDD / ゲスト操作
    /// reconcileFDDMedia の両方)場合に、書込み禁止トグルを問答無用で OFF へ
    /// 確定させる。南京錠は「今挿入中のディスクの保護状態」のみを表す、という
    /// 単純なモデルへの統一(ユーザーとの合意 — 複数回のディスク交換をまたいで
    /// 保護を維持し続けたい場合は、交換のたびに再度 ON にする運用になる)。
    ///
    /// ★このヘルパは ejectFDD/reconcileFDDMedia からのみ呼ぶこと。mountFDD 側
    /// (イジェクトを経ない上書きマウント経路)からは絶対に呼ばないこと —
    /// そちらは既存の resolvedWriteProtectForNewMount/restoreWriteProtectIfNeeded
    /// が「一時強制の直後にイジェクトを経ずに上書きマウントされた場合、本来の
    /// 恒久ポリシー値へ正しく復元する」という別の役割を担っており、ここで
    /// 無条件 false 上書きを割り込ませると、一時強制挿入の直後に別ディスクを
    /// 上書き挿入した際、本来復元されるべき恒久ポリシー値が失われる
    /// (P673c Code Review で指摘された回帰、詳細は P673c_plan.md 参照)。
    private func resetWriteProtectOnEject(drive: Int) {
        setFddWriteProtect(drive, false)
        onFDDWriteProtectChanged?(drive, false)   // config に永続化(次回起動時もOFF)
    }

    func ejectFDD(drive: Int) {
        // P673c — 「実際にメディアが入っていたか」を先に記録する。Fileメニューの
        // 「イジェクト」・電源OFF時の mx68k_sram_eject_on_poweroff 経路は空ドライブに
        // 対しても無条件に呼ばれるため、事前セットした保護トグルを巻き込まないようにする。
        let hadDisk = !fddPath(drive).isEmpty
        // P554 — zip 由来のマウントなら、展開先の一時ディレクトリをここで破棄する。
        releaseArchiveTempDir(drive: drive)
        if hadDisk {
            resetWriteProtectOnEject(drive: drive)   // ★P673c — 実際に入っていた場合のみOFF確定
        }
        mx68k_fdd_eject(Int32(drive))
        setFddPath(drive, "")
        // P191: 取り出し状態を config に永続化(挿入側 onFDDMounted と対称)。
        onFDDEjected?(drive)
    }

    // MARK: - P554: zip 圧縮ディスクイメージのマウント

    /// zip から展開したディスクイメージをマウントする。展開先は一時ディレクトリであり
    /// 破棄後に書込み内容が失われるため、**強制的に書込み禁止**でマウントする
    /// (ユーザー承認済みの安全策)。
    func mountFDDFromArchive(drive: Int, extractedPath: String, tempDir: URL) {
        // ★ 旧一時ディレクトリの破棄。mountFDD 側でも同じ処理を行うが(通常ファイルでの
        //   上書きマウント経路をカバーするため)、こちら側でも明示的に先行して呼ぶ
        //   — 新しい tempDir を記録するのは mountFDD の後なので、破棄対象を取り違えない。
        releaseArchiveTempDir(drive: drive)
        mountFDD(drive: drive, path: extractedPath, writeProtect: true)
        // ツールバーの南京錠アイコンは fddNWriteProtect を見ているため、実際の保護状態と
        // 表示が食い違わないようトグル表示も ON にする。config へは永続化しない
        // (zip マウントは一時的なものであり、ユーザーの恒久設定を書き換えない)。
        setFddWriteProtectBeforeZip(drive, fddWriteProtect(drive))
        setFddWriteProtect(drive, true)
        // マウント結果によらず記録する。失敗時も次回のマウント/イジェクト、
        // 最悪でも次回起動時のスイープで確実に破棄される。
        setFddArchiveTempDir(drive, tempDir)
    }

    /// 対象ドライブに紐づく zip 展開用一時ディレクトリがあれば破棄して忘れる。
    /// P673b — 書込み禁止トグルの復元(restoreWriteProtectIfNeeded)は zip 由来か
    /// どうかに関わらず常に試みる(zip 一時ディレクトリの有無で分岐すると、
    /// zip を経由しない「挿入(書込禁止)…」からの一時退避が復元されないまま
    /// 取り残される — P673 hands-on で発見)。退避値が無ければ
    /// restoreWriteProtectIfNeeded 自身が no-op になるため副作用は無い。
    private func releaseArchiveTempDir(drive: Int) {
        let tempDir = fddArchiveTempDir(drive)
        if let tempDir = tempDir {
            ArchiveMountService.cleanup(tempDir)
            setFddArchiveTempDir(drive, nil)
        }
        restoreWriteProtectIfNeeded(drive: drive)
    }

    /// P599 — zip 由来のトグル書き換えを元に戻す。releaseArchiveTempDir(既存の全マウント/
    /// イジェクト経路の入口)と、通常マウント呼び出し元が事前に呼ぶ
    /// resolvedWriteProtectForNewMount の両方から呼ばれる共有ロジック。
    private func restoreWriteProtectIfNeeded(drive: Int) {
        if let saved = fddWriteProtectBeforeZip(drive) {
            setFddWriteProtect(drive, saved)
            setFddWriteProtectBeforeZip(drive, nil)
        }
    }

    /// P599 — 通常(zip でない)FD イメージをマウントする直前に呼ぶこと。zip 由来の
    /// トグル書き換えが残っていれば先に復元してから、現在のトグル値を返す——
    /// 呼び出し元が古い値をキャプチャする前に復元を完了させるための入口。
    func resolvedWriteProtectForNewMount(drive: Int) -> Bool {
        restoreWriteProtectIfNeeded(drive: drive)
        return fddWriteProtect(drive)
    }

    /// zip を展開し、内容に応じて分岐する共通経路(ツールバーの選択パネル・D&D の両方から呼ぶ)。
    /// - イメージ 1 個 → そのまま自動マウント
    /// - イメージ 2 個以上 → 選択シートを提示(EmulatorView が観測)
    /// - 展開失敗 / イメージ 0 個 → 一時メッセージで通知(マウントしない)
    func handleArchiveSelection(drive: Int, zipPath: String) {
        // 展開は同期実行で数百ms〜数秒かかりうるため、その間は自動一時停止する
        // (requestAutoPause は参照カウント式なので、ツールバー経路での二重呼び出しも安全)。
        requestAutoPause()
        let result = ArchiveMountService.extract(zipPath: zipPath)
        releaseAutoPause()

        switch result {
        case .success(let archive):
            if archive.images.count == 1 {
                mountFDDFromArchive(drive: drive,
                                    extractedPath: archive.images[0].path,
                                    tempDir: archive.tempDir)
            } else {
                archivePickerImages = archive.images
                archivePickerTempDir = archive.tempDir
                archivePickerDrive = drive
                showArchivePicker = true
            }
        case .failure(let error):
            if let extractError = error as? ArchiveMountService.ExtractError,
               case .noDiskImageFound = extractError {
                showTransientMessage(String(localized: "No disk image found in the archive"))
            } else {
                showTransientMessage(String(localized: "Failed to extract the archive"))
            }
        }
    }

    /// 選択シートで確定されたときの処理。
    func completeArchiveSelection(_ url: URL) {
        let drive = archivePickerDrive
        let tempDir = archivePickerTempDir
        clearArchivePickerState()
        guard let tempDir = tempDir else { return }
        mountFDDFromArchive(drive: drive, extractedPath: url.path, tempDir: tempDir)
    }

    /// 選択シートがキャンセルされたときの処理。展開済み一時ディレクトリは破棄する。
    func cancelArchiveSelection() {
        if let tempDir = archivePickerTempDir {
            ArchiveMountService.cleanup(tempDir)
        }
        clearArchivePickerState()
    }

    private func clearArchivePickerState() {
        showArchivePicker = false
        archivePickerImages = []
        archivePickerTempDir = nil
    }

    /// P443 (D-7) — ゲスト側(エミュレート中のプログラム)が FDD をイジェクトした場合に
    /// Swift 側の表示状態を追従させる。Core(fdd.c `FDD_EjectFD`)はゲスト側イジェクトを
    /// 正しく処理し Bridge へ通知しているが、`fdd0Path`/`fdd1Path` は Swift 起点の
    /// `mountFDD()`/`ejectFDD()` でしか書き換えられなかったため、ゲスト側イジェクト後も
    /// 古いパスが残っていた(ツールバー表示・イジェクトボタン活性・D&D 投入先判定・
    /// ライトプロテクト即時適用の `hasDisk` 判定がすべてこれを見ている)。
    /// 1Hz の status ポーリングごとに Core 側のメディア有無と突き合わせる。
    ///
    /// - `mx68k_fdd_eject()` は呼ばない — Core 側では既にイジェクト済みで、二重に
    ///   呼ぶ必要が無いため。
    /// - config 永続化などの後処理は Swift 起点の `ejectFDD()` と対称に
    ///   `onFDDEjected` で行う(実機はディスクが物理的に排出される以上、次回起動時に
    ///   空であるのが忠実な挙動)。
    /// - 由来は `s_fdd_present[]`(`status.fddN_media_present`)であり
    ///   `FDD_IsReady()` ではない。後者は挿入直後の `SetDelay` 猶予期間中 false を
    ///   返す窓があり、1Hz ポーリングがそこに当たると挿入直後のパスを誤って消しうる。
    private func reconcileFDDMedia(_ newStatus: MX68KStatus) {
        if !newStatus.fdd0_media_present && !fdd0Path.isEmpty {
            // P554: Swift 起点の ejectFDD と対称に、zip 展開の一時ディレクトリも破棄する
            // (zip 由来のマウントは常に書込み禁止なので、eject 時の書き戻しは発生しない)。
            releaseArchiveTempDir(drive: 0)
            // P673c — ゲスト側イジェクトでも書込み禁止トグルを OFF へ確定させる
            // (この if は既に !fdd0Path.isEmpty の中なので hadDisk ガードは不要)。
            resetWriteProtectOnEject(drive: 0)
            fdd0Path = ""
            onFDDEjected?(0)
        }
        if !newStatus.fdd1_media_present && !fdd1Path.isEmpty {
            releaseArchiveTempDir(drive: 1)
            // P673c — 同上(この if は既に !fdd1Path.isEmpty の中)。
            resetWriteProtectOnEject(drive: 1)
            fdd1Path = ""
            onFDDEjected?(1)
        }
        // P684 — FD2/FD3 も同型。ステータスバー/ツールバーには出ないが、
        // fdd2Path/fdd3Path は File メニューの「イジェクト」活性判定と
        // config 永続化の起点になるため、ゲスト側イジェクトへの追従は必要。
        if !newStatus.fdd2_media_present && !fdd2Path.isEmpty {
            releaseArchiveTempDir(drive: 2)
            resetWriteProtectOnEject(drive: 2)
            fdd2Path = ""
            onFDDEjected?(2)
        }
        if !newStatus.fdd3_media_present && !fdd3Path.isEmpty {
            releaseArchiveTempDir(drive: 3)
            resetWriteProtectOnEject(drive: 3)
            fdd3Path = ""
            onFDDEjected?(3)
        }
    }

    /// P271 — FDライトプロテクトのトグル。ユーザー承認済みの ON/OFF 非対称設計:
    /// - ON(protect=true): トグル状態を保持・永続化し、さらにそのドライブに
    ///   ディスクが挿入済みなら**即座に** mx68k_fdd_set_write_protect を呼んで反映する
    ///   (Core FDD_SetReadOnly は挿入時である必要がなくいつでも呼べる)。未挿入なら
    ///   次回挿入時に mountFDD 経由で反映される。
    /// - OFF(protect=false): Core にクリア関数が無いため挿入中のディスクには即座に
    ///   反映できない。トグル状態の保持・永続化のみ行い、実際の解除は次回そのドライブへ
    ///   挿入(eject→再挿入)する時から反映される。
    func setWriteProtect(drive: Int, protect: Bool) {
        setFddWriteProtect(drive, protect)
        onFDDWriteProtectChanged?(drive, protect)   // config に永続化

        // ON かつディスク挿入済みのときのみ、その場で Core へ反映する。
        if protect {
            let hasDisk = !fddPath(drive).isEmpty
            if hasDisk {
                mx68k_fdd_set_write_protect(Int32(drive), 1)
            }
        }
    }

    // MARK: - HDD (SASI .hdf) — P200

    /// SASI HDD イメージ(.hdf)をマウントする。Bridge が Config.HDImage[unit*2] に
    /// パスを設定する(P239: 次回ディスクアクセスから即座に反映 — 手動リセット不要)。
    /// 成功時は config へパス保存(onHDDChanged)+ 下部 StatusBar に一時メッセージ。
    func insertHDD(unit: Int, url: URL) {
        let path = url.path
        let result = path.withCString { mx68k_hdd_insert(Int32(unit), $0) }
        if result == 0 {
            onHDDChanged?(unit, path)   // config に永続化
            showTransientMessage(String(localized: "HDD image set: \(url.lastPathComponent) (takes effect immediately)"))
        } else if result == -3 {
            // P458 (D-29): 実ファイルサイズが SASI の実機容量(10/20/40MB)と一致しない
            showTransientMessage(String(localized: "HDD image size does not match a valid SASI capacity (10/20/40MB)"))
        } else if result == -4 {
            // P687 (D-29 B): 先頭 8 バイトが "X68SCSI1" 署名と一致 = SCSI 用に
            // フォーマットされたイメージが SASI スロットへ誤挿入された
            showTransientMessage(String(localized: "This image may be SCSI-formatted (X68SCSI1 identifier detected). Try inserting it into a SCSI slot."))
        } else {
            showTransientMessage(String(localized: "HDD image mount failed"))
        }
    }

    /// SASI HDD イメージを取り外す。Config.HDImage[unit*2] を空にする(P239: 次回ディスクアクセスから即座に反映 — 手動リセット不要)。
    func ejectHDD(unit: Int) {
        // P503 (b): insert と対称に戻り値で分岐する(-2 = wired 機種が SCSI の
        // ため拒否。現在の UI では SASI 設定の Eject 自体が塞がれているので
        // 到達しないが、C API 契約に合わせた多層防御)。
        let result = mx68k_hdd_eject(Int32(unit))
        if result == 0 {
            onHDDChanged?(unit, "")   // config に永続化(空 = 未装着)
            showTransientMessage(String(localized: "HDD image ejected (takes effect immediately)"))
        } else {
            showTransientMessage(String(localized: "HDD eject failed"))
        }
    }

    // MARK: - SCSI (external CZ-6BS1) — P241 Stage A

    /// 外付け SCSI(CZ-6BS1)ディスクイメージをマウントする(id = SCSI ID 0..6)。
    /// Bridge が Config.SCSIEXHDImage[id] にパスを設定する(次回ブロックアクセスから
    /// 即座に反映 — 手動リセット不要)。成功時は config へパス保存(onSCSIChanged)。
    func insertSCSI(id: Int, url: URL) {
        let path = url.path
        let result = path.withCString { mx68k_scsi_insert(Int32(id), $0) }
        if result == 0 {
            onSCSIChanged?(id, path)   // config に永続化
            showTransientMessage(String(localized: "SCSI image set: \(url.lastPathComponent) (takes effect immediately)"))
        } else if result == -3 {
            // P458 (D-29): 512B 倍数でない、または 10MB〜4095MiB の範囲外
            showTransientMessage(String(localized: "SCSI image size is out of the valid range"))
        } else {
            showTransientMessage(String(localized: "SCSI image mount failed"))
        }
    }

    /// 外付け SCSI イメージを取り外す。Config.SCSIEXHDImage[id] を空にする(次回
    /// ブロックアクセスから即座に反映 — 手動リセット不要)。
    func ejectSCSI(id: Int) {
        mx68k_scsi_eject(Int32(id))
        onSCSIChanged?(id, "")   // config に永続化(空 = 未装着)
        showTransientMessage(String(localized: "SCSI image ejected (takes effect immediately)"))
    }

    // MARK: - SCSI MO (magneto-optical, ID5 dedicated slot) — P668

    /// P673 — File メニュー「MO ドライブ」→「挿入…」用。拡張子基準は
    /// SCSISettingsView.browseDiskImage() と同じ(hds/hdf/mos + 汎用 data)。
    func browseAndInsertMO() {
        requestAutoPause()
        defer { releaseAutoPause() }

        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        let types = ["hds", "hdf", "mos"].compactMap { UTType(filenameExtension: $0) } + [UTType.data]
        panel.allowedContentTypes = types

        guard panel.runModal() == .OK, let url = panel.url else { return }
        insertMO(url: url)
    }

    /// SCSI MO(.mos)イメージを装着する。起動時にMOパスが設定済みであれば
    /// P674でライブ反映(即座にゲストへUNIT ATTENTION通知)、未設定なら従来どおり
    /// 次回ハードリセット(⌘R)で反映。受理サイズは128/230/540/640MBの完全一致のみ
    /// (XM6:vm/disk.cpp:2131-2187)。
    ///
    /// P674 — スレッド安全性(★Code Review指摘、改訂版): Open/Ejectはdcacheを
    /// delete/newするため、CVDisplayLinkスレッドの`mx68k_run_frame()`実行と
    /// 排他する必要がある。`requestAutoPause()`(pauseLock)は実行中のフレームを
    /// 同期的に待たないため単独では不十分——`engine.withEmulationLock`
    /// (`mx68k_run_frame()`と同一のロック)でBridge呼び出し自体を包む。
    /// `requestAutoPause`は従来どおり維持する(UXの一貫性のため、次フレーム以降の
    /// 実行を止めておく)。呼び出し元(Fileメニュー・SCSI設定タブのSelect…/
    /// Eject/D&D)がそれぞれ囲む代わりに、この関数自身の内側で両方囲む
    /// (`requestAutoPause`はrefcount式なので二重呼び出しは無害)。
    @discardableResult
    func insertMO(url: URL) -> Bool {
        requestAutoPause()
        defer { releaseAutoPause() }
        let path = url.path
        let result = engine.withEmulationLock {
            path.withCString { mx68k_mo_insert($0) }
        }
        switch result {
        case 0:
            onMOChanged?(path)
            showTransientMessage(String(localized: "MO image inserted: \(url.lastPathComponent)"))
            return true
        case 1:
            onMOChanged?(path)
            showTransientMessage(String(localized:
                "MO image set: \(url.lastPathComponent) (takes effect after a hard reset ⌘R)"))
            return true
        case -3:
            showTransientMessage(String(localized:
                "MO image size must be exactly 128, 230, 540 or 640 MB"))
            return false
        case -5:
            showTransientMessage(String(localized:
                "The guest has locked the MO drive — eject it from the guest first"))
            return false
        default:
            showTransientMessage(String(localized: "MO image mount failed"))
            return false
        }
    }

    /// SCSI MO イメージを取り外す。P674でライブ反映(装着済みならその場でイジェクト)、
    /// 未装着なら従来どおり次回ハードリセット(⌘R)で反映。
    @discardableResult
    func ejectMO() -> Bool {
        requestAutoPause()
        defer { releaseAutoPause() }
        let result = engine.withEmulationLock { mx68k_mo_eject() }
        switch result {
        case 0:
            onMOChanged?("")
            showTransientMessage(String(localized: "MO image ejected"))
            return true
        case 1:
            onMOChanged?("")
            showTransientMessage(String(localized: "MO image ejected (takes effect after a hard reset ⌘R)"))
            return true
        case -5:
            showTransientMessage(String(localized:
                "The guest has locked the MO drive — eject it from the guest first"))
            return false
        default:
            showTransientMessage(String(localized: "MO eject failed"))
            return false
        }
    }

    // MARK: - SCSI CD-ROM (ID6 dedicated slot) — P676

    /// P676 — File メニュー「CD-ROM ドライブ」→「挿入…」用。拡張子は
    /// `.iso` に限定しない(D-8 再発防止方針 — 実データの検証は Bridge 側
    /// `mx68k_cd_insert` と `SCSICD::OpenIso` が担う)。
    func browseAndInsertCD() {
        requestAutoPause()
        defer { releaseAutoPause() }

        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        let types = ["iso", "bin", "img"].compactMap { UTType(filenameExtension: $0) } + [UTType.data]
        panel.allowedContentTypes = types

        guard panel.runModal() == .OK, let url = panel.url else { return }
        insertCD(url: url)
    }

    /// SCSI CD-ROM(ISO / Mode1)イメージを装着する。起動時に CD パスが設定済みで
    /// あればライブ反映(即座にゲストへ UNIT ATTENTION 通知)、未設定なら次回
    /// ハードリセット(⌘R)で反映。受理サイズは範囲判定(2352B または 2048B の
    /// セクタ倍数、かつ各上限以内 — XM6:vm/disk.cpp:2772-2858)。
    ///
    /// ★スレッド安全性(P674 の教訓): `SCSICD::Open`/`Eject` は `disk.dcache` を
    /// delete/new するため、CVDisplayLink スレッドの `mx68k_run_frame()` と
    /// 排他する必要がある。`requestAutoPause()` は実行中のフレームを同期的に
    /// 待たないため単独では不十分 —— `engine.withEmulationLock`
    /// (`mx68k_run_frame()` と同一のロック)で Bridge 呼び出し自体を包む。
    @discardableResult
    func insertCD(url: URL) -> Bool {
        requestAutoPause()
        defer { releaseAutoPause() }
        let path = url.path
        let result = engine.withEmulationLock {
            path.withCString { mx68k_cd_insert($0) }
        }
        switch result {
        case 0:
            onCDChanged?(path)
            showTransientMessage(String(localized: "CD-ROM image inserted: \(url.lastPathComponent)"))
            return true
        case 1:
            onCDChanged?(path)
            showTransientMessage(String(localized:
                "CD-ROM image set: \(url.lastPathComponent) (takes effect after a hard reset ⌘R)"))
            return true
        case -3:
            showTransientMessage(String(localized:
                "Not a valid CD-ROM image (must be a multiple of the 2048-byte or 2352-byte sector size)"))
            return false
        case -5:
            showTransientMessage(String(localized:
                "The guest has locked the CD-ROM drive — eject it from the guest first"))
            return false
        default:
            showTransientMessage(String(localized: "CD-ROM image mount failed"))
            return false
        }
    }

    /// SCSI CD-ROM イメージを取り外す。装着済みならその場でイジェクト、
    /// 未装着なら次回ハードリセット(⌘R)で反映。
    @discardableResult
    func ejectCD() -> Bool {
        requestAutoPause()
        defer { releaseAutoPause() }
        let result = engine.withEmulationLock { mx68k_cd_eject() }
        switch result {
        case 0:
            onCDChanged?("")
            showTransientMessage(String(localized: "CD-ROM image ejected"))
            return true
        case 1:
            onCDChanged?("")
            showTransientMessage(String(localized: "CD-ROM image ejected (takes effect after a hard reset ⌘R)"))
            return true
        case -5:
            showTransientMessage(String(localized:
                "The guest has locked the CD-ROM drive — eject it from the guest first"))
            return false
        default:
            showTransientMessage(String(localized: "CD-ROM eject failed"))
            return false
        }
    }
}
