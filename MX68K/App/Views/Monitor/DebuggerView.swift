import SwiftUI

/// P748 — 実行制御デバッガ(⌘⌥J)。単一 PC ブレークポイント + 1 命令ステップ実行。
///
/// 本プロジェクト初の「読取専用でないモニタ」である。既存 25 枚のモニタは
/// すべて観測専用だったが、このウィンドウだけは CPU の実行そのものを止める。
///
/// ★機構は `Bridge/m68000_bridge.c` の `m68000_execute()` チャンクループにある:
///   武装中に限りチャンク予算 `c` を 1 に縮めて単命令粒度にし、各命令の直前で
///   ゲスト PC を武装アドレスと比較する。**無武装時(既定)はフラグ 1 回の
///   ロードだけで抜けるので、ゲスト実行はバイト同一**(Core 無改変)。
///
/// ★`DisassemblyMonitorView`(P740)と同じ作法を踏襲する:
///   - 逆アセンブルは `cpu_readmem24` を通る経路なので必ず
///     `EmulatorEngine.withEmulationLock` で囲む。
///   - 1 リフレッシュにつきロックは 1 回だけ取り、停止状態・レジスタ・
///     逆アセンブル 1 画面ぶんを**まとめて**その中で取得する。これにより
///     画面上の全欄が同一スナップショット由来であることが保証される
///     (P373 の「派生値と生値の鮮度がずれる」欠陥を繰り返さないため)。
///
/// ★自己反証可能性(Fix Plan §自己反証可能性): 「止まらない」ときに
///   原因を画面だけで一意に切り分けられるよう、状態(armed / bp_addr)・
///   分子(hits)・**分母(stepped chunks)**の 3 点を常時表示する。
///   分母が増えていれば機構は生きており「そのアドレスをゲストが実行して
///   いない」と確定でき、増えていなければ機構側の問題(または CPU が
///   HALTED)だと分かる。
///   また `bp_addr`(比較対象)と `stop_pc`(停止時に実際に読まれた生 PC)は
///   **別欄**に並べ、意図どおりのアドレスで止まったことを標本自身が示す。

/// 逆アセンブル 1 行ぶんの表示データ(P740 の `DisassemblyLine` と同型だが、
/// あちらの型をそのまま借りると片方の変更が他方へ波及するので独立させる)。
struct DebuggerDisasmLine: Identifiable {
    let id: Int
    let address: UInt32
    let bytesText: String
    let text: String
}

final class DebuggerModel: ObservableObject {
    /// 逆アセンブル表示行数。P740 の 28 行より小さいのは、レジスタ欄・
    /// 操作欄と同居させるため。
    static let linesPerPage = 12
    static let addressMask: UInt32 = 0x00FF_FFFF          // ゲストは 24bit 空間
    /// 生バイト列の表示上限(P740 と同じ防御的な上限)。
    private static let maxBytesShown = 16

    @Published private(set) var debug = MX68KDebugStatus()
    @Published private(set) var cpu = MX68KStatus()
    @Published private(set) var lines: [DebuggerDisasmLine] = []
    /// 逆アセンブルの起点として実際に使ったアドレス(= 表示時点の PC)。
    @Published private(set) var listingOrigin: UInt32 = 0

    private var timer: Timer?

    func startPolling(engine: EmulatorEngine) {
        stopPolling()
        refresh(engine: engine)
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self, weak engine] _ in
            guard let self, let engine else { return }
            self.refresh(engine: engine)
        }
    }

    func stopPolling() {
        timer?.invalidate()
        timer = nil
    }

    func refresh(engine: EmulatorEngine) {
        var dbg = MX68KDebugStatus()
        var status = MX68KStatus()
        var decoded: [DebuggerDisasmLine] = []
        decoded.reserveCapacity(Self.linesPerPage)
        var origin: UInt32 = 0

        // ★ロックは 1 リフレッシュにつき 1 回。停止状態・CPU レジスタ・
        //   逆アセンブル 1 画面ぶんを同一区間でまとめて取る。
        engine.withEmulationLock {
            mx68k_debug_get_status(&dbg)
            mx68k_get_status(&status)

            // 逆アセンブルの起点は常に現在の PC(停止中は停止 PC と一致する)。
            var addr = status.pc & Self.addressMask
            origin = addr
            var textBuf = [CChar](repeating: 0, count: 256)

            for row in 0..<Self.linesPerPage {
                let length = textBuf.withUnsafeMutableBufferPointer { p in
                    mx68k_disassemble_line(addr, p.baseAddress, UInt32(p.count))
                }
                let text = String(cString: textBuf)

                // 生バイト列は副作用ゼロの P600 経路から取る(逆アセンブラ経路とは別)。
                let count = max(1, min(Int(length), Self.maxBytesShown))
                var raw = [UInt8](repeating: 0xFF, count: count)
                var kinds = [UInt8](repeating: MemoryKind.busErr, count: count)
                raw.withUnsafeMutableBufferPointer { rp in
                    kinds.withUnsafeMutableBufferPointer { kp in
                        mx68k_read_memory_bytes(addr, rp.baseAddress, kp.baseAddress, UInt32(count))
                    }
                }
                var bytesParts: [String] = []
                bytesParts.reserveCapacity(count)
                for i in 0..<count {
                    bytesParts.append(MemoryKind.isReadable(kinds[i])
                                      ? String(format: "%02X", UInt32(raw[i]))
                                      : "--")
                }

                decoded.append(DebuggerDisasmLine(id: row,
                                                  address: addr,
                                                  bytesText: bytesParts.joined(separator: " "),
                                                  text: text))
                // C 側は必ず 2 以上を返す(防御的フロアあり)ので前進は保証される。
                addr = (addr &+ length) & Self.addressMask
            }
        }

        debug = dbg
        cpu = status
        lines = decoded
        listingOrigin = origin
    }
}

struct DebuggerView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    @StateObject private var model = DebuggerModel()
    @State private var addressText = "FC0000"

    private var engine: EmulatorEngine { emulatorViewModel.engine }
    private var isStopped: Bool { model.debug.stopped != 0 }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Debugger").font(.headline)
            statusLine
            Divider()
            breakpointControls
            executionControls
            Divider()
            listing
            Divider()
            registers
            Spacer(minLength: 0)
            Text("Step may land on an exception entry when an interrupt is taken at the instruction boundary — the stopped PC and the listing below always show where execution actually is.")
                .font(.caption)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 700, minHeight: 620, alignment: .topLeading)
        .onAppear {
            syncAddressText()
            model.startPolling(engine: engine)
        }
        .onDisappear { model.stopPolling() }
    }

    // MARK: - 状態行

    private var statusLine: some View {
        HStack(spacing: 12) {
            Text(stateText)
                .fontWeight(.bold)
                .foregroundColor(isStopped ? .orange : .green)
            if model.debug.cpu_halted != 0 {
                Text("CPU: HALTED/WAITING")
                    .font(.caption)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(Color.secondary.opacity(0.2))
                    .cornerRadius(4)
            }
            Spacer()
        }
    }

    /// ★`String(format:)` で組んだプレーンな `String` を `Text(_:)` へ渡すと
    ///   `StringProtocol` オーバーロードへ解決され、カタログ参照が一切行われない
    ///   (P745 で一度発見・修正した落とし穴と同型)。呼出し側で
    ///   `String(localized:)` を構築しておくことで実際にローカライズが効く。
    private var stateText: String {
        guard isStopped else { return String(localized: "Running") }
        return String(localized: "Stopped: \(reasonText) @ $\(String(format: "%06X", model.debug.stop_pc))")
    }

    private var reasonText: String {
        switch model.debug.stop_reason {
        case Int32(MX68K_DEBUG_STOP_BREAKPOINT): return String(localized: "BREAKPOINT")
        case Int32(MX68K_DEBUG_STOP_STEP):       return String(localized: "STEP")
        case Int32(MX68K_DEBUG_STOP_HALTED):     return String(localized: "CPU HALTED")
        default:                                 return String(localized: "NONE")
        }
    }

    // MARK: - ブレークポイント操作 + 自己反証 3 点

    private var breakpointControls: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Text("Breakpoint $")
                TextField("FC0000", text: $addressText)
                    .frame(width: 90)
                    .onSubmit { commitBreakpoint() }
                Button("Set") { commitBreakpoint() }
                Button("Clear") { emulatorViewModel.debuggerClearBreakpoint() }
                    .disabled(model.debug.armed == 0)
                Spacer()
            }
            // ★自己反証の 3 点。armed(状態)/ hits(分子)/ stepped chunks(分母)。
            //   bp_addr と stop_pc は別欄として並べる。
            HStack(spacing: 16) {
                Text(model.debug.armed != 0
                     ? String(localized: "armed: yes  bp_addr $\(String(format: "%06X", model.debug.bp_addr))")
                     : String(localized: "armed: no"))
                Text("hits: \(model.debug.bp_hit_count)")
                Text("stepped chunks: \(model.debug.armed_chunks)")
                Text("steps: \(model.debug.step_count)")
                Text(isStopped
                     ? String(localized: "stop_pc $\(String(format: "%06X", model.debug.stop_pc))")
                     : String(localized: "stop_pc —"))
                Spacer()
            }
            .font(.system(size: 11, design: .monospaced))
            .foregroundColor(.secondary)
        }
    }

    // MARK: - 実行制御ボタン

    private var executionControls: some View {
        HStack(spacing: 8) {
            Button("Step") { emulatorViewModel.debuggerStep() }
                .disabled(!isStopped)
            Button("Continue") { emulatorViewModel.debuggerContinue() }
                .disabled(!isStopped)
            Button("Break Now") { emulatorViewModel.debuggerBreakNow() }
                .disabled(isStopped)
            Spacer()
        }
    }

    // MARK: - 逆アセンブル

    private var listing: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 0) {
                Text("Address").frame(width: 80, alignment: .leading)
                Text("Bytes").frame(width: 260, alignment: .leading)
                Text("Instruction").frame(maxWidth: .infinity, alignment: .leading)
            }
            .foregroundColor(.secondary)

            ForEach(model.lines) { line in
                HStack(spacing: 0) {
                    Text(String(format: "%06X", line.address))
                        .frame(width: 80, alignment: .leading)
                    Text(line.bytesText)
                        .frame(width: 260, alignment: .leading)
                    Text(line.text)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                // 先頭行 = 現在の PC。停止中はそこが停止位置そのもの。
                .foregroundColor(line.address == model.listingOrigin ? .accentColor : .primary)
                .fontWeight(line.address == model.listingOrigin ? .bold : .regular)
            }
        }
        .font(.system(size: 12, design: .monospaced))
        .textSelection(.enabled)
    }

    // MARK: - レジスタ(読取専用)

    private var registers: some View {
        // ★MX68KStatus.d/a は C の uint32_t[8] → Swift ではタプルとして渡るため
        //   添字アクセスできない。CPUMonitorView.swift:13-14 と同じ定石で展開する。
        let s = model.cpu
        let dRegs = [s.d.0, s.d.1, s.d.2, s.d.3, s.d.4, s.d.5, s.d.6, s.d.7]
        let aRegs = [s.a.0, s.a.1, s.a.2, s.a.3, s.a.4, s.a.5, s.a.6, s.a.7]
        return VStack(alignment: .leading, spacing: 2) {
            Text(String(format: "PC $%06X   SR $%04X", s.pc & DebuggerModel.addressMask, UInt32(s.sr)))
            Text("D " + dRegs.map { String(format: "%08X", $0) }.joined(separator: " "))
            Text("A " + aRegs.map { String(format: "%08X", $0) }.joined(separator: " "))
        }
        .font(.system(size: 11, design: .monospaced))
        .textSelection(.enabled)
    }

    // MARK: - アドレス入力

    private func commitBreakpoint() {
        let cleaned = addressText
            .trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: "$", with: "")
            .replacingOccurrences(of: "0x", with: "")
            .replacingOccurrences(of: "0X", with: "")
        guard let value = UInt32(cleaned, radix: 16) else {
            syncAddressText()   // 解釈できない入力は現在値へ戻す(武装はしない)
            return
        }
        // 68000 の命令は必ず偶数アドレスから始まるので偶数へ丸める
        // (奇数を武装すると PC と永久に一致せず「止まらない」原因になる)。
        let addr = (value & DebuggerModel.addressMask) & ~UInt32(1)
        emulatorViewModel.debuggerSetBreakpoint(addr)
        addressText = String(format: "%06X", addr)
    }

    private func syncAddressText() {
        let s = emulatorViewModel.debuggerStatus()
        if s.armed != 0 {
            addressText = String(format: "%06X", s.bp_addr)
        }
    }
}
