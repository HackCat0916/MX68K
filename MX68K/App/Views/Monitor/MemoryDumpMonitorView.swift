import SwiftUI

/// P600 — メモリダンプビューア(⌘⌥2)。任意のゲストアドレスの生バイト列を
/// 16進 + ASCII で表示する読み取り専用モニタ。
///
/// 値の取得は `mx68k_read_memory_bytes()` の 1 本のみで、Core の
/// `MemReadTable` / `cpu_readmem24` は経由しない。あの経路は FDC のデータ FIFO
/// 前進や SASI のホストディスク I/O 起動といった実機同等の副作用を持つため、
/// ダンプを覗いただけでゲスト状態が進んでしまう(詳細は EmulatorBridge.h の
/// `MX68KMemKind` 宣言まわりのコメント)。読めない領域は値を捏造せず、
/// 種別に応じたプレースホルダと誘導先パネル名を出す。
///
/// 更新頻度: このウィンドウが開いている間だけ動く 2Hz ポーリング
/// (SoftKeyboardState の LED ポーリングと同型)。エミュレーションスレッドとは
/// 同期していないため、稀に読み取り途中の値が混ざることがある — CPU/Register
/// Viewer 等の既存モニタと同じ非同期スナップショット前提のデバッグ用途。

/// ジャンプ要求。アドレスが前回と同じでも再ジャンプできるよう世代番号を持つ。
struct MemoryJumpRequest: Equatable {
    let address: UInt32
    let generation: Int
}

/// メモリマップビューア → メモリダンプビューアへ「この番地へ飛べ」を伝える
/// だけの極小オブジェクト。EmulatorViewModel を経由しないのは、あちらが毎フレーム
/// 更新される `@Published` を多数抱えており、観測するとウィンドウ全体が
/// 再描画され続けるため(MouseCaptureState / EmulatorRunState と同じ理由)。
final class MemoryViewerTarget: ObservableObject {
    static let shared = MemoryViewerTarget()
    private init() {}

    @Published private(set) var request: MemoryJumpRequest?
    private var generation = 0

    func jump(to address: UInt32) {
        generation += 1
        request = MemoryJumpRequest(address: address, generation: generation)
    }
}

/// C 側 `MX68KMemKind` の値をそのまま使うための薄いラッパ。
/// 数値はハードコードせず C の定数から取る(EmulatorBridge.h が単一の情報源)。
enum MemoryKind {
    static let flatSwap     = UInt8(MX68K_MEMKIND_FLAT_SWAP.rawValue)
    static let flatRaw      = UInt8(MX68K_MEMKIND_FLAT_RAW.rawValue)
    static let decoded      = UInt8(MX68K_MEMKIND_DECODED.rawValue)
    static let ioUnsafe     = UInt8(MX68K_MEMKIND_IO_UNSAFE.rawValue)
    static let notInstalled = UInt8(MX68K_MEMKIND_NOT_INSTALLED.rawValue)
    static let busErr       = UInt8(MX68K_MEMKIND_BUSERR.rawValue)

    static func isReadable(_ kind: UInt8) -> Bool {
        kind == flatSwap || kind == flatRaw || kind == decoded
    }
}

final class MemoryDumpModel: ObservableObject {
    static let bytesPerRow = 16
    static let rowsPerPage = 16
    static let pageBytes = bytesPerRow * rowsPerPage      // 256
    static let addressMask: UInt32 = 0x00FF_FFFF          // ゲストは 24bit 空間

    @Published var baseAddress: UInt32 = 0
    @Published private(set) var bytes: [UInt8] = []
    @Published private(set) var kinds: [UInt8] = []

    private var timer: Timer?

    func startPolling() {
        stopPolling()
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    func stopPolling() {
        timer?.invalidate()
        timer = nil
    }

    func refresh() {
        let n = Self.pageBytes
        var b = [UInt8](repeating: 0xFF, count: n)
        var k = [UInt8](repeating: MemoryKind.busErr, count: n)
        b.withUnsafeMutableBufferPointer { bp in
            k.withUnsafeMutableBufferPointer { kp in
                mx68k_read_memory_bytes(baseAddress, bp.baseAddress, kp.baseAddress, UInt32(n))
            }
        }
        bytes = b
        kinds = k
    }

    /// 16 バイト境界へ切り下げ、24bit 空間へ丸める。
    func seek(to address: UInt32) {
        baseAddress = (address & Self.addressMask) & ~UInt32(Self.bytesPerRow - 1)
        refresh()
    }

    func pageForward() {
        let next = UInt64(baseAddress) + UInt64(Self.pageBytes)
        seek(to: next > UInt64(Self.addressMask) ? 0 : UInt32(next))
    }

    func pageBackward() {
        if baseAddress < UInt32(Self.pageBytes) {
            seek(to: (Self.addressMask + 1) - UInt32(Self.pageBytes))
        } else {
            seek(to: baseAddress - UInt32(Self.pageBytes))
        }
    }

    /// ページ全体が読み出し不可なら、その種別を返す(混在時は nil = 通常のダンプ表示)。
    var uniformUnreadableKind: UInt8? {
        guard let first = kinds.first, !MemoryKind.isReadable(first) else { return nil }
        return kinds.allSatisfy { $0 == first } ? first : nil
    }
}

struct MemoryDumpMonitorView: View {
    @StateObject private var model = MemoryDumpModel()
    @ObservedObject private var target = MemoryViewerTarget.shared
    @State private var addressText = "000000"

    /// ★タプルの要素へは KeyPath を作れないため(Swift の制約)、`ForEach(id:)` に
    ///   使う一覧は Identifiable な構造体で持つ。
    private struct QuickJump: Identifiable {
        let id: String
        let address: UInt32
    }

    private static let quickJumps: [QuickJump] = [
        QuickJump(id: "RAM",    address: 0x000000),
        QuickJump(id: "GVRAM",  address: 0xC00000),
        QuickJump(id: "TVRAM",  address: 0xE00000),
        QuickJump(id: "SRAM",   address: 0xED0000),
        QuickJump(id: "CGROM",  address: 0xF00000),
        QuickJump(id: "IPLROM", address: 0xFC0000),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Memory").font(.headline)
            controls
            quickJumpBar
            Divider()
            dumpArea
            Spacer(minLength: 0)
            Text("読み取り専用。値の書き換え機能はありません。")
                .font(.caption)
                .foregroundColor(.secondary)
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 660, minHeight: 480, alignment: .topLeading)
        .onAppear {
            model.startPolling()
            applyPendingJump()
        }
        .onDisappear { model.stopPolling() }
        .onChange(of: target.request) { _ in applyPendingJump() }
    }

    // MARK: - 操作部

    private var controls: some View {
        HStack(spacing: 8) {
            Text("Address $")
            TextField("000000", text: $addressText)
                .frame(width: 90)
                .onSubmit { commitAddress() }
            Button("Go") { commitAddress() }
            Divider().frame(height: 16)
            Button("◀ 前") { model.pageBackward(); syncAddressText() }
            Button("次 ▶") { model.pageForward(); syncAddressText() }
            Divider().frame(height: 16)
            Text(String(format: "$%06X - $%06X",
                        model.baseAddress,
                        model.baseAddress &+ UInt32(MemoryDumpModel.pageBytes - 1)))
                .foregroundColor(.secondary)
        }
    }

    private var quickJumpBar: some View {
        HStack(spacing: 6) {
            ForEach(Self.quickJumps) { item in
                Button(item.id) { jump(to: item.address) }
            }
        }
    }

    // MARK: - ダンプ表示

    @ViewBuilder
    private var dumpArea: some View {
        if let kind = model.uniformUnreadableKind {
            unreadablePlaceholder(kind)
        } else {
            VStack(alignment: .leading, spacing: 2) {
                Text(Self.headerLine)
                    .foregroundColor(.secondary)
                ForEach(0..<MemoryDumpModel.rowsPerPage, id: \.self) { row in
                    Text(rowLine(row))
                }
            }
            .font(.system(size: 12, design: .monospaced))
            .textSelection(.enabled)
        }
    }

    private static let headerLine: String = {
        var s = "         "
        // ★String(format:) に Int を渡さない(64bit/32bit の幅違い、P485 の教訓)。
        for i in 0..<MemoryDumpModel.bytesPerRow { s += String(format: "%02X ", UInt32(i)) }
        s += " |ASCII"
        return s
    }()

    private func rowLine(_ row: Int) -> String {
        let start = row * MemoryDumpModel.bytesPerRow
        guard model.bytes.count >= start + MemoryDumpModel.bytesPerRow else { return "" }
        let addr = model.baseAddress &+ UInt32(start)
        var hex = String(format: "%06X   ", addr)
        var ascii = ""
        for i in 0..<MemoryDumpModel.bytesPerRow {
            let idx = start + i
            if MemoryKind.isReadable(model.kinds[idx]) {
                let b = model.bytes[idx]
                hex += String(format: "%02X ", UInt32(b))
                ascii += (b >= 0x20 && b < 0x7F) ? String(UnicodeScalar(b)) : "."
            } else {
                // 読めない範囲は値を捏造せず「--」を出す(0xFF と区別できるようにする)。
                hex += "-- "
                ascii += " "
            }
        }
        return hex + " |" + ascii + "|"
    }

    private func unreadablePlaceholder(_ kind: UInt8) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("――（専用モニタパネルを参照）")
                .font(.title3)
            Text(Self.reason(kind))
            if let hint = Self.panelHint(model.baseAddress, kind) {
                Text(hint).foregroundColor(.secondary)
            }
            Text("この範囲は読み出し自体がハードウェアの状態を進めてしまうため、"
                 + "ダンプビューアからはアクセスしません。")
                .font(.caption)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private static func reason(_ kind: UInt8) -> String {
        if kind == MemoryKind.ioUnsafe     { return "デバイス I/O 窓(読み出しに副作用あり)" }
        if kind == MemoryKind.notInstalled { return "未装着のボード窓" }
        return "未マップ領域(バスエラー窓)"
    }

    /// I/O 窓の番地から、その内容を見るための既存モニタパネルを案内する。
    private static func panelHint(_ addr: UInt32, _ kind: UInt8) -> String? {
        guard kind == MemoryKind.ioUnsafe else { return nil }
        switch addr {
        case 0xE80000..<0xE82000: return "→ CRTC Viewer (⌘⌥4)"
        case 0xE82000..<0xE84000: return "→ Palette Viewer (⌘⌥P) / Video Controller Viewer (⌘⌥5)"
        case 0xE90000..<0xE94000: return "→ Sound Viewer (⌘⌥3)"
        case 0xEB0000..<0xEC0000: return "→ BG Viewer (⌘⌥6) / Sprite Viewer (⌘⌥8)"
        case 0xECC000..<0xECE000: return "→ Sound Viewer (⌘⌥3)  ※Mercury Unit"
        default:                  return nil
        }
    }

    // MARK: - アドレス操作

    private func commitAddress() {
        let cleaned = addressText
            .trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: "$", with: "")
            .replacingOccurrences(of: "0x", with: "")
            .replacingOccurrences(of: "0X", with: "")
        guard let value = UInt32(cleaned, radix: 16) else {
            syncAddressText()   // 解釈できない入力は現在値へ戻す
            return
        }
        model.seek(to: value)
        syncAddressText()
    }

    private func jump(to address: UInt32) {
        model.seek(to: address)
        syncAddressText()
    }

    private func syncAddressText() {
        addressText = String(format: "%06X", model.baseAddress)
    }

    private func applyPendingJump() {
        guard let req = target.request else { return }
        jump(to: req.address)
    }
}
