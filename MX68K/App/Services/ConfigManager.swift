import Foundation
import Combine

class ConfigManager: ObservableObject {
    @Published var config: EmulatorConfig

    private let configURL: URL
    private let filename = "config.json"

    init() {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let dir = appSupport.appendingPathComponent("MX68K", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        configURL = dir.appendingPathComponent(filename)
        config = ConfigManager.load(from: configURL)
    }

    private static func load(from url: URL) -> EmulatorConfig {
        guard let data = try? Data(contentsOf: url) else {
            var cfg = EmulatorConfig()
            cfg.bios = autodetectBIOS()
            return cfg
        }
        guard let decoded = try? JSONDecoder().decode(EmulatorConfig.self, from: data) else {
            var cfg = EmulatorConfig()
            cfg.bios = autodetectBIOS()
            return cfg
        }
        var cfg = decoded
        if cfg.bios.iplromPath.isEmpty || cfg.bios.cgromPath.isEmpty {
            cfg.bios = autodetectBIOS()
        }
        // P274: バージョンベースの一回性移行(version<3 の単一ゲート)。
        // P273 の version<2 ブロックはこのサイクルで scsiExternalEnabled フィールド
        // 自体を削除するため共存できず(ビルドが壊れる)、version<3 へ統合した。
        // 旧フィールド(bios.scsiInDisk0Path / extensions.scsiExternalEnabled)は
        // 新 struct 定義から消えており decode 後の cfg からは復元できないため、
        // 生 JSON を再パースして読み取る。version ゲートで一回性を保証しており、
        // 値の非空判定だけに依存しない(P273 の恒久トラップは再発しない)。
        if cfg.version < 3 {
            let raw = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
            let rawBios = raw?["bios"] as? [String: Any]
            let rawExt  = raw?["extensions"] as? [String: Any]
            let oldInternalPath = (rawBios?["scsiInDisk0Path"] as? String) ?? ""
            let isSCSIMachine = MachineTypeMigration.normalize(cfg.hardware.machineType) == "SCSI"

            if isSCSIMachine {
                // 「内蔵」だったかは bios.scsiInDisk0Path の非空性のみで判定する。
                // extensions.scsi0Path は外付け専用フィールドであり、機種往復履歴の
                // あるユーザーでは非空の場合があるため判定根拠にしない(Code Review指摘)。
                if cfg.extensions.scsi0Path.isEmpty && !oldInternalPath.isEmpty {
                    cfg.extensions.scsi0Path = oldInternalPath
                }
                cfg.extensions.scsiMode = oldInternalPath.isEmpty ? "none" : "internal"
            } else {
                if let wasEnabled = rawExt?["scsiExternalEnabled"] as? Bool {
                    cfg.extensions.scsiMode = wasEnabled ? "external" : "none"
                } else {
                    // P273より前の極めて古いconfig(scsiExternalEnabledキー自体が
                    // 存在しない) — P273自身が採用したのと同じヒューリスティックへ
                    // フォールバック(既存scsi0-6Pathの非空性から推定)。
                    let hadExternalScsi = [
                        cfg.extensions.scsi0Path, cfg.extensions.scsi1Path, cfg.extensions.scsi2Path,
                        cfg.extensions.scsi3Path, cfg.extensions.scsi4Path, cfg.extensions.scsi5Path,
                        cfg.extensions.scsi6Path,
                    ].contains { !$0.isEmpty }
                    cfg.extensions.scsiMode = hadExternalScsi ? "external" : "none"
                }
            }
            cfg.version = 3
            if let data = try? JSONEncoder().encode(cfg) {
                try? data.write(to: url, options: .atomic)   // save()と同じエンコード方式、ただちに書き戻し
            }
        }
        return cfg
    }

    private static func autodetectBIOS() -> BIOSConfig {
        let fm = FileManager.default
        let home = NSHomeDirectory()

        // Default BIOS directory: ~/Library/Application Support/MX68K/bios/
        let biosDir = "\(home)/Library/Application Support/MX68K/bios"
        try? fm.createDirectory(atPath: biosDir, withIntermediateDirectories: true)

        var bios = BIOSConfig()
        let iplPath = (biosDir as NSString).appendingPathComponent("IPLROM.DAT")
        let cgPath = (biosDir as NSString).appendingPathComponent("CGROM.DAT")
        let ipl30Path = (biosDir as NSString).appendingPathComponent("IPLROM30.DAT")
        if fm.fileExists(atPath: iplPath) {
            bios.iplromPath = iplPath
        }
        if fm.fileExists(atPath: cgPath) {
            bios.cgromPath = cgPath
        }
        if fm.fileExists(atPath: ipl30Path) {
            bios.iplrom30Path = ipl30Path
        }
        // P241 Stage A: 外付け SCSI(CZ-6BS1)IPL ROM。任意 — 無ければ外付け SCSI 起動のみ無効。
        let scsiExtPath = (biosDir as NSString).appendingPathComponent("SCSIEXROM.DAT")
        if fm.fileExists(atPath: scsiExtPath) {
            bios.scsiExtRomPath = scsiExtPath
        }
        // P244: 内蔵 SCSI IPL ROM。Stage 2 実装まで未機能だが、配置されていれば先行して
        // 検出・保存し、実装時にそのまま使えるようにする。
        let scsiInPath = (biosDir as NSString).appendingPathComponent("SCSIINROM.DAT")
        if fm.fileExists(atPath: scsiInPath) {
            bios.scsiInRomPath = scsiInPath
        }
        return bios
    }

    func save() {
        do {
            let data = try JSONEncoder().encode(config)
            try data.write(to: configURL, options: .atomic)
        } catch {
            print("Failed to save config: \(error)")
        }
    }

    func applySettings(from settingsViewModel: SettingsViewModel) {
        var newConfig = config
        settingsViewModel.apply(to: &newConfig)
        config = newConfig
        save()
    }
}
