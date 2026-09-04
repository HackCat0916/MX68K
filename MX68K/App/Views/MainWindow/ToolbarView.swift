import SwiftUI
import UniformTypeIdentifiers

struct ToolbarView: View {
    @EnvironmentObject var viewModel: EmulatorViewModel

    var body: some View {
        HStack(spacing: 12) {
            // P191: 確認フラグは ViewModel 側(メニュー ⌘R からも同じ alert を出すため)。
            // P192 — 原寸(768pt)でツールバーが収まるようアイコンのみに。機能名は .help() で補う。
            Button(action: { viewModel.showHardResetConfirm = true }) {
                Label("Hard Reset", systemImage: "arrow.counterclockwise")
            }
            .labelStyle(.iconOnly)
            .help(String(localized: "Hard Reset"))
            .disabled(viewModel.powerState == .poweringOff)   // P204/S3: 電源OFF フェード中は無効
            // P451: レガシー Alert(primaryButton:secondaryButton:) では Alert.Button に
            // .keyboardShortcut を付けられず Enter/Esc の挙動が AppKit 既定任せだったため、
            // 新 API (.alert(_:isPresented:actions:message:)) へ移行して明示割当てする。
            .alert("Hard Reset", isPresented: $viewModel.showHardResetConfirm) {
                Button("Cancel", role: .cancel) { }
                    .keyboardShortcut(.cancelAction)
                Button("Reset", role: .destructive) {
                    viewModel.hardReset()
                }
                .keyboardShortcut(.defaultAction)
            } message: {
                Text("The current state will be lost. Execute a hard reset?")
            }

            Button(action: { viewModel.softReset() }) {
                Label("Soft Reset", systemImage: "arrow.uturn.backward")
            }
            .labelStyle(.iconOnly)
            .help(String(localized: "Soft Reset"))
            .disabled(viewModel.powerState == .poweringOff)   // P204/S3

            Button(action: { viewModel.nmi() }) {
                Label("Interrupt", systemImage: "exclamationmark.triangle")
            }
            .labelStyle(.iconOnly)
            .help(String(localized: "Interrupt"))
            .disabled(viewModel.powerState == .poweringOff)   // P204/S3

            Divider()
                .frame(height: 20)

            fddSection(drive: 0, path: viewModel.fdd0Path)

            Divider()
                .frame(height: 20)

            fddSection(drive: 1, path: viewModel.fdd1Path)

            // P192 — Spacer() は削除。Text 自身が余白を吸い、幅不足時は最初に縮む
            // (Spacer が余白を全部吸うと priority -1 の Text に最小幅しか提案されず
            //  「…」だけになってしまうため)。
            Text(viewModel.statusText)
                .font(.caption)
                .foregroundColor(.secondary)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .trailing)
                .layoutPriority(-1)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
    }

    @ViewBuilder
    private func fddSection(drive: Int, path: String) -> some View {
        HStack(spacing: 4) {
            Text("FDD\(drive):")
                .font(.caption)
                .foregroundColor(.secondary)

            Text(path.isEmpty ? String(localized: "(empty)") : path)
                .font(.caption)
                .lineLimit(1)
                .truncationMode(.middle)   // P192: 先頭と拡張子が読めるように
                .frame(minWidth: 50, idealWidth: 90, maxWidth: 120)

            Button(action: { selectDisk(drive: drive) }) {
                Image(systemName: "folder")
            }
            .help(String(localized: "Select disk image for FDD\(drive)"))
            .disabled(viewModel.powerState == .poweringOff)   // P204/S3

            Button(action: { viewModel.ejectFDD(drive: drive) }) {
                Image(systemName: "eject")
            }
            .help(String(localized: "Eject FDD\(drive)"))
            .disabled(path.isEmpty || viewModel.powerState == .poweringOff)   // P204/S3

            // P271: FDライトプロテクト トグル。南京錠アイコンで現在の保護状態を表す。
            // 空ドライブでも「次に入れるディスクを保護するか」を事前設定できるため
            // path.isEmpty では無効化しない(明示的な製品判断)。
            let isProtected = (drive == 0) ? viewModel.fdd0WriteProtect : viewModel.fdd1WriteProtect
            Button(action: { viewModel.setWriteProtect(drive: drive, protect: !isProtected) }) {
                Image(systemName: isProtected ? "lock.fill" : "lock.open")
                    .frame(width: 16)   // P272: lock.fill/lock.open のグリフ幅差でレイアウトがガタつかないよう固定幅化
            }
            .help(String(localized: "Write-protect FDD\(drive). Turning it ON takes effect immediately, even on an inserted disk. To turn it OFF, eject and re-insert the disk to apply."))
            .disabled(viewModel.powerState == .poweringOff)   // P204/S3
        }
    }

    private func selectDisk(drive: Int) {
        // P673 — 実体は EmulatorViewModel 側の共通メソッドへ移設した
        // (File メニューの「挿入…」と同一導線を共有するため)。挙動は無変更。
        viewModel.browseAndMountFDD(drive: drive)
    }
}
