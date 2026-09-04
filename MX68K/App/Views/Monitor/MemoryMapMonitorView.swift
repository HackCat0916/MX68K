import SwiftUI

/// P600 — メモリマップビューア(⌘⌥R)。ゲスト 24bit 空間の領域構成と、
/// 各領域の現在の状態(読み出し可 / デバイス I/O / 未装着 / 未マップ)を一覧する。
///
/// 装着状態は設定値ではなく **配線確定値** から構成される
/// (`mx68k_get_region_map()` 側で `g_wired_machine_type` /
/// `g_scsi_ext_board_wired` / `g_mercury_installed` / `g_midi_installed` /
/// `sram_ext_is_enabled()` を読む) — 設定を変えてもハードリセットまで実際の
/// 配線は変わらないため、UI の基準を P457 の wired 値基準ゲートと揃えている。
///
/// 帯をクリックするとメモリダンプビューア(⌘⌥2)がその領域の先頭へジャンプする。
struct MemoryRegionRow: Identifiable {
    let id: Int
    let base: UInt32
    let size: UInt32
    let name: String
    let status: Int32

    var endAddress: UInt32 { base &+ size &- 1 }

    var statusLabel: String {
        switch status {
        case 0:  return "読み出し可"
        case 1:  return "I/O(専用モニタ)"
        case 2:  return "未装着"
        default: return "未マップ"
        }
    }

    var statusColor: Color {
        switch status {
        case 0:  return .accentColor
        case 1:  return .gray
        case 2:  return .orange
        default: return .red
        }
    }

    var sizeLabel: String {
        if size >= 1024 * 1024 {
            let mb = Double(size) / (1024.0 * 1024.0)
            return mb == mb.rounded() ? String(format: "%.0fMB", mb) : String(format: "%.1fMB", mb)
        }
        return "\(size / 1024)KB"
    }
}

final class MemoryMapModel: ObservableObject {
    /// C 側リージョン表の上限。実際の件数より十分大きい固定値を渡す。
    private static let maxRegions = 64

    @Published private(set) var regions: [MemoryRegionRow] = []

    private var timer: Timer?

    func startPolling() {
        stopPolling()
        refresh()
        // 装着状態はハードリセット時にしか変わらないので、更新は 1Hz で十分。
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    func stopPolling() {
        timer?.invalidate()
        timer = nil
    }

    func refresh() {
        var buffer = [MX68KRegionInfo](repeating: MX68KRegionInfo(), count: Self.maxRegions)
        let count = buffer.withUnsafeMutableBufferPointer { p in
            Int(mx68k_get_region_map(p.baseAddress, Int32(Self.maxRegions)))
        }
        guard count > 0 else {
            regions = []
            return
        }
        regions = (0..<min(count, Self.maxRegions)).map { i in
            let info = buffer[i]
            return MemoryRegionRow(id: i,
                                   base: info.base,
                                   size: info.size,
                                   name: Self.decodeName(info.name),
                                   status: info.status)
        }
    }

    /// `char name[32]` は Swift へ 32 要素のタプルとして取り込まれるため、
    /// NUL 終端までを UTF-8 として復号する。
    /// ★引数は `Any` ではなくジェネリクスで受ける — `Any` にすると
    ///   `withUnsafeBytes(of:)` が実体ではなく existential のボックスを覗いてしまう。
    private static func decodeName<T>(_ raw: T) -> String {
        withUnsafeBytes(of: raw) { buf in
            let bytes = buf.bindMemory(to: UInt8.self)
            let terminated = Array(bytes.prefix(while: { $0 != 0 }))
            return String(decoding: terminated, as: UTF8.self)
        }
    }
}

struct MemoryMapMonitorView: View {
    @StateObject private var model = MemoryMapModel()
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Memory Map").font(.headline)
            Text("帯をクリックするとメモリビューア(⌘⌥2)がその領域の先頭へジャンプします。")
                .font(.caption)
                .foregroundColor(.secondary)
            Divider()
            ScrollView {
                VStack(spacing: 3) {
                    ForEach(model.regions) { region in
                        regionBar(region)
                    }
                }
            }
            legend
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 560, minHeight: 520, alignment: .topLeading)
        .onAppear { model.startPolling() }
        .onDisappear { model.stopPolling() }
    }

    private func regionBar(_ region: MemoryRegionRow) -> some View {
        Button {
            // 未オープンならここで開き、開いていれば前面化される。ジャンプ先は
            // 共有オブジェクト経由で伝える(SwiftUI の openWindow はウィンドウへ
            // 任意の値を渡す口を持たないため)。
            MemoryViewerTarget.shared.jump(to: region.base)
            openWindow(id: "monitor-memory")
        } label: {
            HStack(spacing: 10) {
                Rectangle()
                    .fill(region.statusColor)
                    .frame(width: 6)
                Text(String(format: "$%06X-$%06X", region.base, region.endAddress))
                Text(region.sizeLabel)
                    .frame(width: 64, alignment: .trailing)
                    .foregroundColor(.secondary)
                Text(region.name)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Text(region.statusLabel)
                    .font(.caption)
                    .foregroundColor(region.statusColor)
            }
            .frame(height: 26)
            .background(region.statusColor.opacity(0.12))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private var legend: some View {
        HStack(spacing: 14) {
            legendItem(.accentColor, "読み出し可")
            legendItem(.gray, "I/O(専用モニタ)")
            legendItem(.orange, "未装着")
            legendItem(.red, "未マップ")
        }
        .font(.caption)
        .foregroundColor(.secondary)
    }

    private func legendItem(_ color: Color, _ label: String) -> some View {
        HStack(spacing: 4) {
            Rectangle().fill(color).frame(width: 10, height: 10)
            Text(label)
        }
    }
}
