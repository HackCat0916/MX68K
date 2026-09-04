import SwiftUI
import UniformTypeIdentifiers

// P273: SASI 設定タブ。旧 ExtensionSettingsView の「Hard Disk (SASI .hdf)」
// セクションを移設。論理 unit 0..7 は Bridge で SASI device 0-7
// (Config.HDImage[unit*2])にマップされる(P455 で 2 → 8 台)。
// P239: HDD イメージの差し替えは次回のディスクアクセスから即座に反映される
// (sasi.c がセクタ毎に無キャッシュで File_Open するため)。手動リセットは不要。
// P442 (D-8): 各 HDD 行を個別のドロップターゲットにし、`.hdf` イメージを
// 直接ドラッグ&ドロップでマウントできるようにする。メインウィンドウの
// D&D は FD 専用のまま(実機/XM6 準拠、スコープ外)。
// P712: iOS へ移植。macOS 側の実効コードパスは変更せず、`#if os(iOS)` 分岐を
// 追加するだけ(BIOSSettingsView / HardwareSettingsView が確立した「宣言自体を
// `#if` で囲む」流儀を踏襲)。iOS はコピーインではなくセキュリティスコープ
// ブックマーク方式(`DiskBookmarkStore`)を使う。
// P712c: 当初は行ごとに `DiskPickerButton`(専用 fileImporter 持ち)を置いていたが、
// 同一 Form 内に 8 個の .fileImporter が同居する構成がシート誤発火の疑いとなり、
// `pendingUnit` + 単一の共有 .fileImporter へ集約した(DiskPickerButton は廃止)。
// P442 のドラッグ&ドロップは iOS へ移植しない(同等の操作系が無いためスコープ外)。
struct SASISettingsView: View {
    @EnvironmentObject var configManager: ConfigManager
    #if os(macOS)
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    // P442: D&D の状態は「行ごとに独立」させる(unit をキーにした辞書)。
    // 単一の共有 @State だと、行 A の警告表示中に行 B へドロップされた場合に
    // 警告の早期消失/取り違え表示が起きる。
    @State private var dropWarnings: [Int: String] = [:]
    @State private var dropTimers: [Int: Timer] = [:]
    @State private var isTargetedRows: [Int: Bool] = [:]
    #else
    @EnvironmentObject var iosViewModel: MX68KiOSViewModel

    /// P712 §R-D / R-E — マウント失敗理由と成功時の注記を無言にしないための共有 1 行。
    /// BIOSSettingsView の `importErrorRow` と同じ様式。★`Section` の外に置くこと ——
    /// 上の Section には `.disabled(isSCSIMachine)` が掛かっており、中へ入れると
    /// SCSI 機ではメッセージ自体がグレーアウトして読めなくなる。
    @State private var mountError: String?

    /// P712c — 単一の共有 fileImporter。どの unit が Select… を押したかを保持する
    /// (nil = 非表示)。行ごとに専用 fileImporter を持つ旧方式は、同一 Form 内に
    /// 多数(8個)の .fileImporter が同居することによるシート誤発火の疑いがあり撤回。
    @State private var pendingUnit: Int?

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

    // P268: SCSI 機では内蔵 SASI と外付 CZ-6BS1 を排他(内蔵 SCSI のみ配線)。
    // P457 (D-34): 判定基準を config の pending 値(Apply で即時変化)から
    // Bridge の「配線確定機種」(mx68k_reset_hard() でのみ更新)へ切替える。
    // insert 側のゲート(P268)も同じ g_wired_machine_type を見ているため、
    // 「操作できるのに挿入は必ず拒否される」窓がこれで消える。4 == SCSI。
    private var isSCSIMachine: Bool {
        mx68k_get_wired_machine_type() == 4
    }

    var body: some View {
        Form {
            Section(header: Text("Hard Disk (SASI .hdf)")) {
                // P455: 2 行ベタ書き → 8 行(SASI device ID 0-7)。
                // hddRow は既に unit 引数で一般化済み、D&D 状態も [Int: …] 辞書。
                ForEach(0..<ExtensionsConfig.hddUnitCount, id: \.self) { unit in
                    hddRow(unit: unit,
                           path: configManager.config.extensions.hddPath(unit))
                }
                if isSCSIMachine {
                    // P457 (D-34): 文言に「ハードリセット後に反映」を追記。
                    // 判定が配線確定機種基準になったため、機種を切り替えて Apply した
                    // だけではこの表示は変わらない(実際にハードリセットするまで旧機種
                    // のまま)——その挙動を利用者に伝える。
                    // ★文字列リテラルを変えるとローカライズキーも変わるので、
                    // MX68K/Localizable.xcstrings の ja エントリも同時に更新すること
                    // (更新漏れは英語のまま表示される = 孤児化)。
                    // P581: 注意文の先頭記号を `⚠` へ統一(リテラル変更 = キー変更のため
                    // xcstrings へ新リテラルの ja 訳を追加済み)。
                    // P712g: iOS には Command キーが無く、ハードリセットは画面上部の
                    // ボタンをタップする操作(BIOSSettingsView / HardwareSettingsView が
                    // P706 で確立した分岐様式に倣う)。
                    #if os(macOS)
                    Text("⚠ Disabled on SCSI machines — internal SASI is not wired. Switch Machine Type to SASI, click Apply, and then perform a hard reset (⌘R) to use these.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    #else
                    Text("⚠ Disabled on SCSI machines — internal SASI is not wired. Switch Machine Type to SASI, tap Apply, and then tap Hard Reset to use these.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    #endif
                } else {
                    Text("HDD changes take effect immediately on the next disk access — no reset needed.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .disabled(isSCSIMachine)

            // P450: メモリスイッチ自動更新(XM6「メモリスイッチ自動更新」相当)。
            // ★上の「Hard Disk (SASI .hdf)」Section とは別の独立 Section にすること —
            // あちらには .disabled(isSCSIMachine) が掛かっており、その中に置くと
            // SCSI 機で操作不能になり SCSI 側 memsw を制御できなくなる。
            // この設定は SASI/SCSI 共通(XM6 と同じく SASI タブに1個だけ置く)。
            Section(header: Text("Memory Switch")) {
                Toggle("Auto-update memory switches",
                       isOn: $configManager.config.extensions.memSwitchAutoUpdate)
                // ★1本の文字列リテラルにすること — `"a" + "b"` は String を作るため
                // Text(LocalizedStringKey) ではなく Text(S: StringProtocol) が選ばれ、
                // ローカライズされなくなる(既存タブは全て単一リテラル)。
                // P456 (D-33): $ED005A も本設定の管理下に入ったため文言を更新。
                // 文字列を変えるとローカライズキーも変わるので、
                // MX68K/Localizable.xcstrings の ja エントリも同時に更新すること
                // (更新漏れは英語のまま表示される = 孤児化)。
                // P712g: 末尾のハードリセット案内のみ iOS 向けへ分岐(⌘R → Hard Reset タップ)。
                #if os(macOS)
                Text("On hard reset, update the SRAM memory switches ($ED005A and $ED006F/70/71) to match the current machine and SASI disk configuration (matches XM6). $ED005A is the SASI drive-index limit: leave this off and a cleared SRAM keeps $ED005A at 0, so no SASI disk is recognized at all. Turn off to leave your SRAM untouched. Note: values previously written by guest software (SWITCH.X etc.) may be overwritten. ⚠ The toggle itself takes effect after pressing Apply and then performing a hard reset (⌘R).")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #else
                Text("On hard reset, update the SRAM memory switches ($ED005A and $ED006F/70/71) to match the current machine and SASI disk configuration (matches XM6). $ED005A is the SASI drive-index limit: leave this off and a cleared SRAM keeps $ED005A at 0, so no SASI disk is recognized at all. Turn off to leave your SRAM untouched. Note: values previously written by guest software (SWITCH.X etc.) may be overwritten. ⚠ The toggle itself takes effect after pressing Apply and then tapping Hard Reset.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #endif
            }

            // P574 (Docs/10 C-1) → P578 でこのタブにも「Create Blank Image」Section を
            // 置いていたが、P581 で撤去した。空イメージ作成は Tools メニュー
            // (「Create Image」→「SASI Hard Disk Image…」= `BlankImagePresentation`)へ
            // 一本化する。ダイアログ本体(BlankSASIImageDialog)と生成ロジックは無変更。

            #if os(iOS)
            mountErrorRow
            #endif
        }
        // P580 — macOS 標準のグループスタイル(Form 自体がスクロール可能)。
        .formStyle(.grouped)
        // P712c — 単一の共有 fileImporter(§バグBの修正)。行ごとに 8 個の
        // .fileImporter を同居させる旧方式を撤回し、この 1 個へ集約する。
        #if os(iOS)
        .fileImporter(isPresented: Binding(
            get: { pendingUnit != nil },
            set: { newValue in
                guard !newValue else { return }
                // P712e — キャンセル時は completion ハンドラが一度も呼ばれない(実機ログで確認済み)。
                // 成功時は completion が同期的に pendingUnit をクリアするので、次の RunLoop の
                // 時点ではこの set を引き起こした値と一致しなくなっている(= 処理済み)。一致した
                // ままなら completion が呼ばれなかった = キャンセルとみなしてここでクリアする。
                // set の中で即座にクリアしない理由は P712c/P712d の教訓(completion より先に
                // クリアすると成功時の処理が空振りする)。
                let dismissedUnit = pendingUnit
                DispatchQueue.main.async {
                    if pendingUnit == dismissedUnit {
                        pendingUnit = nil
                    }
                }
            }
        ), allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            guard let unit = pendingUnit else { return }
            pendingUnit = nil
            guard case .success(let urls) = result, let url = urls.first else { return }
            let (ok, message) = iosViewModel.insertHDD(unit: unit, url: url)
            mountError = message
            if ok {
                configManager.config.extensions.setHDDPath(unit, url.path)
                configManager.save()
            }
        }
        #endif
    }

    @ViewBuilder
    private func hddRow(unit: Int, path: String) -> some View {
        #if os(macOS)
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(verbatim: "HDD\(unit)")
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
                    let panel = NSOpenPanel()
                    panel.allowsMultipleSelection = false
                    panel.canChooseFiles = true
                    panel.canChooseDirectories = false
                    if let hdfType = UTType(filenameExtension: "hdf") {
                        panel.allowedContentTypes = [hdfType]
                    }
                    if panel.runModal() == .OK, let url = panel.url {
                        emulatorViewModel.insertHDD(unit: unit, url: url)
                    }
                }
                Button("Eject") {
                    emulatorViewModel.ejectHDD(unit: unit)
                }
                .disabled(path.isEmpty)
            }
            // P442: 拒否理由のインライン警告(設定シート表示中は StatusBar が
            // 見えないため、行の直下に出す)。数秒後に自動で消える。
            if let warning = dropWarnings[unit] {
                Text(warning)
                    .font(.subheadline)
                    .foregroundColor(.red)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .contentShape(Rectangle())
        .background(
            RoundedRectangle(cornerRadius: 4)
                .fill(Color.accentColor.opacity((isTargetedRows[unit] ?? false) ? 0.15 : 0.0))
        )
        .onDrop(of: [UTType.fileURL.identifier],
                isTargeted: Binding(
                    get: { isTargetedRows[unit] ?? false },
                    set: { isTargetedRows[unit] = $0 }
                )) { providers in
            handleDrop(unit: unit, providers: providers)
        }
        #else
        // P712 — iOS 版の行。macOS の `NSOpenPanel` は使えないため fileImporter を使う。
        // 永続化クロージャ(`onHDDChanged`)機構は移植せず、iOS の FDD0/1 が既に
        // 確立しているパターン(呼び出し側 View が config を直接書いて save)に倣う。
        // P712c — Select… は行ごとの fileImporter ではなく `pendingUnit` を立てるだけ。
        // 実際の提示は body 側の単一の共有 .fileImporter が行う。
        HStack {
            Text(verbatim: "HDD\(unit)")
            Text(path.isEmpty ? String(localized: "(no image)")
                              : (path as NSString).lastPathComponent)
                .foregroundColor(path.isEmpty ? .secondary : .primary)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer()
            // Form の行タップ認識と個々の Button のタップ認識が競合し、隣接ボタンの
            // アクションまで同一フレームで発火する(P712f 実機ログで確認)。
            Button("Select…") {
                pendingUnit = unit
            }
            .buttonStyle(.borderless)
            Button("Eject") {
                let (ok, message) = iosViewModel.ejectHDD(unit: unit)
                mountError = message
                if ok {
                    configManager.config.extensions.setHDDPath(unit, "")
                    configManager.save()
                }
            }
            .buttonStyle(.borderless)
            .disabled(path.isEmpty)
        }
        #endif
    }

    #if os(macOS)

    // MARK: - Drag & drop (P442 / D-8)

    /// 落とされた行の unit へマウントする。受理条件は `Select…`(NSOpenPanel)と
    /// 同一 — 実在する通常ファイルで拡張子が `.hdf` のもののみ。
    /// マウントは `Select…` とまったく同じ関数(`insertHDD`)を呼ぶ。
    private func handleDrop(unit: Int, providers: [NSItemProvider]) -> Bool {
        // P442: セクションの `.disabled(isSCSIMachine)` が `.onDrop` を確実に
        // 無効化する保証がないため、ハンドラ内でも同じ条件で早期 return する。
        guard !isSCSIMachine else { return false }
        guard let provider = providers.first else { return false }
        // 複数ファイル同時ドロップは初版では非対応(先頭のみ処理)。
        _ = provider.loadObject(ofClass: URL.self) { url, _ in
            guard let url = url else { return }
            // loadObject の completion はバックグラウンドキューで走る。
            DispatchQueue.main.async {
                var isDirectory: ObjCBool = false
                guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory),
                      !isDirectory.boolValue else {
                    showWarning(unit: unit,
                                String(localized: "Not a file — drop a disk image file."))
                    return
                }
                guard url.pathExtension.lowercased() == "hdf" else {
                    showWarning(unit: unit,
                                String(localized: "Only .hdf images can be dropped here."))
                    return
                }
                clearWarning(unit: unit)
                emulatorViewModel.insertHDD(unit: unit, url: url)
            }
        }
        return true
    }

    /// 行ごとの警告を表示し、3 秒後に自動で消す。
    /// 同じ行へ連続ドロップされた場合、古いタイマーが新しい警告を早期に
    /// 消さないよう、差し替え前に必ず invalidate() する。
    private func showWarning(unit: Int, _ message: String) {
        dropTimers[unit]?.invalidate()
        dropWarnings[unit] = message
        dropTimers[unit] = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: false) { _ in
            dropWarnings[unit] = nil
            dropTimers[unit] = nil
        }
    }

    private func clearWarning(unit: Int) {
        dropTimers[unit]?.invalidate()
        dropTimers[unit] = nil
        dropWarnings[unit] = nil
    }

    #endif
}
