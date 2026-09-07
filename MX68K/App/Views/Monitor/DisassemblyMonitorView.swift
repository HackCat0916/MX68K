import SwiftUI

/// P740 — 逆アセンブルビューア(⌘⌥A)。任意のゲストアドレスから 1 命令ずつ
/// MC68000 命令を逆アセンブルして表示する読み取り専用モニタ。
///
/// 逆アセンブラ本体は Core 同梱の Debabelizer(`Core/px68k/x68k/d68k.c`、
/// Karl Stenerud 1999)で、元から Xcode の Sources ビルドフェーズに登録されて
/// いながら呼び出し元がゼロのまま埋まっていた「隠れた機能」を P740 で配線した。
///
/// ★`MemoryDumpMonitorView`(P600)との決定的な違い:
/// あちらの `mx68k_read_memory_bytes()` は Bridge 側の実体バッファを直接索く
/// 副作用ゼロの経路だが、こちらの `mx68k_disassemble_line()` は
/// `d68kconf.h` のマクロ定義により最終的に `cpu_readmem24` を通る
/// ——c68k コア本体が実行中に使うのと同じ関数である。そのため:
///   (1) C 側が P600 のリージョン分類で「読める」種別に限定し、I/O 窓・未装着・
///       未マップ窓では逆アセンブラを一切呼ばない(安全ゲートは Bridge 側にある)。
///   (2) Swift 側は必ず `EmulatorEngine.withEmulationLock` で囲む。
///       `cpu_readmem24` は呼ばれる度に無条件で `BusErrFlag` をリセットするが、
///       同じ変数を `dmac.c` / `midi.c` / `scsi.c` が自分の転送結果の判定に
///       実際に読んでいるため、ロック無しに UI スレッドから叩くと
///       それらのサブシステムが自分自身のバスエラー条件を取りこぼしうる。
///       (SCSI MO/CD のライブ操作 P674 と同型の既存メカニズムの横展開。)
///
/// 更新頻度: このウィンドウが開いている間だけ動く 2Hz ポーリング
/// (`MemoryDumpMonitorView` / `SoftKeyboardState` と同型)。
///
/// ★「前ページ」を実装しない理由: 68000 は可変長命令なので、あるアドレスの
/// 手前に何バイトの命令があったかは後方からは一意に決まらない(標準的な
/// 逆アセンブラの既知の制約)。戻りたい場合は Goto Address で明示的に指定する。

/// 逆アセンブル 1 行ぶんの表示データ。
struct DisassemblyLine: Identifiable {
    /// ★行番号を id にする。アドレスは通常一意だが、24bit 空間の折り返しが
    ///   絡む極端なケースで重複しうるため ForEach の id には使わない。
    let id: Int
    let address: UInt32
    /// 生バイト列(命令長ぶん)。読めないバイトは値を捏造せず "--" を出す。
    let bytesText: String
    /// ニーモニック文字列。C 側が拒否した場合は "(unreadable)"。
    let text: String
}

final class DisassemblyModel: ObservableObject {
    /// 1 画面ぶんの行数。可変長命令のため「1 ページ = N バイト」ではなく
    /// 「1 ページ = N 命令」で数える。
    static let linesPerPage = 28
    static let addressMask: UInt32 = 0x00FF_FFFF          // ゲストは 24bit 空間
    /// 生バイト列の表示上限。C 側の安全ゲート(最悪ケース 12 バイト)より
    /// 十分大きく取り、万一それを超える戻り値が来ても配列確保が暴れないようにする。
    private static let maxBytesShown = 16

    /// 起動時の既定位置は IPL-ROM 先頭 — 常に読める領域で、開いた瞬間に
    /// 意味のある逆アセンブル結果が出る。
    @Published var baseAddress: UInt32 = 0xFC0000
    @Published var followPC: Bool = false
    @Published private(set) var lines: [DisassemblyLine] = []
    @Published private(set) var currentPC: UInt32 = 0
    /// 直近に表示した最終行の「次」のアドレス。Next Page の起点。
    @Published private(set) var nextPageAddress: UInt32 = 0xFC0000

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
        var status = MX68KStatus()
        var decoded: [DisassemblyLine] = []
        decoded.reserveCapacity(Self.linesPerPage)
        var cursorEnd: UInt32 = 0

        // ★ロックは 1 リフレッシュにつき 1 回だけ取り、1 画面ぶんのデコードを
        //   まとめてその中で行う(命令ごとに取り直すと 28 回の取得になり、
        //   エミュレーションスレッドを細かく突く回数がそのぶん増える)。
        //   PC の読み取りも同じ区間に入れて、表示中の PC 位置とデコード結果が
        //   同一スナップショットから来ることを保証する。
        engine.withEmulationLock {
            mx68k_get_status(&status)
            var addr = followPC ? (status.pc & Self.addressMask) : baseAddress
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
                let bytesText = bytesParts.joined(separator: " ")

                decoded.append(DisassemblyLine(id: row,
                                               address: addr,
                                               bytesText: bytesText,
                                               text: text))
                // C 側は必ず 2 以上を返す(防御的フロアあり)ので前進は保証される。
                addr = (addr &+ length) & Self.addressMask
            }
            cursorEnd = addr
        }

        currentPC = status.pc & Self.addressMask
        lines = decoded
        nextPageAddress = cursorEnd
    }

    /// 絶対ジャンプ。命令境界は利用者の指定を尊重し、偶数番地へだけ丸める
    /// (68000 の命令は必ず偶数アドレスから始まる)。
    func seek(to address: UInt32, engine: EmulatorEngine) {
        baseAddress = (address & Self.addressMask) & ~UInt32(1)
        refresh(engine: engine)
    }

    /// 直近に表示した最終行の次から続きをデコードする。
    func pageForward(engine: EmulatorEngine) {
        seek(to: nextPageAddress, engine: engine)
    }
}

struct DisassemblyMonitorView: View {
    /// ★`EmulatorViewModel` を観測するのは `engine`(= `withEmulationLock` の供給元)を
    ///   得るためだけ。あちらは毎フレーム更新の `@Published` を多数抱えるため
    ///   このウィンドウも同じ頻度で再描画されるが、これは `CPUMonitorView` 等の
    ///   既存モニタと同じ既存の割り切り。表示値そのものは 2Hz ポーリングで作った
    ///   `model.lines` から来ており、再描画頻度が上がっても逆アセンブル呼出し
    ///   (= `cpu_readmem24` を通る唯一の経路)の回数は増えない。
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    @StateObject private var model = DisassemblyModel()
    @State private var addressText = "FC0000"

    private var engine: EmulatorEngine { emulatorViewModel.engine }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Disassembly").font(.headline)
            controls
            Divider()
            listing
            Spacer(minLength: 0)
            Text("Read-only. Backward paging is not available because 68000 instructions are variable length — use Goto Address to move back.")
                .font(.caption)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 560, minHeight: 520, alignment: .topLeading)
        .onAppear {
            syncAddressText()
            model.startPolling(engine: engine)
        }
        .onDisappear { model.stopPolling() }
    }

    // MARK: - 操作部

    private var controls: some View {
        HStack(spacing: 8) {
            Text("Address $")
            TextField("FC0000", text: $addressText)
                .frame(width: 90)
                .disabled(model.followPC)
                .onSubmit { commitAddress() }
            Button("Goto") { commitAddress() }
                .disabled(model.followPC)
            Button("Next ▶") {
                model.pageForward(engine: engine)
                syncAddressText()
            }
            .disabled(model.followPC)
            Divider().frame(height: 16)
            Toggle("Follow PC", isOn: $model.followPC)
                .onChange(of: model.followPC) { _ in
                    model.refresh(engine: engine)
                    syncAddressText()
                }
            Divider().frame(height: 16)
            Text(String(format: "PC $%06X", model.currentPC))
                .foregroundColor(.secondary)
        }
    }

    // MARK: - 一覧表示

    private var listing: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 0) {
                Text("Address").frame(width: 80, alignment: .leading)
                Text("Bytes").frame(width: 150, alignment: .leading)
                Text("Instruction").frame(maxWidth: .infinity, alignment: .leading)
            }
            .foregroundColor(.secondary)

            ForEach(model.lines) { line in
                HStack(spacing: 0) {
                    Text(String(format: "%06X", line.address))
                        .frame(width: 80, alignment: .leading)
                    Text(line.bytesText)
                        .frame(width: 150, alignment: .leading)
                    Text(line.text)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                // 現在 PC の行を強調する。Follow PC が OFF でも、たまたま画面内に
                // PC が入っていれば同じように光る。
                .foregroundColor(line.address == model.currentPC ? .accentColor : .primary)
                .fontWeight(line.address == model.currentPC ? .bold : .regular)
            }
        }
        .font(.system(size: 12, design: .monospaced))
        .textSelection(.enabled)
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
        model.seek(to: value, engine: engine)
        syncAddressText()
    }

    private func syncAddressText() {
        addressText = String(format: "%06X", model.lines.first?.address ?? model.baseAddress)
    }
}
