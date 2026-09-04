import SwiftUI
import UniformTypeIdentifiers

struct EmulatorView: View {
    @EnvironmentObject var viewModel: EmulatorViewModel
    @EnvironmentObject var configManager: ConfigManager
    // P194: 共有インスタンスを参照(観測しない。@Published keyboardState はキー入力の
    // たびに変化するため、観測すると毎キーストロークでビューが再評価されていた)。
    private let inputManager = InputManager.shared

    var body: some View {
        VStack(spacing: 0) {
            ToolbarView()
                .environmentObject(viewModel)

            ZStack {
                EmulatorMetalView()
                // P204 — 電源OFF の画面コントラスト低下~黒画面を Metal 非改変で近似する
                // 黒オーバーレイ。allowsHitTesting(false) で下の Metal ビューへの
                // ドラッグ&ドロップ/入力を妨げない。
                Rectangle()
                    .fill(Color.black)
                    .opacity(viewModel.displayDimming)
                    .allowsHitTesting(false)
            }
                .frame(minWidth: 640, minHeight: 480)
                // P192 — GeometryReader による metalViewSize の publish は削除
                // (WindowScaler が AppKit から同期実測するようになったため)。
                .onAppear {
                    // P194: 設定を入れてから割当を構築する単一経路
                    inputManager.applyConfig(configManager.config.input)
                    // P227: ゲームパッド(GameController)の接続監視を開始
                    inputManager.startGamepadMonitoring()
                    // P684: ドライブ 0..3。分岐は FDDConfig.setLastPath /
                    // setWriteProtect に集約済み(onHDDChanged と同じ形)。
                    viewModel.onFDDMounted = { drive, path in
                        configManager.config.fdd.setLastPath(drive, path)
                        configManager.save()
                    }
                    // P271: FDライトプロテクト トグルの初期状態を config から反映し、
                    // 変更時は config に永続化する(onFDDMounted と対称)。
                    viewModel.fdd0WriteProtect = configManager.config.fdd.fdd0WriteProtect
                    viewModel.fdd1WriteProtect = configManager.config.fdd.fdd1WriteProtect
                    // P684: FD2/FD3 は南京錠トグルの UI を持たないが、config からの
                    // 初期化は FD0/FD1 と対称に行う(File メニューの「挿入(書込禁止)…」
                    // 経由で書き込まれた値を次回起動時に引き継ぐため)。
                    viewModel.fdd2WriteProtect = configManager.config.fdd.fdd2WriteProtect
                    viewModel.fdd3WriteProtect = configManager.config.fdd.fdd3WriteProtect
                    viewModel.onFDDWriteProtectChanged = { drive, protect in
                        configManager.config.fdd.setWriteProtect(drive, protect)
                        configManager.save()
                    }
                    // P191: イジェクトも config に反映(終了→再起動でディスクが復活しない)。
                    viewModel.onFDDEjected = { drive in
                        configManager.config.fdd.setLastPath(drive, "")
                        configManager.save()
                    }
                    // P200: HDD(.hdf)の挿入/取り外しを config に永続化(path 空 = 取り外し)。
                    // P455: unit 0..7。分岐は ExtensionsConfig.setHDDPath に集約済み。
                    viewModel.onHDDChanged = { unit, path in
                        configManager.config.extensions.setHDDPath(unit, path)
                        configManager.save()
                    }
                    // P241: 外付け SCSI(CZ-6BS1)の挿入/取り外しを config に永続化(ID 0..6, path 空 = 取り外し)。
                    viewModel.onSCSIChanged = { id, path in
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
                    // P668: MO(ID5 固定スロット)の装着/取り外しを config に永続化
                    // (path 空 = 取り外し)。MO は単一スロットなので ID 引数を取らない。
                    viewModel.onMOChanged = { path in
                        configManager.config.extensions.moPath = path
                        configManager.save()
                    }
                    // P676: CD-ROM(ID6 固定スロット)の装着/取り外しを config に永続化
                    // (path 空 = 取り外し)。MO と同じく単一スロットなので ID 引数を取らない。
                    viewModel.onCDChanged = { path in
                        configManager.config.extensions.cdPath = path
                        configManager.save()
                    }
                    // P204: 電源ON(cold boot)= config/fdd を渡して startEmulation を再実行。
                    // N2: viewModel が保持するクロージャゆえ [weak viewModel] で retain cycle 回避。
                    viewModel.onPowerOn = { [weak viewModel] in
                        viewModel?.startEmulation(
                            config: configManager.config,
                            fdd0: configManager.config.fdd.lastFDD0Path,
                            fdd1: configManager.config.fdd.lastFDD1Path,
                            // P684: FD2/FD3 も cold boot 時に再マウントする。
                            fdd2: configManager.config.fdd.lastFDD2Path,
                            fdd3: configManager.config.fdd.lastFDD3Path)
                    }
                    let fdd0 = configManager.config.fdd.lastFDD0Path
                    let fdd1 = configManager.config.fdd.lastFDD1Path
                    // P684: 起動時も FD2/FD3 の保存済みパスから自動再マウントする。
                    let fdd2 = configManager.config.fdd.lastFDD2Path
                    let fdd3 = configManager.config.fdd.lastFDD3Path
                    viewModel.startEmulation(config: configManager.config,
                                             fdd0: fdd0, fdd1: fdd1,
                                             fdd2: fdd2, fdd3: fdd3)
                }
                .onDisappear {
                    // P227: ゲームパッドの接続監視を停止(observer/ハンドラ解除 + idle 送出)
                    inputManager.stopGamepadMonitoring()
                    viewModel.stopEmulation()
                }
                .onDrop(of: [UTType.fileURL.identifier], isTargeted: nil) { providers in
                    guard let provider = providers.first else { return false }
                    _ = provider.loadObject(ofClass: URL.self) { url, _ in
                        guard let url = url else { return }
                        let ext = url.pathExtension.lowercased()
                        // P554: zip も受け付ける(展開は EmulatorViewModel 側の共通経路)。
                        let allowed = ["xdf", "dim", "d88", "hdm", "2hd", "img", "zip"]
                        if allowed.contains(ext) {
                            DispatchQueue.main.async {
                                let drive = viewModel.fdd0Path.isEmpty ? 0 : 1
                                if ext == "zip" {
                                    viewModel.handleArchiveSelection(drive: drive, zipPath: url.path)
                                } else {
                                    // P599: zip 由来の一時的な書込み禁止が残っていれば先に復元してから読む。
                                    let wp = viewModel.resolvedWriteProtectForNewMount(drive: drive)
                                    viewModel.mountFDD(drive: drive, path: url.path, writeProtect: wp)
                                }
                            }
                        }
                    }
                    return true
                }
                // P554 — zip 内に複数のディスクイメージがあった場合の選択シート。
                // ツールバー経路・D&D 経路どちらの提示要求もここが受ける(状態は ViewModel)。
                // 同一ビューへ .sheet を 2 つ付けると片方しか機能しないため、
                // 他の .sheet とは別のビューへ付ける。
                .sheet(isPresented: $viewModel.showArchivePicker) {
                    ArchiveEntryPickerSheet(
                        images: viewModel.archivePickerImages,
                        onSelect: { url in viewModel.completeArchiveSelection(url) },
                        onCancel: { viewModel.cancelArchiveSelection() })
                }

            StatusBarView()
                .environmentObject(viewModel)
        }
        // P579 — 設定画面の `.sheet` はここから削除した(専用ウィンドウ化、
        // MX68KApp.swift の `Window("Settings", id: "settings")` を参照)。
    }
}
