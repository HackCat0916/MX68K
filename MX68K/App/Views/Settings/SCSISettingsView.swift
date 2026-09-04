import SwiftUI
import UniformTypeIdentifiers

// P274: SCSI 設定タブ。XM6 準拠で内蔵/外付け SCSI を 1 画面に集約。
// 「装着しない/内蔵タイプ/外付けボード」の 3 択と、ID0-6 の共有ディスク一覧を持つ。
// SCSIINROM/SCSIEXROM(BIOS)は BIOSSettingsView へ移設(機種非依存で常時表示)。
//
// 機種による選択可否:
//   - 機種=SCSI: 「内蔵タイプ」のみ選択可(「装着しない」「外付け」は無効)。
//   - 機種=SASI: 「装着しない」「外付けボード」のみ選択可(「内蔵タイプ」は無効)。
// 共有ディスク一覧(scsi0Path〜scsi6Path)は内蔵/外付けで同じフィールドを使い、
// scsiMode でどちらの経路へ流すかを分岐する:
//   - external: emulatorViewModel.insertSCSI/ejectSCSI(ライブ反映)。
//   - internal: config へ直接書込み+save()(ライブ反映なし、次回ハードリセットで反映)。
//
// P442 (D-8): ID0-6 の各行を個別のドロップターゲットにし、ディスクイメージを
// 直接ドラッグ&ドロップでマウントできるようにする。受理条件は `Select…`
// (browseDiskImage の UTType.data 許容)と揃え、拡張子では絞らない。
//
// P712: iOS へ移植。macOS 側の実効コードパスは変更せず、`#if os(iOS)` 分岐を
// 追加するだけ。iOS はコピーインではなくセキュリティスコープブックマーク方式
// (`DiskBookmarkStore`)を使う。内蔵/外付けの分岐
// (internal = config 書込みのみ・次回ハードリセットで反映 / external = ライブ反映)は
// macOS とまったく同じ意味論を保つ。P442 の D&D は iOS へ移植しない。
// P712c: 当初は行ごとに `DiskPickerButton`(専用 fileImporter 持ち)を置いていたが、
// 同一 Form 内に 9 個(SCSI7+MO+CD)の .fileImporter が同居する構成がシート誤発火の
// 疑いとなり、`pendingTarget` + 単一の共有 .fileImporter へ集約した。
struct SCSISettingsView: View {
    @EnvironmentObject var settingsViewModel: SettingsViewModel
    @EnvironmentObject var configManager: ConfigManager
    #if os(macOS)
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    // P442: D&D の状態は「行ごとに独立」させる(ID をキーにした辞書)。
    // 単一の共有 @State だと、行 A の警告表示中に行 B へドロップされた場合に
    // 警告の早期消失/取り違え表示が起きる。
    @State private var dropWarnings: [Int: String] = [:]
    @State private var dropTimers: [Int: Timer] = [:]
    @State private var isTargetedRows: [Int: Bool] = [:]
    #else
    @EnvironmentObject var iosViewModel: MX68KiOSViewModel

    /// P712 §R-D / R-E — マウント失敗理由と成功時の注記を無言にしないための共有 1 行。
    /// ★`Section` の外に置くこと —— 各 Section には `.disabled(scsiMode == "none")` が
    /// 掛かっており、中へ入れるとメッセージ自体がグレーアウトして読めなくなる。
    @State private var mountError: String?

    /// 内蔵モードは Bridge へライブ挿入せず、config への書込み +
    /// `pushConfig` 経由(次回ハードリセット)で反映する。macOS :12-14 と同一。
    private var scsiLive: Bool {
        settingsViewModel.scsiMode != "internal"
    }

    /// P712c — SASISettingsView と同じ設計(§バグBの修正参照)。SCSI / MO / CD の
    /// 3 種のターゲットを 1 つの enum で表し、単一の共有 fileImporter で受ける。
    /// 行ごとに専用 fileImporter を持つ旧方式は、同一 Form 内に多数(9個)の
    /// .fileImporter が同居することによるシート誤発火の疑いがあり撤回。
    private enum PickerTarget: Equatable {
        case scsi(Int)
        case mo
        case cd
    }
    @State private var pendingTarget: PickerTarget?

    @ViewBuilder
    private var mountErrorRow: some View {
        if let mountError {
            Text(mountError)
                .font(.subheadline)
                .foregroundColor(.red)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
    #endif

    // P457 (D-34): 判定基準を settingsViewModel の pending 値(Apply で即時変化)から
    // Bridge の「配線確定機種」(mx68k_reset_hard() でのみ更新)へ切替える。
    // insert 側のゲート(P268)も同じ g_wired_machine_type を見ているため、
    // 「操作できるのに挿入は必ず拒否される」窓がこれで消える。4 == SCSI。
    private var isSCSIMachine: Bool {
        mx68k_get_wired_machine_type() == 4
    }

    var body: some View {
        Form {
            // P581: 本タブは日本語リテラル直書き(英語モードでも日本語が出る)だったため、
            // 他タブと同じ「英語ソース + MX68K/Localizable.xcstrings の ja 訳」方式へ統一した。
            Section(header: Text("SCSI Mode")) {
                // P274-fix: .pickerStyle(.radioGroup) では個々のオプションへの
                // .disabled() が効かず、機種条件に関わらず全選択肢が選べてしまう
                // (ユーザーの hands-on 確認で発覚)。個別に .disabled() が確実に
                // 効く Button ベースのカスタムラジオ行に置き換える。
                VStack(alignment: .leading, spacing: 6) {
                    radioRow(label: "Not installed", tag: "none", enabled: !isSCSIMachine)
                    radioRow(label: "Internal", tag: "internal", enabled: isSCSIMachine)
                    radioRow(label: "External board (CZ-6BS1)", tag: "external", enabled: !isSCSIMachine)
                }

                if isSCSIMachine {
                    // P712g: iOS には Command キーが無く、ハードリセットは画面上部の
                    // ボタンをタップする操作(P706 の分岐様式に倣う)。
                    #if os(macOS)
                    Text("This machine uses the internal SCSI interface. Disk changes take effect after pressing Apply and then performing a hard reset (⌘R). To boot from SCSI, load SCSIINROM in the BIOS tab.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    #else
                    Text("This machine uses the internal SCSI interface. Disk changes take effect after pressing Apply and then tapping Hard Reset. To boot from SCSI, load SCSIINROM in the BIOS tab.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    #endif
                } else {
                    Text("This is a SASI machine. Choose “External board” to use disks on an external SCSI board (CZ-6BS1). Mounting and ejecting take effect immediately. SCSIEXROM must be loaded in the BIOS tab.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                // P457 (D-34): 上のラジオの選択可否は「配線確定機種」基準になったため、
                // 機種を切り替えて Apply しただけでは変わらない(ハードリセットで反映)。
                #if os(macOS)
                Text("⚠ A machine type change takes effect after pressing Apply and then performing a hard reset (⌘R) — until then the choices above stay as they are.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #else
                Text("⚠ A machine type change takes effect after pressing Apply and then tapping Hard Reset — until then the choices above stay as they are.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #endif
                // P450: XM6 と同じくメモリスイッチ自動更新は SASI/SCSI 共通の 1 設定。
                // チェックボックス本体は SASI タブにのみ置き、ここでは所在を案内する。
                Text("The memory switch auto-update setting is shared with the SASI tab.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Section(header: Text("SCSI Devices (ID0-6)")) {
                ForEach(0..<7) { id in
                    scsiRow(id: id)
                }
                if settingsViewModel.scsiMode == "internal" {
                    // P576 (D-52): 「次回ハードリセット」だけだと Apply を挟まずに
                    // ⌘R すれば反映されると読めてしまう。実際は Apply で
                    // 設定が保存されて初めてハードリセットが新しい構成を読む。
                    // P712g: iOS 側の文言は BIOSSettingsView / HardwareSettingsView が
                    // 既に使っているキーと完全に同一(xcstrings の既存エントリを再利用)。
                    #if os(macOS)
                    Text("⚠ Takes effect after pressing Apply and then performing a hard reset (⌘R).")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    #else
                    Text("⚠ Takes effect after pressing Apply and then tapping Hard Reset.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    #endif
                } else if settingsViewModel.scsiMode == "external" {
                    Text("Mounting and ejecting take effect immediately, on the next disk access.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .disabled(settingsViewModel.scsiMode == "none")

            // P668 (Docs/10 C-4): SCSI MO(光磁気ディスク)。ID5 固定の専用スロット。
            Section(header: Text("MO Drive (ID5)")) {
                moRow()
                if !scsiPath(5).isEmpty {
                    Text("⚠ SCSI ID5 is occupied by a hard disk image. Eject the SCSI5 image above to use the MO drive.")
                        .font(.subheadline).foregroundColor(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }
                #if os(macOS)
                Text("⚠ Takes effect after pressing Apply and then performing a hard reset (⌘R). Media cannot be swapped while running.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #else
                Text("⚠ Takes effect after pressing Apply and then tapping Hard Reset. Media cannot be swapped while running.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #endif
                Text("A resident SCSI device driver such as SUSIE.X must be loaded on the guest (e.g. SUSIE -ID5 @:) for the MO drive to appear as a drive letter — the real X68000 works the same way. MX68K does not bundle such a driver.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .disabled(settingsViewModel.scsiMode == "none")

            // P676 (Docs/10 C-5): SCSI CD-ROM(ISO / Mode1)。ID6 固定の専用スロット。
            Section(header: Text("CD-ROM Drive (ID6)")) {
                cdRow()
                if cdBlockedByHD {
                    Text("⚠ SCSI ID6 is occupied by a hard disk image. Eject the SCSI6 image above to use the CD-ROM drive.")
                        .font(.subheadline).foregroundColor(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }
                #if os(macOS)
                Text("⚠ The first mount takes effect after pressing Apply and then performing a hard reset (⌘R). Once the drive is attached, discs can be swapped while running.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #else
                Text("⚠ The first mount takes effect after pressing Apply and then tapping Hard Reset. Once the drive is attached, discs can be swapped while running.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #endif
                Text("A resident SCSI device driver such as SUSIE.X must be loaded on the guest (e.g. SUSIE -ID6 @:) for the CD-ROM drive to appear as a drive letter — the real X68000 works the same way. MX68K does not bundle such a driver.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Text("Data (Mode1) tracks only: ISO images with 2048-byte sectors and MODE1/2352 RAW images are supported. CD-DA audio playback and booting from CD are not supported.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .disabled(settingsViewModel.scsiMode == "none")

            // P574 (Docs/10 C-2) → P578 でこのタブにも「Create Blank Image」Section を
            // 置いていたが、P581 で撤去した。空イメージ作成は Tools メニュー
            // (「Create Image」→「SCSI Hard Disk Image…」= `BlankImagePresentation`)へ
            // 一本化する。ダイアログ本体(BlankSCSIImageDialog)と生成ロジックは無変更。

            #if os(iOS)
            mountErrorRow
            #endif
        }
        // P580 — macOS 標準のグループスタイル(Form 自体がスクロール可能)。
        .formStyle(.grouped)
        // P712c — 単一の共有 fileImporter(§バグBの修正)。SCSI 7 行 + MO + CD の
        // 計 9 個の .fileImporter を同居させる旧方式を撤回し、この 1 個へ集約する。
        #if os(iOS)
        .fileImporter(isPresented: Binding(
            get: { pendingTarget != nil },
            set: { newValue in
                guard !newValue else { return }
                // P712e — キャンセル時は completion ハンドラが一度も呼ばれない(実機ログで確認済み)。
                // 成功時は completion が同期的に pendingTarget をクリアするので、次の RunLoop の
                // 時点ではこの set を引き起こした値と一致しなくなっている(= 処理済み)。一致した
                // ままなら completion が呼ばれなかった = キャンセルとみなしてここでクリアする。
                // set の中で即座にクリアしない理由は P712c/P712d の教訓(completion より先に
                // クリアすると成功時の処理が空振りする)。
                let dismissedTarget = pendingTarget
                DispatchQueue.main.async {
                    if pendingTarget == dismissedTarget {
                        pendingTarget = nil
                    }
                }
            }
        ), allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            guard let target = pendingTarget else { return }
            pendingTarget = nil
            guard case .success(let urls) = result, let url = urls.first else { return }
            switch target {
            case .scsi(let id):
                let (ok, message) = iosViewModel.insertSCSI(id: id, url: url, live: scsiLive)
                mountError = message
                if ok { setSCSIPath(id, url.path) }
            case .mo:
                let (ok, message) = iosViewModel.insertMO(url: url)
                mountError = message
                if ok {
                    configManager.config.extensions.moPath = url.path
                    configManager.save()
                }
            case .cd:
                let (ok, message) = iosViewModel.insertCD(url: url)
                mountError = message
                if ok {
                    configManager.config.extensions.cdPath = url.path
                    configManager.save()
                }
            }
        }
        #endif
    }

    // MARK: - Mode radio

    // P581: `label` は `String` だと `Text(_ content: S)`(非ローカライズ)側の
    // オーバーロードへ解決され、xcstrings の ja 訳が引かれない。`LocalizedStringKey`
    // にすることで呼出側のリテラルがそのままローカライズキーになる。
    @ViewBuilder
    private func radioRow(label: LocalizedStringKey, tag: String, enabled: Bool) -> some View {
        Button {
            settingsViewModel.scsiMode = tag
        } label: {
            HStack {
                Image(systemName: settingsViewModel.scsiMode == tag ? "largecircle.fill.circle" : "circle")
                Text(label)
                Spacer()
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .foregroundColor(enabled ? .primary : .secondary)
    }

    // MARK: - Disk row

    private func scsiPath(_ id: Int) -> String {
        let ext = configManager.config.extensions
        switch id {
        case 0: return ext.scsi0Path
        case 1: return ext.scsi1Path
        case 2: return ext.scsi2Path
        case 3: return ext.scsi3Path
        case 4: return ext.scsi4Path
        case 5: return ext.scsi5Path
        case 6: return ext.scsi6Path
        default: return ""
        }
    }

    /// 内蔵モードでのみ使用: config へ直接書込み+save()(ライブ反映なし)。
    private func setSCSIPath(_ id: Int, _ path: String) {
        switch id {
        case 0: configManager.config.extensions.scsi0Path = path
        case 1: configManager.config.extensions.scsi1Path = path
        case 2: configManager.config.extensions.scsi2Path = path
        case 3: configManager.config.extensions.scsi3Path = path
        case 4: configManager.config.extensions.scsi4Path = path
        case 5: configManager.config.extensions.scsi5Path = path
        case 6: configManager.config.extensions.scsi6Path = path
        default: break
        }
        configManager.save()
    }

    @ViewBuilder
    private func scsiRow(id: Int) -> some View {
        let path = scsiPath(id)
        #if os(macOS)
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(verbatim: "SCSI\(id)")
                Text(path.isEmpty ? String(localized: "(no image)")
                                  : (path as NSString).lastPathComponent)
                    .foregroundColor(path.isEmpty ? .secondary : .primary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    // P578: 中略(P442 の意図的設計)は維持したまま、ホバーで
                    // フルパスを確認できるようにする。
                    .help(path)
                Spacer()
                Button("Select…") {
                    guard let url = browseDiskImage() else { return }
                    if settingsViewModel.scsiMode == "internal" {
                        setSCSIPath(id, url.path)
                    } else {
                        emulatorViewModel.insertSCSI(id: id, url: url)
                    }
                }
                Button("Eject") {
                    if settingsViewModel.scsiMode == "internal" {
                        setSCSIPath(id, "")
                    } else {
                        emulatorViewModel.ejectSCSI(id: id)
                    }
                }
                .disabled(path.isEmpty)
            }
            // P442: 拒否理由のインライン警告(設定シート表示中は StatusBar が
            // 見えないため、行の直下に出す)。数秒後に自動で消える。
            if let warning = dropWarnings[id] {
                Text(warning)
                    .font(.subheadline)
                    .foregroundColor(.red)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .contentShape(Rectangle())
        .background(
            RoundedRectangle(cornerRadius: 4)
                .fill(Color.accentColor.opacity((isTargetedRows[id] ?? false) ? 0.15 : 0.0))
        )
        .onDrop(of: [UTType.fileURL.identifier],
                isTargeted: Binding(
                    get: { isTargetedRows[id] ?? false },
                    set: { isTargetedRows[id] = $0 }
                )) { providers in
            handleDrop(id: id, providers: providers)
        }
        #else
        // P712 — iOS 版の行。内蔵/外付けの分岐は macOS(:208-222)と同一で、
        // `live` フラグとして ViewModel へ渡す(内蔵では `mx68k_scsi_insert` を
        // 呼ばない —— 配線確定機種が SCSI のとき必ず -2 で拒否されるため)。
        HStack {
            Text(verbatim: "SCSI\(id)")
            Text(path.isEmpty ? String(localized: "(no image)")
                              : (path as NSString).lastPathComponent)
                .foregroundColor(path.isEmpty ? .secondary : .primary)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer()
            // Form の行タップ認識と個々の Button のタップ認識が競合し、隣接ボタンの
            // アクションまで同一フレームで発火する(P712f 実機ログで確認)。
            Button("Select…") {
                pendingTarget = .scsi(id)
            }
            .buttonStyle(.borderless)
            Button("Eject") {
                let (ok, message) = iosViewModel.ejectSCSI(id: id, live: scsiLive)
                mountError = message
                if ok { setSCSIPath(id, "") }
            }
            .buttonStyle(.borderless)
            .disabled(path.isEmpty)
        }
        #endif
    }

    // MARK: - MO row (P668 / Docs/10 C-4)

    #if os(macOS)
    /// MO 行の D&D 状態は SCSI 行と同じ辞書を使い回す。ID0-6 と衝突しない
    /// キーを充てることで、行ごとに独立した警告/ハイライトという P442 の
    /// 設計をそのまま流用する。
    private static let moDropKey = -1
    #endif

    /// ID5 に SCSI HD イメージが設定済みなら MO は装着できない
    /// (設定済み HD を絶対に押し退けない — Bridge 側 Construct() の
    ///  `!hd5_path` 条件が backstop、こちらは前段ゲート)。
    private var moBlockedByHD: Bool {
        !scsiPath(5).isEmpty
    }

    @ViewBuilder
    private func moRow() -> some View {
        let path = configManager.config.extensions.moPath
        #if os(macOS)
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(verbatim: "MO")
                Text(path.isEmpty ? String(localized: "(no image)")
                                  : (path as NSString).lastPathComponent)
                    .foregroundColor(path.isEmpty ? .secondary : .primary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(path)
                Spacer()
                Button("Select…") {
                    guard let url = browseDiskImage() else { return }
                    emulatorViewModel.insertMO(url: url)
                }
                .disabled(moBlockedByHD)
                Button("Eject") {
                    emulatorViewModel.ejectMO()
                }
                .disabled(path.isEmpty)
            }
            if let warning = dropWarnings[Self.moDropKey] {
                Text(warning)
                    .font(.subheadline)
                    .foregroundColor(.red)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .contentShape(Rectangle())
        .background(
            RoundedRectangle(cornerRadius: 4)
                .fill(Color.accentColor.opacity((isTargetedRows[Self.moDropKey] ?? false) ? 0.15 : 0.0))
        )
        .onDrop(of: [UTType.fileURL.identifier],
                isTargeted: Binding(
                    get: { isTargetedRows[Self.moDropKey] ?? false },
                    set: { isTargetedRows[Self.moDropKey] = $0 }
                )) { providers in
            handleMODrop(providers: providers)
        }
        #else
        // P712 — iOS 版の MO 行。ライブ反映/次回ハードリセットの分岐は Bridge の
        // 戻り値(0 / 1)が決め、その注記文言は ViewModel が macOS と同一文言で返す。
        HStack {
            Text(verbatim: "MO")
            Text(path.isEmpty ? String(localized: "(no image)")
                              : (path as NSString).lastPathComponent)
                .foregroundColor(path.isEmpty ? .secondary : .primary)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer()
            // Form の行タップ認識と個々の Button のタップ認識が競合し、隣接ボタンの
            // アクションまで同一フレームで発火する(P712f 実機ログで確認)。
            Button("Select…") {
                pendingTarget = .mo
            }
            .buttonStyle(.borderless)
            .disabled(moBlockedByHD)
            Button("Eject") {
                let (ok, message) = iosViewModel.ejectMO()
                mountError = message
                if ok {
                    configManager.config.extensions.moPath = ""
                    configManager.save()
                }
            }
            .buttonStyle(.borderless)
            .disabled(path.isEmpty)
        }
        #endif
    }

    #if os(macOS)
    /// MO 行への D&D。SCSI 行と同じくディレクトリのみ拒否し、拡張子では絞らない
    /// (D-8 再発防止)。サイズ検証は Bridge 側 `mx68k_mo_insert` が担う。
    private func handleMODrop(providers: [NSItemProvider]) -> Bool {
        guard settingsViewModel.scsiMode != "none" else { return false }
        guard !moBlockedByHD else {
            showWarning(id: Self.moDropKey,
                        String(localized: "SCSI ID5 is occupied by a hard disk image."))
            return false
        }
        guard let provider = providers.first else { return false }
        _ = provider.loadObject(ofClass: URL.self) { url, _ in
            guard let url = url else { return }
            DispatchQueue.main.async {
                var isDirectory: ObjCBool = false
                guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory),
                      !isDirectory.boolValue else {
                    showWarning(id: Self.moDropKey,
                                String(localized: "Not a file — drop a disk image file."))
                    return
                }
                clearWarning(id: Self.moDropKey)
                emulatorViewModel.insertMO(url: url)
            }
        }
        return true
    }
    #endif

    // MARK: - CD-ROM row (P676 / Docs/10 C-5)

    #if os(macOS)
    /// CD 行の D&D 状態は MO 行と同じ辞書を使い回す。ID0-6 とも MO(-1)とも
    /// 衝突しないキーを充てる。
    private static let cdDropKey = -2
    #endif

    /// ID6 に SCSI HD イメージが設定済みなら CD は装着できない
    /// (設定済み HD を絶対に押し退けない — Bridge 側 Construct() の
    ///  `!hd6_path` 条件が backstop、こちらは前段ゲート)。
    /// ★XM6 の「ID6 が一杯なら ID7 へ退避」は採らない: MX の ID7 は
    ///  イニシエータ自身であり、HDMax=7(P576/D-52)とも非互換。
    private var cdBlockedByHD: Bool {
        !scsiPath(6).isEmpty
    }

    @ViewBuilder
    private func cdRow() -> some View {
        let path = configManager.config.extensions.cdPath
        #if os(macOS)
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(verbatim: "CD")
                Text(path.isEmpty ? String(localized: "(no image)")
                                  : (path as NSString).lastPathComponent)
                    .foregroundColor(path.isEmpty ? .secondary : .primary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(path)
                Spacer()
                // P676: CD-ROM は物理的に ROM 媒体なので、ライトプロテクトは
                // 常に ON で変更できない(Bridge 側 `disk.writep = TRUE` 固定 —
                // XM6:vm/disk.cpp:2569/2744)。P673 のライトプロテクト UI 統一に
                // 沿って表示は出すが、操作はできないようグレーアウトする。
                Image(systemName: "lock.fill")
                    .foregroundColor(.secondary)
                    .frame(width: 16)
                    .help(String(localized:
                        "CD-ROM media is always write-protected — this cannot be changed."))
                Button("Select…") {
                    guard let url = browseCDImage() else { return }
                    emulatorViewModel.insertCD(url: url)
                }
                .disabled(cdBlockedByHD)
                Button("Eject") {
                    emulatorViewModel.ejectCD()
                }
                .disabled(path.isEmpty)
            }
            if let warning = dropWarnings[Self.cdDropKey] {
                Text(warning)
                    .font(.subheadline)
                    .foregroundColor(.red)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .contentShape(Rectangle())
        .background(
            RoundedRectangle(cornerRadius: 4)
                .fill(Color.accentColor.opacity((isTargetedRows[Self.cdDropKey] ?? false) ? 0.15 : 0.0))
        )
        .onDrop(of: [UTType.fileURL.identifier],
                isTargeted: Binding(
                    get: { isTargetedRows[Self.cdDropKey] ?? false },
                    set: { isTargetedRows[Self.cdDropKey] = $0 }
                )) { providers in
            handleCDDrop(providers: providers)
        }
        #else
        // P712 — iOS 版の CD 行。ライトプロテクトは常時 ON で変更不可という
        // macOS の表示(P676)もそのまま持ち込む。
        HStack {
            Text(verbatim: "CD")
            Text(path.isEmpty ? String(localized: "(no image)")
                              : (path as NSString).lastPathComponent)
                .foregroundColor(path.isEmpty ? .secondary : .primary)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer()
            Image(systemName: "lock.fill")
                .foregroundColor(.secondary)
                .frame(width: 16)
            // Form の行タップ認識と個々の Button のタップ認識が競合し、隣接ボタンの
            // アクションまで同一フレームで発火する(P712f 実機ログで確認)。
            Button("Select…") {
                pendingTarget = .cd
            }
            .buttonStyle(.borderless)
            .disabled(cdBlockedByHD)
            Button("Eject") {
                let (ok, message) = iosViewModel.ejectCD()
                mountError = message
                if ok {
                    configManager.config.extensions.cdPath = ""
                    configManager.save()
                }
            }
            .buttonStyle(.borderless)
            .disabled(path.isEmpty)
        }
        #endif
    }

    #if os(macOS)
    /// CD 行への D&D。MO 行と同じくディレクトリのみ拒否し、拡張子では絞らない
    /// (D-8 再発防止)。サイズ検証は Bridge 側 `mx68k_cd_insert` が、
    /// Mode1 判定は `SCSICD::OpenIso` が担う。
    private func handleCDDrop(providers: [NSItemProvider]) -> Bool {
        guard settingsViewModel.scsiMode != "none" else { return false }
        guard !cdBlockedByHD else {
            showWarning(id: Self.cdDropKey,
                        String(localized: "SCSI ID6 is occupied by a hard disk image."))
            return false
        }
        guard let provider = providers.first else { return false }
        _ = provider.loadObject(ofClass: URL.self) { url, _ in
            guard let url = url else { return }
            DispatchQueue.main.async {
                var isDirectory: ObjCBool = false
                guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory),
                      !isDirectory.boolValue else {
                    showWarning(id: Self.cdDropKey,
                                String(localized: "Not a file — drop a disk image file."))
                    return
                }
                clearWarning(id: Self.cdDropKey)
                emulatorViewModel.insertCD(url: url)
            }
        }
        return true
    }

    // MARK: - Drag & drop (P442 / D-8)

    /// 落とされた行の ID へマウントする。
    /// 拡張子チェックは**行わない** — 既存の `browseDiskImage()` ピッカーは
    /// `allowedContentTypes` に `UTType.data` を含み事実上ほぼ全てのファイルを
    /// 通すため、D&D 側だけ `.hds`/`.hdf` に絞ると「ピッカーでは通るファイルが
    /// D&D では黙って拒否される」= 形を変えた D-8 の再発になる。
    /// マウント経路は `Select…` と同じく `scsiMode` で分岐させる
    /// (internal = config 書込みのみ、それ以外 = insertSCSI 経由)。
    /// ここを一本化してはならない。
    /// ★P692: 「それ以外 = ライブ反映」という旧記述は P510 以降 stale。外付け
    ///   ボードが実際に配線されている(ext_wired && ext_rom_loaded)ときは
    ///   $EA0000 の SPC レジスタ領域が移植 XM6 SPC へ差し替わり、Core scsi.c の
    ///   per-block open 経路に到達しない。実ディスクは
    ///   scsi_real_install_construct() で一度だけ開かれ fd は DiskCache が保持する
    ///   ため、あとからパスを書き換えても反映は次のハードリセット(⌘R)——内蔵と
    ///   同じ規則になる。旧来のライブ反映が残るのは配線ゲート不成立時
    ///   (= 外付け SCSI がそもそも機能しないとき)だけ。
    private func handleDrop(id: Int, providers: [NSItemProvider]) -> Bool {
        // P442: セクションの `.disabled(scsiMode == "none")` が `.onDrop` を
        // 確実に無効化する保証がないため、ハンドラ内でも同じ条件で早期 return。
        guard settingsViewModel.scsiMode != "none" else { return false }
        guard let provider = providers.first else { return false }
        // 複数ファイル同時ドロップは初版では非対応(先頭のみ処理)。
        _ = provider.loadObject(ofClass: URL.self) { url, _ in
            guard let url = url else { return }
            // loadObject の completion はバックグラウンドキューで走る。
            DispatchQueue.main.async {
                var isDirectory: ObjCBool = false
                guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory),
                      !isDirectory.boolValue else {
                    showWarning(id: id,
                                String(localized: "Not a file — drop a disk image file."))
                    return
                }
                clearWarning(id: id)
                if settingsViewModel.scsiMode == "internal" {
                    setSCSIPath(id, url.path)
                } else {
                    emulatorViewModel.insertSCSI(id: id, url: url)
                }
            }
        }
        return true
    }

    /// 行ごとの警告を表示し、3 秒後に自動で消す。
    /// 同じ行へ連続ドロップされた場合、古いタイマーが新しい警告を早期に
    /// 消さないよう、差し替え前に必ず invalidate() する。
    private func showWarning(id: Int, _ message: String) {
        dropTimers[id]?.invalidate()
        dropWarnings[id] = message
        dropTimers[id] = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: false) { _ in
            dropWarnings[id] = nil
            dropTimers[id] = nil
        }
    }

    private func clearWarning(id: Int) {
        dropTimers[id]?.invalidate()
        dropTimers[id] = nil
        dropWarnings[id] = nil
    }

    // MARK: - Helpers

    private func browseDiskImage() -> URL? {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        // SCSI イメージ拡張子の慣習は未確定 — 過度に絞らず .hds/.hdf/汎用 data を許容。
        // P668: MO イメージ(.mos、XM6 のファイルフィルタ由来)も候補に加える。
        let types = ["hds", "hdf", "mos"].compactMap { UTType(filenameExtension: $0) } + [UTType.data]
        panel.allowedContentTypes = types
        return panel.runModal() == .OK ? panel.url : nil
    }

    /// P676: CD-ROM イメージ用のピッカー。拡張子で絞りすぎない方針は
    /// browseDiskImage() と同じ(`UTType.data` を含むため事実上ほぼ全ての
    /// ファイルを通す)。検証は Bridge 側が担う。
    private func browseCDImage() -> URL? {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        let types = ["iso", "bin", "img"].compactMap { UTType(filenameExtension: $0) } + [UTType.data]
        panel.allowedContentTypes = types
        return panel.runModal() == .OK ? panel.url : nil
    }
    #endif
}
