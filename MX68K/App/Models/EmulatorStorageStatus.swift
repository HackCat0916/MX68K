import Foundation

/// P692 — ストレージモニタ(Storage Viewer)用の SCSI ライブ状態。
///
/// 走行中の SPC インスタンス自身(`mx68k_scsi_live_*`)から読んだ値だけを持つ純
/// Swift 型。設定側の値(`config.extensions.scsiNPath` / `moPath` / `cdPath`)は
/// ここには入れない —— モニタ側で「ライブ値」と「設定値」を必ず並べて表示し、
/// 食い違うときに「⌘R 待ち」を明示するため、両者は別経路のまま保つ。
///
/// SASI(8 ユニット)と busy ランプはこの型に含めない。既存の
/// `EmulatorViewModel.status`(`hdd_inserted_mask` / `hdd_busy`)がそのまま
/// 真実源であり、ステータスバーの HDD ランプと同じ標本を共有させることで
/// 「モニタとステータスバーで表示がずれる」余地を構造的に無くしている。
struct EmulatorStorageSlot: Identifiable {
    /// SCSI ID 0..6。
    var id: Int
    /// 0=HD / 1=MO / 2=CD / -1=未装着(`mx68k_scsi_live_kind()` の生値)。
    var kind: Int
    /// メディア有り(`Disk::IsReady()`)。未装着なら false。
    var ready: Bool
    /// 実際に開かれているイメージのパス。未装着なら空文字。
    var path: String

    var isAttached: Bool { kind >= 0 }

    /// 種別の表示名。生値 `kind` をそのまま持っているので、この派生値が
    /// 誤っていても元の値は失われない。
    var kindLabel: String {
        switch kind {
        case 0:  return "HD"
        case 1:  return "MO"
        case 2:  return "CD"
        default: return "--"
        }
    }
}

struct EmulatorStorageStatus {
    /// bit id = SCSI ID id に何かが装着されている(`mx68k_scsi_live_attached_mask()`)。
    /// `slots` の派生元となる生値。両方保持するのは、派生値だけを見せない方針
    /// (自己反証可能性)による。
    var attachedMask: UInt32 = 0
    /// SCSI ID 0..6 の 7 件(常に 7 件、未装着 ID も欠落させない)。
    var slots: [EmulatorStorageSlot] = []
}
