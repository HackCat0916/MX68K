import SwiftUI

// P692 — ストレージモニタ(Storage Viewer)。SASI 8 ユニットと SCSI ID0-6
// (ID5 は MO、ID6 は CD-ROM も使える固定スロット)の計 17 スロットのマウント
// 状況を 1 画面で見せる。
//
// ★設計方針(Fix Plan「自己反証可能性」節): SCSI 側は「走行中の SPC が実際に
//   開いているもの」(ライブ値、mx68k_scsi_live_*)と「設定値」(config)を必ず
//   両方並べ、片方だけを見せて確定状態であるかのように誤認させない。食い違う
//   場合は「⌘R 待ち」バッジを出す(WindrvSettingsView の wired 値基準 UI ゲート、
//   P457 由来と同じ考え方)。
//
// ★busy ランプはパネル見出しに 1 個だけ。どのスロットが busy かを知る手段は
//   実装に存在しない(Core/Bridge のいずれも単一のグローバル hdd_busy しか
//   持たない)ため、行ごとの busy ランプは置かない — 存在しない情報を作らない。
//   標本はステータスバーの HDD ランプと同一(viewModel.status)なので、両者が
//   ずれることは構造的に起こらない。
struct StorageMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    @EnvironmentObject var configManager: ConfigManager

    var body: some View {
        let ext = configManager.config.extensions
        let status = emulatorViewModel.status
        let storage = emulatorViewModel.storageStatus
        // SCSI が「装着しない」のときは SPC がディスクを開かないので、設定パスが
        // 残っていてもライブ値は永久に空になる。この場合に ⌘R 待ちバッジを出すと
        // 「⌘R を押せば直る」という誤った案内になるため、比較自体を止める。
        let scsiInUse = (ext.scsiMode != "none")

        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text("Storage Status").font(.headline)
                Spacer()
                Text("Busy:")
                Text(status.hdd_busy ? "●" : "○")
                    .foregroundColor(status.hdd_busy ? .red : .gray)
            }
            Divider()

            // ---- SASI (8 units) ----
            Text("SASI (internal, 8 units)").font(.subheadline)
            ForEach(0..<8, id: \.self) { unit in
                sasiRow(unit: unit,
                        inserted: (status.hdd_inserted_mask & UInt8(1 << unit)) != 0,
                        path: ext.hddPath(unit))
            }

            Divider()

            // ---- SCSI (ID 0-6) ----
            HStack(spacing: 8) {
                Text("SCSI (ID0-6)").font(.subheadline)
                Text(scsiModeLabel(ext.scsiMode)).foregroundColor(.secondary)
            }
            ForEach(0..<7, id: \.self) { id in
                scsiRow(id: id,
                        slot: storage.slots.first(where: { $0.id == id }),
                        configuredPath: configuredSCSIPath(id: id, ext: ext),
                        compareEnabled: scsiInUse)
            }
            // 派生値 slots の元になった生値も併記する(どちらか一方だけを見せない)。
            Text(String(format: "attached mask: 0x%02X", storage.attachedMask))
                .foregroundColor(.secondary)
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 560, minHeight: 460, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.storageVisible = true }
        .onDisappear { emulatorViewModel.engine.storageVisible = false }
    }

    // MARK: - SASI

    @ViewBuilder
    private func sasiRow(unit: Int, inserted: Bool, path: String) -> some View {
        HStack(spacing: 8) {
            Text(verbatim: "SASI\(unit)")
                .frame(width: 90, alignment: .leading)
            Text(inserted ? "●" : "○")
                .foregroundColor(inserted ? .green : .gray)
            if inserted {
                Text(fileName(path))
            } else if !path.isEmpty {
                // ★「未装着」と一緒くたにしない。機種が SCSI のときハードリセットで
                //   Config.HDImage[] だけがクリアされ、設定シャドウは保持される
                //   (Bridge 側 s_sasi_hdd_path[] は復元元なのでクリアしない)。
                //   つまり「設定はあるが機種不一致で配線されていない」状態。
                Text(fileName(path)).foregroundColor(.secondary)
                Text("(not wired — machine type mismatch)")
                    .foregroundColor(.orange)
            } else {
                Text("(no image)").foregroundColor(.secondary)
            }
            Spacer()
        }
    }

    // MARK: - SCSI

    @ViewBuilder
    private func scsiRow(id: Int, slot: EmulatorStorageSlot?,
                         configuredPath: String, compareEnabled: Bool) -> some View {
        let kindLabel = slot?.kindLabel ?? "--"
        let attached = slot?.isAttached ?? false
        let ready = slot?.ready ?? false
        let livePath = slot?.path ?? ""
        let pending = compareEnabled && (livePath != configuredPath)

        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 8) {
                Text(verbatim: scsiSlotLabel(id))
                    .frame(width: 90, alignment: .leading)
                Text(attached ? "●" : "○")
                    .foregroundColor(attached ? (ready ? .green : .yellow) : .gray)
                Text(verbatim: kindLabel)
                    .frame(width: 28, alignment: .leading)
                Text(attached ? (ready ? "ready" : "no media") : "--")
                    .foregroundColor(attached ? .primary : .secondary)
                if pending {
                    Text("Pending ⌘R")
                        .foregroundColor(.orange)
                }
                Spacer()
            }
            HStack(spacing: 8) {
                Text("live:")
                    .frame(width: 90, alignment: .trailing)
                    .foregroundColor(.secondary)
                Text(livePath.isEmpty ? String(localized: "(none)") : fileName(livePath))
                    .foregroundColor(.secondary)
                Spacer()
            }
            HStack(spacing: 8) {
                Text("config:")
                    .frame(width: 90, alignment: .trailing)
                    .foregroundColor(.secondary)
                Text(configuredPath.isEmpty ? String(localized: "(none)") : fileName(configuredPath))
                    .foregroundColor(.secondary)
                Spacer()
            }
        }
    }

    /// ID5 は MO、ID6 は CD-ROM も使える固定スロット(SCSI 設定タブの表記に合わせる)。
    private func scsiSlotLabel(_ id: Int) -> String {
        switch id {
        case 5:  return "SCSI5 / MO"
        case 6:  return "SCSI6 / CD"
        default: return "SCSI\(id)"
        }
    }

    /// その ID で実際に装着されるはずの設定パス。
    /// ★HD > MO/CD の優先は飾りではなく SCSI::Construct() 側でも強制される
    ///   (ID5/ID6 に HD パスがあれば MO/CD オブジェクトは装着されない)。
    ///   よって HD パスが非空ならそちらが権威、空のときだけ MO/CD パスを見る。
    private func configuredSCSIPath(id: Int, ext: ExtensionsConfig) -> String {
        let hd: String
        switch id {
        case 0: hd = ext.scsi0Path
        case 1: hd = ext.scsi1Path
        case 2: hd = ext.scsi2Path
        case 3: hd = ext.scsi3Path
        case 4: hd = ext.scsi4Path
        case 5: hd = ext.scsi5Path
        case 6: hd = ext.scsi6Path
        default: hd = ""
        }
        if !hd.isEmpty { return hd }
        if id == 5 { return ext.moPath }
        if id == 6 { return ext.cdPath }
        return ""
    }

    private func scsiModeLabel(_ mode: String) -> String {
        switch mode {
        case "internal": return String(localized: "Internal")
        case "external": return String(localized: "External board (CZ-6BS1)")
        default:         return String(localized: "Not installed")
        }
    }

    private func fileName(_ path: String) -> String {
        (path as NSString).lastPathComponent
    }
}
