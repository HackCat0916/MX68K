import Foundation

struct BIOSConfig: Codable {
    var iplromPath: String = ""
    var cgromPath: String = ""
    var iplrom30Path: String = ""
    var scsiExtRomPath: String = ""   // P241 Stage A: 外付け SCSI(CZ-6BS1)IPL ROM
    var scsiInRomPath: String = ""    // P244: 内蔵 SCSI IPL ROM(Stage 2実装まで未機能)

    init() {}   // init(from:) を書くとメンバワイズ init が消えるため必須

    // P194 教訓: 自動生成の init(from:) はキー欠落で throw し、ConfigManager.load が
    // try? で握り潰して設定全体を初期値にリセットしてしまう。scsiExtRomPath を素追加
    // すると旧 config.json で bios のデコードが keyNotFound を throw するため、全
    // フィールドを decodeIfPresent で明示的に既定値補完する。
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        iplromPath     = try c.decodeIfPresent(String.self, forKey: .iplromPath) ?? ""
        cgromPath      = try c.decodeIfPresent(String.self, forKey: .cgromPath) ?? ""
        iplrom30Path   = try c.decodeIfPresent(String.self, forKey: .iplrom30Path) ?? ""
        scsiExtRomPath = try c.decodeIfPresent(String.self, forKey: .scsiExtRomPath) ?? ""
        scsiInRomPath  = try c.decodeIfPresent(String.self, forKey: .scsiInRomPath) ?? ""
    }
}

struct HardwareConfig: Codable {
    // P221b: 実機ストレージ種の 2 択("SASI" / "SCSI")。既定 = SCSI 搭載機(16MHz)。
    // Codable(init(from:))は非改変 — String プロパティは旧 6 値("X68000_XVI" 等)も
    // そのまま decode でき、読み手側の MachineTypeMigration.normalize が canonical に
    // 写像・次回保存で write-back 自己修復する(P194 教訓: init(from:)自前化は回避)。
    var machineType: String = "SCSI"
    var memoryMB: Int = 2
    var clockMHz: Int = 16
    var fpuEnabled: Bool = false
}

/// P221b: 旧 6 値(モデル名)→ 新 2 値(実機ストレージ種)の移行写像。
/// config.json の `hardware.machineType` を読む 2 箇所(SettingsViewModel.load /
/// EmulatorViewModel.machineTypeValue)で使い、旧保存値でも UI/C 値の両方を解決する。
enum MachineTypeMigration {
    static func normalize(_ raw: String) -> String {
        switch raw {
        case "SASI", "SCSI": return raw
        case "X68000", "X68000_ACE", "X68000_PRO", "X68000_PROII": return "SASI"
        case "X68000_XVI", "X68030": return "SCSI"  // 030 は Phase 5 まで非表示 → 最寄りの SCSI へ
        default: return "SCSI"                        // 未知/欠落 → 既定機種
        }
    }
}

struct DisplayConfig: Codable {
    var mode: String = "windowed"
    var scale: Double = 2.0
    var aspectRatio: String = "4:3"
    var scanline: Bool = false
    var filter: String = "nearest"
}

struct AudioConfig: Codable {
    var enabled: Bool = true
    var volume: Double = 1.0
    var sampleRate: Int = 44100
    // P512: チップ別音量。Core の ADPCM_SetVolume / OPM_SetVolume と同じ 0-16 スケール。
    // 既定値は現状の Bridge 初期化時の挙動と完全一致させてある(ADPCM=15 の固定呼出し、
    // OPM=未呼出し = fmgen 側 db=0 相当のフルボリューム = 16)ため、スライダを一度も
    // 操作しないユーザーの聴感上の挙動は変わらない。
    var adpcmVolume: Int = 15
    var opmVolume: Int = 16

    init() {}   // init(from:) を書くとメンバワイズ init が消えるため必須

    // P512: P194/BIOSConfig と同じ教訓 — 自動生成の init(from:) は新規フィールドを
    // キー欠落として throw し、ConfigManager.load の try? がそれを握り潰して
    // 設定「全体」を初期値にリセットしてしまう。AudioConfig は今までフィールド追加が
    // 無かったため custom init(from:) 未着手だったが、本サイクルで初めて必要になる。
    // 5 フィールド全てを decodeIfPresent で明示的に既定値補完する。
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        enabled     = try c.decodeIfPresent(Bool.self, forKey: .enabled) ?? true
        volume      = try c.decodeIfPresent(Double.self, forKey: .volume) ?? 1.0
        sampleRate  = try c.decodeIfPresent(Int.self, forKey: .sampleRate) ?? 44100
        adpcmVolume = try c.decodeIfPresent(Int.self, forKey: .adpcmVolume) ?? 15
        opmVolume   = try c.decodeIfPresent(Int.self, forKey: .opmVolume) ?? 16
    }
}

struct InputConfig: Codable {
    // P511: キーボードの個別上書き(設定画面「キーの割り当てを変更…」で編集する)。
    // キー = macOS 仮想キーコード(NSEvent.keyCode、UInt16)の 10 進文字列(例 "96")、
    // 値   = 送出先の X68000 スキャンコード(UInt8、0-255)の 10 進文字列(例 "85" = 0x55 = XF1)。
    // 空 dict = 上書きなし(InputManager.setupKeyMapping() の既定割当のまま)。
    // 型・スキーマは P511 以前から不変で、意味づけのみを P511 で与えている。
    var keyboardMap: [String: String] = [:]
    var gamepadEnabled: Bool = true
    var gamepadMap: [String: String] = [:]
    // P194: 入力設定
    var arrowKeysAsNumpad: Bool = false   // カーソルキーをテンキー 8/2/4/6 として送る
    var mouseEnabled: Bool = true         // マウスエミュレーション
    var mouseSensitivity: Double = 1.0    // 0.25〜3.0
    var mouseAbsolute: Bool = true        // P196: 絶対座標追従(ホストカーソルに追従)/ false=相対方式

    init() {}   // init(from:) を書くとメンバワイズ init が消えるため必須

    // P194: 自動生成の init(from:) はプロパティ既定値を使わず decode() するため、
    // 旧 config.json(新キーなし)では keyNotFound を throw する。
    // ConfigManager.load は try? でそれを握り潰し設定ファイル全体を初期値にリセットしてしまうため、
    // decodeIfPresent で明示的に既定値補完する。
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        keyboardMap       = try c.decodeIfPresent([String: String].self, forKey: .keyboardMap) ?? [:]
        gamepadEnabled    = try c.decodeIfPresent(Bool.self, forKey: .gamepadEnabled) ?? true
        gamepadMap        = try c.decodeIfPresent([String: String].self, forKey: .gamepadMap) ?? [:]
        arrowKeysAsNumpad = try c.decodeIfPresent(Bool.self, forKey: .arrowKeysAsNumpad) ?? false
        mouseEnabled      = try c.decodeIfPresent(Bool.self, forKey: .mouseEnabled) ?? true
        let s = try c.decodeIfPresent(Double.self, forKey: .mouseSensitivity) ?? 1.0
        mouseSensitivity  = min(max(s, 0.25), 3.0)
        mouseAbsolute     = try c.decodeIfPresent(Bool.self, forKey: .mouseAbsolute) ?? true
    }
}

struct FDDConfig: Codable {
    var lastFDD0Path: String = ""
    var lastFDD1Path: String = ""
    // P684: FDD 2 台 → 4 台。FD2/FD3 は File メニューからのみ操作でき、
    // ツールバー/ステータスバー/D&D の対象外(ユーザーとの合意)。
    // 前回マウントパスの永続化・次回起動時の自動再マウントは FD0/FD1 と同じ扱い。
    var lastFDD2Path: String = ""
    var lastFDD3Path: String = ""
    // P271: FDライトプロテクト(実機のライトプロテクト爪)。ツールバーの明示トグルで設定。
    var fdd0WriteProtect: Bool = false
    var fdd1WriteProtect: Bool = false
    // P684: FD2/FD3 は南京錠トグルの UI を持たないため、File メニューの
    // 「挿入(書込禁止)…」で挿入した場合のみ true になりうる。
    var fdd2WriteProtect: Bool = false
    var fdd3WriteProtect: Bool = false
    // P557: FD アクセス高速化(XM6「フロッピーディスク高速化」相当)。既定 false =
    // 従来どおりの実時間ウェイト。true で Core の usleep を 64µs 固定へ短縮する。
    var fdFastAccess: Bool = false

    init() {}   // init(from:) を書くとメンバワイズ init が消えるため必須

    // P194 教訓: 自動生成の init(from:) はプロパティ既定値を使わずキー欠落で throw し、
    // ConfigManager.load が try? で握り潰して設定全体を初期値にリセットしてしまう。
    // 新キー(fdd0WriteProtect/fdd1WriteProtect)を含む旧 config.json でも壊れないよう、
    // 既存 2 フィールドも含め全フィールドを decodeIfPresent で明示的に既定値補完する。
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        lastFDD0Path     = try c.decodeIfPresent(String.self, forKey: .lastFDD0Path) ?? ""
        lastFDD1Path     = try c.decodeIfPresent(String.self, forKey: .lastFDD1Path) ?? ""
        fdd0WriteProtect = try c.decodeIfPresent(Bool.self, forKey: .fdd0WriteProtect) ?? false
        fdd1WriteProtect = try c.decodeIfPresent(Bool.self, forKey: .fdd1WriteProtect) ?? false
        // P557: 新キー。旧 config.json(キー無し)でも既定 false へフォールバックする。
        fdFastAccess     = try c.decodeIfPresent(Bool.self, forKey: .fdFastAccess) ?? false
        // P684: 新キー。旧 config.json には存在しないため decodeIfPresent 必須
        // (`?? ""` / `?? false` = 既存ユーザーは FD2/FD3 未マウントのまま、影響ゼロ)。
        lastFDD2Path     = try c.decodeIfPresent(String.self, forKey: .lastFDD2Path) ?? ""
        lastFDD3Path     = try c.decodeIfPresent(String.self, forKey: .lastFDD3Path) ?? ""
        fdd2WriteProtect = try c.decodeIfPresent(Bool.self, forKey: .fdd2WriteProtect) ?? false
        fdd3WriteProtect = try c.decodeIfPresent(Bool.self, forKey: .fdd3WriteProtect) ?? false
    }

    // P684: ドライブ 0..3 のパス / 書込み禁止の分岐を 1 か所へ集約する
    // (ExtensionsConfig.hddPath/setHDDPath と同じ設計方針 — 分岐の書き写しは
    //  片方だけ間違える典型的な事故源)。EmulatorView の onFDDMounted /
    //  onFDDWriteProtectChanged / onFDDEjected の 3 クロージャが共用する。
    // ★Bridge 側の FDD ドライブ数上限(mx68k_fdd_* の `drive > 3` ガード)と
    //   同値でなければならない。
    static let driveCount = 4

    func lastPath(_ drive: Int) -> String {
        switch drive {
        case 0: return lastFDD0Path
        case 1: return lastFDD1Path
        case 2: return lastFDD2Path
        case 3: return lastFDD3Path
        default: return ""
        }
    }

    mutating func setLastPath(_ drive: Int, _ path: String) {
        switch drive {
        case 0: lastFDD0Path = path
        case 1: lastFDD1Path = path
        case 2: lastFDD2Path = path
        case 3: lastFDD3Path = path
        default: break
        }
    }

    func writeProtect(_ drive: Int) -> Bool {
        switch drive {
        case 0: return fdd0WriteProtect
        case 1: return fdd1WriteProtect
        case 2: return fdd2WriteProtect
        case 3: return fdd3WriteProtect
        default: return false
        }
    }

    mutating func setWriteProtect(_ drive: Int, _ protect: Bool) {
        switch drive {
        case 0: fdd0WriteProtect = protect
        case 1: fdd1WriteProtect = protect
        case 2: fdd2WriteProtect = protect
        case 3: fdd3WriteProtect = protect
        default: break
        }
    }
}

struct ExtensionsConfig: Codable {
    var mercuryUnit: Bool = false
    var midiEnabled: Bool = false
    // P686 (D-70): 外付け FDD ユニット(ドライブ 2/3 = C:/D:)の装着。
    // 既定 false = 実機の外付け未接続状態(Human68k に C:/D: は出ない)。
    var externalFDDUnit: Bool = false
    // P490 (MIDI Stage 2): リセット送信 / 音源種別 / 送信遅延 / 入出力デバイス選択。
    var midiResetOnInit: Bool = true
    var midiResetType: Int = 0       // 0=LA, 1=GM, 2=GS, 3=XG
    var midiDelayMs: Int = 0         // 0..1000(Stage 1 の既定 0 を維持)
    var midiOutDeviceIndex: Int = 0
    var midiInDeviceIndex: Int = 0
    // P200: SASI HDD (.hdf) イメージパス。空 = 未装着。論理 unit 0..7 が
    // Bridge で SASI device 0-7(Config.HDImage[unit*2])にマップされる。
    // LUN1(奇数 index)は使わない — Human68k は device ID 単位で probe するため
    // (P200 実測所見)。P455 で 2 → 8 台。
    var hdd0Path: String = ""
    var hdd1Path: String = ""
    var hdd2Path: String = ""
    var hdd3Path: String = ""
    var hdd4Path: String = ""
    var hdd5Path: String = ""
    var hdd6Path: String = ""
    var hdd7Path: String = ""
    // P241 Stage A: 外付け SCSI(CZ-6BS1)イメージパス。空 = 未装着。index = SCSI ID 0..6。
    var scsi0Path: String = ""
    var scsi1Path: String = ""
    var scsi2Path: String = ""
    var scsi3Path: String = ""
    var scsi4Path: String = ""
    var scsi5Path: String = ""
    var scsi6Path: String = ""
    // P668: SCSI MO(光磁気ディスク)イメージパス。空 = 未装着。
    // MO は SCSI ID5 固定の専用スロットで、scsi0Path〜scsi6Path とは
    // 共有されない独立フィールド(内蔵/外付けのどちらのモードでも同じ SPC へ流す)。
    // ID5 に SCSI HD(scsi5Path)が設定済みの場合は HD が優先され MO は装着されない。
    // 反映はハードリセット(⌘R)。
    var moPath: String = ""
    // P676: SCSI CD-ROM(ISO / Mode1)イメージパス。空 = 未装着。
    // CD は SCSI ID6 固定の専用スロットで、scsi0Path〜scsi6Path とは共有されない
    // 独立フィールド(内蔵/外付けのどちらのモードでも同じ SPC へ流す)。
    // ID6 に SCSI HD(scsi6Path)が設定済みの場合は HD が優先され CD は装着されない。
    // 反映はハードリセット(⌘R)。ただし CD ドライブが既に装着済みならライブ交換可。
    // ★スコープ: データ(Mode1)トラックのみ。CD-DA 再生と CD ブートは非対応。
    var cdPath: String = ""
    // P274: SCSI 利用方法。"none"/"internal"/"external"。
    // 内蔵/外付けのディスク一覧を scsi0Path〜scsi6Path で共有し、このモードで
    // どちらの経路へ流すかを分岐する。機種=SCSI では internal、機種=SASI では
    // none/external を選択(UI で機種矛盾の選択肢はグレーアウト、Bridge 二重ゲートで無害化)。
    var scsiMode: String = "none"
    // P450: メモリスイッチ自動更新(XM6「メモリスイッチ自動更新」相当)。
    // ON = ハードリセット時に MX が機種構成へ合わせて SRAM のメモリスイッチ
    //      $ED006F/$ED0070/$ED0071 を更新する(XM6 既定と同じ)。
    // OFF = MX はこの3バイトに一切触れない(ユーザー/ゲストが書いた値を保全)。
    // ★RAM サイズ($ED0008-0B, P220)はこの設定の対象外 — 従来どおり常時反映。
    var memSwitchAutoUpdate: Bool = true
    // P493: 内蔵 SRAM 64KB 化(実機改造相当)。false = 標準 16KB。
    // 上位 48KB($ED4000-$EDFFFF)は Bridge 所有バッファ + sram_ext.dat に永続化される。
    // 反映はハードリセット(⌘R)。
    var sram64kEnabled: Bool = false
    // P642: Windrv(Mac フォルダのホスト共有)。既定 false = 未装着。
    // windrvHostPath が空、またはフォルダが実在しない場合は enabled でも装着されない。
    // ★共有トグル単独では読み取り専用(ゲストからの書込み・作成・削除・改名は不可)。
    // 反映はハードリセット(⌘R)。
    var windrvEnabled: Bool = false
    var windrvHostPath: String = ""
    // P647: Windrv 書込み許可。windrvEnabled とは独立した第 2 のトグルで既定 false。
    // ★false のままなら P642/P643 と完全に同一挙動(読み取り専用)。
    var windrvWriteEnabled: Bool = false

    init() {}   // init(from:) を書くとメンバワイズ init が消えるため必須

    // P194 教訓: 自動生成の init(from:) はプロパティ既定値を使わずキー欠落で throw し、
    // ConfigManager.load が try? で握り潰して設定全体を初期値にリセットしてしまう。
    // 新キー(P200 の hdd0Path/hdd1Path、P455 の hdd2Path〜hdd7Path)を含む
    // 旧 config.json でも壊れないよう decodeIfPresent で補完する。
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        mercuryUnit = try c.decodeIfPresent(Bool.self, forKey: .mercuryUnit) ?? false
        midiEnabled = try c.decodeIfPresent(Bool.self, forKey: .midiEnabled) ?? false
        // P686: 新キー。旧 config.json には存在しないため decodeIfPresent 必須。
        externalFDDUnit = try c.decodeIfPresent(Bool.self, forKey: .externalFDDUnit) ?? false
        // P490: 新キー。既存 config.json(P488 以前)には存在しないため decodeIfPresent 必須。
        midiResetOnInit    = try c.decodeIfPresent(Bool.self, forKey: .midiResetOnInit) ?? true
        midiResetType      = try c.decodeIfPresent(Int.self, forKey: .midiResetType) ?? 0
        midiDelayMs        = try c.decodeIfPresent(Int.self, forKey: .midiDelayMs) ?? 0
        midiOutDeviceIndex = try c.decodeIfPresent(Int.self, forKey: .midiOutDeviceIndex) ?? 0
        midiInDeviceIndex  = try c.decodeIfPresent(Int.self, forKey: .midiInDeviceIndex) ?? 0
        hdd0Path    = try c.decodeIfPresent(String.self, forKey: .hdd0Path) ?? ""
        hdd1Path    = try c.decodeIfPresent(String.self, forKey: .hdd1Path) ?? ""
        hdd2Path    = try c.decodeIfPresent(String.self, forKey: .hdd2Path) ?? ""
        hdd3Path    = try c.decodeIfPresent(String.self, forKey: .hdd3Path) ?? ""
        hdd4Path    = try c.decodeIfPresent(String.self, forKey: .hdd4Path) ?? ""
        hdd5Path    = try c.decodeIfPresent(String.self, forKey: .hdd5Path) ?? ""
        hdd6Path    = try c.decodeIfPresent(String.self, forKey: .hdd6Path) ?? ""
        hdd7Path    = try c.decodeIfPresent(String.self, forKey: .hdd7Path) ?? ""
        scsi0Path   = try c.decodeIfPresent(String.self, forKey: .scsi0Path) ?? ""
        scsi1Path   = try c.decodeIfPresent(String.self, forKey: .scsi1Path) ?? ""
        scsi2Path   = try c.decodeIfPresent(String.self, forKey: .scsi2Path) ?? ""
        scsi3Path   = try c.decodeIfPresent(String.self, forKey: .scsi3Path) ?? ""
        scsi4Path   = try c.decodeIfPresent(String.self, forKey: .scsi4Path) ?? ""
        scsi5Path   = try c.decodeIfPresent(String.self, forKey: .scsi5Path) ?? ""
        scsi6Path   = try c.decodeIfPresent(String.self, forKey: .scsi6Path) ?? ""
        // P668: 新キー。既存 config.json には存在しないため decodeIfPresent 必須。
        moPath      = try c.decodeIfPresent(String.self, forKey: .moPath) ?? ""
        // P676: 新キー。既存 config.json には存在しないため decodeIfPresent 必須。
        cdPath      = try c.decodeIfPresent(String.self, forKey: .cdPath) ?? ""
        scsiMode    = try c.decodeIfPresent(String.self, forKey: .scsiMode) ?? "none"
        // P450: `?? true` により既存ユーザーの config.json でも既定 ON(XM6 既定と同じ)。
        memSwitchAutoUpdate =
            try c.decodeIfPresent(Bool.self, forKey: .memSwitchAutoUpdate) ?? true
        // P493: 新キー。既存 config.json には存在しないため decodeIfPresent 必須。
        // `?? false` = 既存ユーザーは標準 16KB のまま(影響ゼロ)。
        sram64kEnabled = try c.decodeIfPresent(Bool.self, forKey: .sram64kEnabled) ?? false
        // P642: 新キー。既存 config.json には存在しないため decodeIfPresent 必須。
        // `?? false` / `?? ""` = 既存ユーザーは Windrv 未装着のまま(影響ゼロ)。
        windrvEnabled  = try c.decodeIfPresent(Bool.self, forKey: .windrvEnabled) ?? false
        windrvHostPath = try c.decodeIfPresent(String.self, forKey: .windrvHostPath) ?? ""
        // P647: 新キー。既存 config.json には存在しないため `?? false` で
        // 書込み禁止(= P642/P643 と同一の読み取り専用)へ落ちる。
        windrvWriteEnabled =
            try c.decodeIfPresent(Bool.self, forKey: .windrvWriteEnabled) ?? false
    }

    // P455: SASI 論理 unit 0..7 のパス。分岐を 1 か所に集約する
    // (SASISettingsView / EmulatorViewModel / EmulatorView.onHDDChanged の
    //  3 サイトが共用。分岐の書き写しは片方だけ間違える典型的な事故源)。
    // ★Bridge の MX68K_SASI_UNIT_COUNT と同値でなければならない。
    static let hddUnitCount = 8

    func hddPath(_ unit: Int) -> String {
        switch unit {
        case 0: return hdd0Path
        case 1: return hdd1Path
        case 2: return hdd2Path
        case 3: return hdd3Path
        case 4: return hdd4Path
        case 5: return hdd5Path
        case 6: return hdd6Path
        case 7: return hdd7Path
        default: return ""
        }
    }

    mutating func setHDDPath(_ unit: Int, _ path: String) {
        switch unit {
        case 0: hdd0Path = path
        case 1: hdd1Path = path
        case 2: hdd2Path = path
        case 3: hdd3Path = path
        case 4: hdd4Path = path
        case 5: hdd5Path = path
        case 6: hdd6Path = path
        case 7: hdd7Path = path
        default: break
        }
    }
}

/// P556 — 「ノーウェイト」(倍率上限なし、ホスト CPU が許す限り高速に実行)を表す
/// `turboTargetMultiplier` のセンチネル値。2〜5 の通常倍率と同じ Int 型の変数へ
/// 収める設計にしたため、範囲外の特殊値であることが一目で分かる名前を与えておく。
/// この値のときだけ `EmulatorEngine` の専用バックグラウンドスレッド駆動へ切り替わる。
let kTurboNoWaitMultiplier = -1

/// P556 — 目標倍率のバリデーションを 1 箇所へ集約する。許可する値は
/// `kTurboNoWaitMultiplier`(= -1、ノーウェイト)と 2〜5 のみで、それ以外
/// (0 / 1 / 6 以上 / -2 以下等)は 2〜5 へクランプする。
///
/// ★P556 でこのヘルパーを新設した理由: P555 時点でクランプ処理が
/// `PerformanceConfig.init(from:)` / `EmulatorViewModel.pushConfig` /
/// `EmulatorViewModel.setTurboTargetMultiplier` の **3 箇所**に散在しており、
/// センチネル値 -1 を許可する改修で 1 箇所でも漏らすと「Picker でノーウェイトを
/// 選んだ瞬間に 2 へ引き戻される」= 機能そのものが成立しない、という壊れ方を
/// する。将来 4 箇所目を足す際の見落としも同じ形で防ぐため、`private` ではなく
/// (別ファイルの `EmulatorViewModel` から呼ぶ必要があるため)`internal` で公開する。
func clampTurboMultiplier(_ v: Int) -> Int {
    if v == kTurboNoWaitMultiplier { return kTurboNoWaitMultiplier }
    return min(max(v, 2), 5)
}

/// P555 — 実行速度(ターボ)関連の設定。`turboTargetMultiplier` は「ターボを ON に
/// したときに使う倍率」であり、ターボ ON/OFF 自体は永続化しない(起動時は常に OFF)。
///
/// ★このサイクルで `HardwareConfig` へ相乗りせず新規 struct にしたのは、
/// `HardwareConfig` が custom `init(from:)` を持たない(合成デコーダのみ)ため、
/// フィールドを素追加すると旧 config.json のデコードが keyNotFound で throw し、
/// `ConfigManager.load` の `try?` がそれを握り潰して設定「全体」が初期値へ
/// リセットされる P194 型の罠を踏むため。新規 struct なら最初から
/// decodeIfPresent 方式で書けるので、その懸念自体が発生しない。
struct PerformanceConfig: Codable {
    // P556: 2〜5、または kTurboNoWaitMultiplier(= -1、ノーウェイト)。
    // 1 = ターボ OFF は「目標倍率」としては選べない(OFF は isTurboActive 側で表す)。
    var turboTargetMultiplier: Int = 3

    // P753: Release ビルドでの debug_log() 有効/無効の永続設定。
    // Debug ビルドはこの値を無視し、常に Bridge 側のコンパイル時既定(ON)を使う
    // (判定は mx68k_is_debug_build() で Bridge 側に一本化してある)。
    var debugLogEnabled: Bool = false

    init() {}   // init(from:) を書くとメンバワイズ init が消えるため必須

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let m = try c.decodeIfPresent(Int.self, forKey: .turboTargetMultiplier) ?? 3
        // InputConfig.mouseSensitivity と同じく、手書き編集された config.json でも
        // UI の選択肢(2x〜5x + ノーウェイト)の範囲外に出ないようクランプする。
        // P556: クランプは共通ヘルパーへ集約(3 箇所の実装ずれを防ぐ)。
        turboTargetMultiplier = clampTurboMultiplier(m)
        debugLogEnabled = try c.decodeIfPresent(Bool.self, forKey: .debugLogEnabled) ?? false
    }
}

struct EmulatorConfig: Codable {
    var version: Int = 1
    var bios: BIOSConfig = BIOSConfig()
    var hardware: HardwareConfig = HardwareConfig()
    var display: DisplayConfig = DisplayConfig()
    var audio: AudioConfig = AudioConfig()
    var input: InputConfig = InputConfig()
    var fdd: FDDConfig = FDDConfig()
    var extensions: ExtensionsConfig = ExtensionsConfig()
    var performance: PerformanceConfig = PerformanceConfig()   // P555

    init() {}   // init(from:) を書くとメンバワイズ init が消えるため必須

    /// P555 — トップレベルへの新規キー追加(`performance`)はこのサイクルが初。
    /// P194 の教訓はネスト struct だけでなくこの親 struct にも同じ形で当てはまる:
    /// 合成デコーダのままだと、`PerformanceConfig` 側の decodeIfPresent ロジックへ
    /// 到達する前に `EmulatorConfig` 自身が `keyNotFound(performance)` で throw し、
    /// `ConfigManager.load`(ConfigManager.swift:24)の `try?` がそれを握り潰して
    /// 設定「全体」がデフォルトへリセットされる。全フィールドを decodeIfPresent で
    /// 受け、どのキーが欠落していても安全にデフォルトへフォールバックさせる。
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version     = try c.decodeIfPresent(Int.self, forKey: .version) ?? 1
        bios        = try c.decodeIfPresent(BIOSConfig.self, forKey: .bios) ?? BIOSConfig()
        hardware    = try c.decodeIfPresent(HardwareConfig.self, forKey: .hardware) ?? HardwareConfig()
        display     = try c.decodeIfPresent(DisplayConfig.self, forKey: .display) ?? DisplayConfig()
        audio       = try c.decodeIfPresent(AudioConfig.self, forKey: .audio) ?? AudioConfig()
        input       = try c.decodeIfPresent(InputConfig.self, forKey: .input) ?? InputConfig()
        fdd         = try c.decodeIfPresent(FDDConfig.self, forKey: .fdd) ?? FDDConfig()
        extensions  = try c.decodeIfPresent(ExtensionsConfig.self, forKey: .extensions) ?? ExtensionsConfig()
        performance = try c.decodeIfPresent(PerformanceConfig.self, forKey: .performance) ?? PerformanceConfig()
    }
}
