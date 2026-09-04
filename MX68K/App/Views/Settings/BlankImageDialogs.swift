import SwiftUI
import UniformTypeIdentifiers

// P578 (Docs/10 A-1 / C-1 / C-2 の UI 再設計) — 空(未フォーマット)ディスクイメージ
// 作成の専用ダイアログ。XM6 の「ツール」→「イメージ作成」に倣い、種別ごとに
// 独立したダイアログ(保存先 + 種別固有のオプション)を開く。
//
// 生成ロジックそのものは P574 の `BlankImageService`(UI 非依存の純 static 関数)を
// そのまま呼ぶだけで、API は一切変更していない。Core/Bridge も無関係。
//
// ★呼び出し元は 2 系統ある(Tools メニュー / SASI・SCSI 設定タブのボタン)が、
//   **ダイアログの View 構造体はこの 1 組だけ**にして二重保守を避ける。
//   一方で `.sheet` 修飾子とその表示状態は呼び出し元ごとに**完全に独立**させること —
//   同一ビューへ `.sheet` を 2 つ付けると片方しか機能しない、という制約を
//   本プロジェクトは既に実地で踏んでいる(`EmulatorView.swift:123-124` のコメント)。
//   ここでは Tools メニュー側 = `BlankImagePresentation.shared`(RootView が提示)、
//   設定タブ側 = 各タブのローカルな `@State`(そのタブの Form が提示)と分けている。

/// Tools メニューから開くダイアログの種別(`.sheet(item:)` 用)。
enum BlankImageKind: Int, Identifiable {
    case floppy
    case sasi
    case scsi
    case mo

    var id: Int { rawValue }
}

/// P578 — Tools メニュー(`ToolsCommands`)と `RootView` の `.sheet(item:)` を繋ぐ
/// 最小の観測対象。
///
/// ★D-49/P563 規律: `Commands` 構造体は `ObservableObject` を
/// `@ObservedObject`/`@StateObject` で**観測してはならない**(観測すると
/// `App.body` = `.commands{}` 全体が再評価され、開いていたサブメニューが閉じる)。
/// `ToolsCommands` は本オブジェクトを観測せず `.shared` へ書き込むだけで、
/// 観測するのは `RootView` 側だけ。更新頻度が極めて低い専用オブジェクトである点も
/// 含め、`EmulatorRunState`(P546)/ `MouseCaptureState`(P237)と同型。
final class BlankImagePresentation: ObservableObject {
    static let shared = BlankImagePresentation()
    private init() {}

    @Published var kind: BlankImageKind? = nil
}

/// 種別で 4 つのダイアログを出し分けるだけの薄いラッパー(Tools メニュー経路用)。
struct BlankImageDialog: View {
    let kind: BlankImageKind

    var body: some View {
        switch kind {
        case .floppy: BlankFDImageDialog()
        case .sasi:   BlankSASIImageDialog()
        case .scsi:   BlankSCSIImageDialog()
        case .mo:     BlankMOImageDialog()
        }
    }
}

// MARK: - フロッピーディスク(A-1)

/// 空の 2HD FD イメージ(raw/XDF、1,261,568B、0xE5 埋め)を作成する。
/// P574 では作成とマウントを分離していたが、XM6 式のダイアログでは
/// 「作成後にどのドライブへ入れるか」をこの場で選べるようにする(既定は挿入しない)。
struct BlankFDImageDialog: View {
    @EnvironmentObject private var emulatorViewModel: EmulatorViewModel
    @Environment(\.dismiss) private var dismiss

    /// 作成後に挿入するドライブ。-1 = 挿入しない(既定 = P574 までの挙動)。
    @State private var mountDrive: Int = -1
    @State private var resultMessage: String? = nil
    @State private var resultIsError = false

    var body: some View {
        BlankImageDialogFrame(title: "Create Floppy Disk Image",
                              resultMessage: resultMessage,
                              resultIsError: resultIsError,
                              onCreate: create) {
            Text("Creates an unformatted (physical-format-only) 2HD floppy image (1,261,568 bytes). Format it with FORMAT.X on the guest before use.")
                .font(.subheadline).foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Picker("After creating", selection: $mountDrive) {
                Text("Do not insert").tag(-1)
                Text("Insert into FDD0").tag(0)
                Text("Insert into FDD1").tag(1)
            }
            .pickerStyle(.radioGroup)
        }
    }

    private func create() {
        // P475 と同型: パネル(runModal)表示中は CVDisplayLink が走り続けるため
        // 自動一時停止し、OK/Cancel いずれの経路でも defer で確実に再開する。
        emulatorViewModel.requestAutoPause()
        defer { emulatorViewModel.releaseAutoPause() }

        let panel = NSSavePanel()
        panel.nameFieldStringValue = "blank.xdf"
        if let xdfType = UTType(filenameExtension: "xdf") {
            panel.allowedContentTypes = [xdfType]
        }
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try BlankImageService.createBlankFD(at: url)
            // 作成に成功した場合のみ挿入する(失敗時に空振りマウントしない)。
            if mountDrive == 0 || mountDrive == 1 {
                // ライトプロテクトは既存のドライブ別トグルに従う(通常のマウント経路と同条件)。
                // P599: zip 由来の一時的な書込み禁止が残っていれば先に復元してから読む。
                let writeProtect = emulatorViewModel.resolvedWriteProtectForNewMount(drive: mountDrive)
                emulatorViewModel.mountFDD(drive: mountDrive,
                                           path: url.path,
                                           writeProtect: writeProtect)
            }
            resultIsError = false
            resultMessage = String(localized: "Blank image created: \(url.lastPathComponent)")
        } catch {
            resultIsError = true
            resultMessage = String(localized: "Failed to create the blank disk image.")
        }
    }
}

// MARK: - SASI ハードディスク(C-1)

/// 空の SASI HDD イメージを作成する。サイズは実機 SASI の 3 容量(10/20/40MB)のみ
/// — それ以外は Bridge の既存検証(P458)がマウントを拒否するため、自由入力にしない。
struct BlankSASIImageDialog: View {
    @EnvironmentObject private var emulatorViewModel: EmulatorViewModel

    @State private var sizeBytes: Int = BlankImageService.sasi10MB
    @State private var resultMessage: String? = nil
    @State private var resultIsError = false

    var body: some View {
        BlankImageDialogFrame(title: "Create SASI Hard Disk Image",
                              resultMessage: resultMessage,
                              resultIsError: resultIsError,
                              onCreate: create) {
            // P579 — 順序を是正: マウントしていないとゲスト側がドライブを認識せず
            // FORMAT.X でフォーマットできない(ユーザーの実機操作報告)。
            Text("Creates an unformatted (physical-format-only) SASI hard disk image. Mount it from the SASI settings tab, then format it with FORMAT.X on the guest.")
                .font(.subheadline).foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Picker("Size", selection: $sizeBytes) {
                Text(verbatim: "10 MB").tag(BlankImageService.sasi10MB)
                Text(verbatim: "20 MB").tag(BlankImageService.sasi20MB)
                Text(verbatim: "40 MB").tag(BlankImageService.sasi40MB)
            }
            .pickerStyle(.radioGroup)
        }
    }

    private func create() {
        emulatorViewModel.requestAutoPause()
        defer { emulatorViewModel.releaseAutoPause() }

        let panel = NSSavePanel()
        panel.nameFieldStringValue = "blank.hdf"
        if let hdfType = UTType(filenameExtension: "hdf") {
            panel.allowedContentTypes = [hdfType]
        }
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try BlankImageService.createBlankSASI(at: url, sizeBytes: sizeBytes)
            resultIsError = false
            resultMessage = String(localized: "Blank image created: \(url.lastPathComponent)")
        } catch {
            resultIsError = true
            resultMessage = String(localized: "Failed to create the blank disk image.")
        }
    }
}

// MARK: - SCSI ハードディスク(C-2)

/// 空の外付け/内蔵 SCSI HDD イメージを作成する。サイズは MB 単位入力(XM6 と同じ流儀)、
/// 範囲は Bridge の既存検証(P458)と同じ 10〜4095MB。
struct BlankSCSIImageDialog: View {
    @EnvironmentObject private var emulatorViewModel: EmulatorViewModel

    @State private var sizeMB: Int = 100
    @State private var resultMessage: String? = nil
    @State private var resultIsError = false

    /// MB 単位入力の下限/上限。`BlankImageService.scsiMinSize/scsiMaxSize`
    /// (= Bridge の `P458_SCSI_MIN`/`P458_SCSI_MAX`)から導出する。
    /// 下限は 10,441,728B = 9.96MiB のため、MiB 単位では切り上げて 10MB とする
    /// (10MB = 10,485,760B ≧ P458_SCSI_MIN、512B の倍数でもある)。
    private static let minMB = 10
    private static let maxMB = BlankImageService.scsiMaxSize / (1024 * 1024)   // 4095

    var body: some View {
        BlankImageDialogFrame(title: "Create SCSI Hard Disk Image",
                              resultMessage: resultMessage,
                              resultIsError: resultIsError,
                              onCreate: create) {
            // P579 — 順序を是正: マウントしていないとゲスト側がドライブを認識せず
            // SXFORMAT でフォーマットできない(ユーザーの実機操作報告)。
            Text("Creates an unformatted (physical-format-only) SCSI hard disk image, 10–4095 MB. Mount it from the SCSI settings tab, then format it with SXFORMAT on the guest.")
                .font(.subheadline).foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack {
                Text("Size (MB)")
                TextField("", value: $sizeMB, format: .number)
                    .frame(width: 80)
                    .multilineTextAlignment(.trailing)
                Stepper("", value: $sizeMB, in: Self.minMB...Self.maxMB, step: 10)
                    .labelsHidden()
                Spacer()
            }
        }
    }

    private func create() {
        // TextField へ直接打ち込まれた範囲外の値もここで弾く(Stepper だけでは
        // 範囲を保証できない)。パネルを開く**前に**チェックする = 早期エラー。
        guard sizeMB >= Self.minMB, sizeMB <= Self.maxMB else {
            resultIsError = true
            resultMessage = String(localized: "Size must be between 10 and 4095 MB.")
            return
        }
        let sizeBytes = sizeMB * 1_048_576
        guard sizeBytes >= BlankImageService.scsiMinSize,
              sizeBytes <= BlankImageService.scsiMaxSize,
              sizeBytes % 512 == 0 else {
            resultIsError = true
            resultMessage = String(localized: "Size must be between 10 and 4095 MB.")
            return
        }

        emulatorViewModel.requestAutoPause()
        defer { emulatorViewModel.releaseAutoPause() }

        let panel = NSSavePanel()
        panel.nameFieldStringValue = "blank.hds"
        if let hdsType = UTType(filenameExtension: "hds") {
            panel.allowedContentTypes = [hdsType]
        }
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try BlankImageService.createBlankSCSI(at: url, sizeBytes: sizeBytes)
            resultIsError = false
            resultMessage = String(localized: "Blank image created: \(url.lastPathComponent)")
        } catch {
            resultIsError = true
            resultMessage = String(localized: "Failed to create the blank disk image.")
        }
    }
}

// MARK: - MO 光磁気ディスク(C-4)

/// 空の SCSI MO イメージを作成する。サイズは実機 MO の 4 容量(128/230/540/640MB)のみ
/// — `SCSIMO::Open`(`XM6:vm/disk.cpp:2131-2187` からの移植)がファイルサイズの
/// **完全一致**でのみ受理するため、自由入力にしない。
struct BlankMOImageDialog: View {
    @EnvironmentObject private var emulatorViewModel: EmulatorViewModel

    @State private var sizeBytes: Int = BlankImageService.mo230MB
    /// P672 — 論理フォーマット。既定は `.none`(P578 までの挙動を維持)。
    @State private var logicalFormat: MOLogicalFormat = .none
    /// P674 — 作成後に MO スロット(SCSI ID5)へ装着するか。既定は OFF
    /// (= P673 までの挙動を維持)。MO は ID5 単一スロットのため
    /// `BlankFDImageDialog` と違ってドライブ選択は不要。
    @State private var mountAfterCreating = false
    @State private var resultMessage: String? = nil
    @State private var resultIsError = false

    var body: some View {
        BlankImageDialogFrame(title: "Create MO Disk Image",
                              resultMessage: resultMessage,
                              resultIsError: resultIsError,
                              onCreate: create) {
            Text("Creates an MO disk image (128 / 230 / 540 / 640 MB — only these exact sizes are recognized). Mount it in the SCSI settings tab. With “None” the image is unformatted, so format it on the guest with an MO formatter (e.g. FIM.X). With “IBM format” a FAT16 super-floppy filesystem is written, so the image can be used as-is without formatting on the guest — useful for 640 MB, which the guest-side MO formatter does not support. The SHARP (bootable) format is not supported.")
                .font(.subheadline).foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Picker("Size", selection: $sizeBytes) {
                Text(verbatim: "128 MB").tag(BlankImageService.mo128MB)
                Text(verbatim: "230 MB").tag(BlankImageService.mo230MB)
                Text(verbatim: "540 MB").tag(BlankImageService.mo540MB)
                Text(verbatim: "640 MB").tag(BlankImageService.mo640MB)
            }
            .pickerStyle(.radioGroup)

            Picker("Logical format", selection: $logicalFormat) {
                Text("None").tag(MOLogicalFormat.none)
                Text("IBM format").tag(MOLogicalFormat.ibm)
            }
            .pickerStyle(.radioGroup)

            Toggle("Insert after creating", isOn: $mountAfterCreating)
        }
    }

    private func create() {
        emulatorViewModel.requestAutoPause()
        defer { emulatorViewModel.releaseAutoPause() }

        let panel = NSSavePanel()
        panel.nameFieldStringValue = "blank.mos"
        if let mosType = UTType(filenameExtension: "mos") {
            panel.allowedContentTypes = [mosType]
        }
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try BlankImageService.createBlankMO(at: url, sizeBytes: sizeBytes, format: logicalFormat)
            // 作成に成功した場合のみ装着する(失敗時に空振りマウントしない)。
            // P674 でライブ配線が入ったため、insertMO 側が「即座に反映」/
            // 「次回⌘Rで反映」のいずれか適切なメッセージを自動的に出す。
            if mountAfterCreating {
                emulatorViewModel.insertMO(url: url)
            }
            resultIsError = false
            resultMessage = String(localized: "Blank image created: \(url.lastPathComponent)")
        } catch {
            resultIsError = true
            resultMessage = String(localized: "Failed to create the blank disk image.")
        }
    }
}

// MARK: - 共通の枠

/// 4 種のダイアログで共通の見た目(見出し / 本文 / 結果メッセージ / ボタン行)。
/// 作成後もダイアログは閉じず結果メッセージをその場に出す — 連続で複数枚
/// 作りたい場合にダイアログを開き直さずに済むため(閉じるのは「閉じる」ボタン)。
private struct BlankImageDialogFrame<Content: View>: View {
    let title: LocalizedStringKey
    let resultMessage: String?
    let resultIsError: Bool
    let onCreate: () -> Void
    @ViewBuilder let content: Content

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title)
                .font(.headline)

            content

            if let message = resultMessage {
                Text(message)
                    .font(.subheadline)
                    .foregroundColor(resultIsError ? .red : .secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack {
                Spacer()
                Button("Close") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Create…") { onCreate() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 460)
    }
}
