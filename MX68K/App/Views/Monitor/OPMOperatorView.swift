import SwiftUI

/// P933 — OPM(YM2151)オペレータパネル(独立ウィンドウ、`Docs/03` §5.2 Section B の実装)。
///
/// 8ch × 4OP の詳細パラメータ(DT1/MUL/TL/KS/AR/AMS-EN/D1R/DT2/D2R/D1L/RR)の一覧と、
/// 選択チャンネルの 4OP 分のエンベロープ理論形状の模式図を表示する。
///
/// ★EG 図は静的パラメータから描いた理論上の形状であり、実チップの EG 位相・出力レベルではない。
/// fmgen の EG 動的値は Core 内部に閉じており Bridge から到達できないため(P933 投資調査§3)。
///
/// 更新頻度: 専用の可視性フラグ(EmulatorEngine.opmOperatorVisible)で駆動される毎フレーム更新。
struct OPMOperatorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    @State private var selectedChannel = 0

    /// C の固定長配列はタプルとして取り込まれるため配列へ展開する(OPMSynthesizerView と同じ手法)。
    private var channels: [MX68K_OPMOperatorChannel] {
        withUnsafeBytes(of: emulatorViewModel.opmOperatorStatus.ch) { raw in
            Array(raw.bindMemory(to: MX68K_OPMOperatorChannel.self))
        }
    }

    private static func bytes<T>(_ tuple: T) -> [UInt8] {
        withUnsafeBytes(of: tuple) { Array($0.bindMemory(to: UInt8.self)) }
    }

    private static func flags4(_ tuple: (Bool, Bool, Bool, Bool)) -> [Bool] {
        [tuple.0, tuple.1, tuple.2, tuple.3]
    }

    /// 配列インデックス 0-3 = M1/C1/M2/C2(EmulatorBridge.h の MX68K_OPMOperatorChannel のコメント)。
    private static let opNames = ["M1", "C1", "M2", "C2"]
    private static let opColors: [Color] = [.red, .orange, .green, .blue]
    private static let channelColors: [Color] = [
        .red, .orange, .yellow, .green, .cyan, .blue, .purple, .pink,
    ]
    private static let paramHeaders = ["DT1", "MUL", "TL", "KS", "AR", "AMS", "D1R", "DT2", "D2R", "D1L", "RR"]
    private static let opColumnWidth: CGFloat = 28
    private static let paramColumnWidth: CGFloat = 30

    /// 1 オペレータ分の表示用パラメータ。
    private struct OperatorParams {
        let written: Bool
        let values: [UInt8]   // paramHeaders と同じ並び
        let tl: UInt8, ar: UInt8, d1r: UInt8, d1l: UInt8, d2r: UInt8, rr: UInt8
    }

    private static func operators(_ c: MX68K_OPMOperatorChannel) -> [OperatorParams] {
        let written = flags4(c.written)
        let dt1 = bytes(c.dt1), mul = bytes(c.mul), tl = bytes(c.tl), ks = bytes(c.ks)
        let ar = bytes(c.ar), amsen = bytes(c.amsen), d1r = bytes(c.d1r), dt2 = bytes(c.dt2)
        let d2r = bytes(c.d2r), d1l = bytes(c.d1l), rr = bytes(c.rr)
        return (0..<4).map { i in
            OperatorParams(written: written[i],
                           values: [dt1[i], mul[i], tl[i], ks[i], ar[i], amsen[i],
                                    d1r[i], dt2[i], d2r[i], d1l[i], rr[i]],
                           tl: tl[i], ar: ar[i], d1r: d1r[i], d1l: d1l[i], d2r: d2r[i], rr: rr[i])
        }
    }

    // MARK: - 本体

    var body: some View {
        let chs = channels
        let algs = Self.bytes(emulatorViewModel.opmOperatorStatus.alg)
        let fbs = Self.bytes(emulatorViewModel.opmOperatorStatus.fb)
        let keyons = Self.bytes(emulatorViewModel.opmOperatorStatus.keyon)
        let sel = min(max(selectedChannel, 0), 7)

        return VStack(alignment: .leading, spacing: 6) {
            Text("OPM Operator (YM2151 FM 8ch × 4OP)").font(.headline)
            Divider()
            ScrollView(.vertical, showsIndicators: true) {
                VStack(alignment: .leading, spacing: 10) {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 400), alignment: .topLeading)],
                              alignment: .leading, spacing: 10) {
                        ForEach(0..<8, id: \.self) { idx in
                            channelBlock(index: idx, ops: Self.operators(chs[idx]),
                                         alg: algs[idx], fb: fbs[idx], keyon: keyons[idx],
                                         selected: idx == sel)
                                .contentShape(Rectangle())
                                .onTapGesture { selectedChannel = idx }
                        }
                    }
                    Divider()
                    envelopeSection(channel: sel, ops: Self.operators(chs[sel]))
                }
            }
            Divider()
            footnotes
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 560, minHeight: 400, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.opmOperatorVisible = true }
        .onDisappear { emulatorViewModel.engine.opmOperatorVisible = false }
    }

    // MARK: - 1ch 分のパラメータ表

    private func channelBlock(index: Int, ops: [OperatorParams], alg: UInt8, fb: UInt8,
                              keyon: UInt8, selected: Bool) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            HStack(spacing: 10) {
                Text(verbatim: "CH\(index + 1)")
                    .foregroundColor(keyon != 0 ? Self.channelColors[index] : .primary)
                Text(verbatim: "ALG:\(alg)")
                Text(verbatim: "FB:\(fb)")
            }
            .font(.system(.caption, design: .monospaced).bold())
            HStack(spacing: 0) {
                Text(verbatim: "OP").frame(width: Self.opColumnWidth, alignment: .leading)
                ForEach(Self.paramHeaders, id: \.self) { h in
                    Text(verbatim: h).frame(width: Self.paramColumnWidth, alignment: .trailing)
                }
            }
            .foregroundColor(.secondary)
            ForEach(0..<4, id: \.self) { i in
                HStack(spacing: 0) {
                    Text(verbatim: Self.opNames[i])
                        .foregroundColor((keyon >> UInt8(i)) & 1 != 0 ? Self.opColors[i] : .primary)
                        .frame(width: Self.opColumnWidth, alignment: .leading)
                    ForEach(0..<Self.paramHeaders.count, id: \.self) { p in
                        Text(verbatim: ops[i].written ? String(ops[i].values[p]) : "--")
                            .frame(width: Self.paramColumnWidth, alignment: .trailing)
                    }
                }
            }
        }
        .font(.system(.caption, design: .monospaced))
        .padding(6)
        .overlay(RoundedRectangle(cornerRadius: 4)
            .stroke(selected ? Color.accentColor : Color.gray.opacity(0.4),
                    lineWidth: selected ? 2 : 1))
    }

    // MARK: - EG 模式図

    private func envelopeSection(channel: Int, ops: [OperatorParams]) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 12) {
                Text("Envelope (schematic)").font(.subheadline.bold())
                Picker("Channel", selection: $selectedChannel) {
                    ForEach(0..<8, id: \.self) { i in
                        Text(verbatim: "CH\(i + 1)").tag(i)
                    }
                }
                .frame(width: 180)
            }
            HStack(spacing: 12) {
                ForEach(0..<4, id: \.self) { i in
                    HStack(spacing: 4) {
                        Rectangle().fill(Self.opColors[i]).frame(width: 14, height: 3)
                        Text(verbatim: Self.opNames[i])
                    }
                }
            }
            .font(.system(.caption, design: .monospaced))
            Canvas { ctx, size in
                drawEnvelopes(ctx: ctx, size: size, ops: ops)
            }
            .frame(height: 160)
            .background(Color.gray.opacity(0.08))
            .overlay(Rectangle().stroke(Color.gray.opacity(0.4), lineWidth: 1))
        }
    }

    /// 模式図の時間軸(単位は無次元の相対値)。キーオフ位置は固定。
    private static let keyOffTime: Double = 100
    private static let totalTime: Double = 140
    private static let silentDB: Double = 96

    /// レート 1-31 で全域(96dB)を変化させるのに要する相対時間。レートが 1 増えるごとに
    /// 線形に短くなる簡易写像(実チップは指数的)。レート 0 は変化しない。
    private static func speed(rate: Int) -> Double {
        guard rate > 0 else { return 0 }
        return silentDB / Double(32 - min(rate, 31))
    }

    /// 1 OP 分の理論エンベロープ(x = 相対時間、y = 減衰量 dB、0 = 最大)。
    /// TL を最大音量位置、D1L を TL からの追加減衰(3dB 刻み、15 は無音扱い)として扱う。
    private static func envelopePoints(tl: Int, ar: Int, d1r: Int, d1l: Int, d2r: Int, rr: Int) -> [(Double, Double)] {
        let peak = min(silentDB, Double(tl) * 0.75)
        let sustain = d1l >= 15 ? silentDB : min(silentDB, peak + Double(d1l) * 3)
        var t = 0.0
        var a = silentDB
        var pts: [(Double, Double)] = [(t, a)]

        func segment(target: Double, speed v: Double) {
            guard t < keyOffTime else { return }
            guard a != target else { return }
            guard v > 0 else {
                t = keyOffTime
                pts.append((t, a))
                return
            }
            let dur = abs(target - a) / v
            if t + dur >= keyOffTime {
                let step = (keyOffTime - t) * v
                a += target > a ? step : -step
                t = keyOffTime
            } else {
                t += dur
                a = target
            }
            pts.append((t, a))
        }

        segment(target: peak, speed: speed(rate: ar))
        segment(target: sustain, speed: speed(rate: d1r))
        segment(target: silentDB, speed: speed(rate: d2r))
        if t < keyOffTime {
            t = keyOffTime
            pts.append((t, a))
        }
        // RR(0-15)は実効レート 2*RR+1 相当として同じ写像に載せる。
        let vr = speed(rate: rr * 2 + 1)
        let rel = (silentDB - a) / vr
        if rel > 0 {
            let end = min(totalTime, t + rel)
            let endA = a + (end - t) * vr
            pts.append((end, min(silentDB, endA)))
        }
        if pts.last!.0 < totalTime {
            pts.append((totalTime, pts.last!.1))
        }
        return pts
    }

    private func drawEnvelopes(ctx: GraphicsContext, size: CGSize, ops: [OperatorParams]) {
        let inset: CGFloat = 6
        let w = size.width - inset * 2
        let h = size.height - inset * 2
        func x(_ t: Double) -> CGFloat { inset + CGFloat(t / Self.totalTime) * w }
        func y(_ db: Double) -> CGFloat { inset + CGFloat(db / Self.silentDB) * h }

        var koff = Path()
        koff.move(to: CGPoint(x: x(Self.keyOffTime), y: inset))
        koff.addLine(to: CGPoint(x: x(Self.keyOffTime), y: inset + h))
        ctx.stroke(koff, with: .color(.secondary), style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
        ctx.draw(Text("Key Off").font(.caption2).foregroundColor(.secondary),
                 at: CGPoint(x: x(Self.keyOffTime) + 4, y: inset + 2), anchor: .topLeading)

        for i in 0..<4 where ops[i].written {
            let o = ops[i]
            let pts = Self.envelopePoints(tl: Int(o.tl), ar: Int(o.ar), d1r: Int(o.d1r),
                                          d1l: Int(o.d1l), d2r: Int(o.d2r), rr: Int(o.rr))
            var path = Path()
            for (n, p) in pts.enumerated() {
                let pt = CGPoint(x: x(p.0), y: y(p.1))
                if n == 0 { path.move(to: pt) } else { path.addLine(to: pt) }
            }
            ctx.stroke(path, with: .color(Self.opColors[i]), lineWidth: 1.5)
        }
    }

    // MARK: - 注記

    private var footnotes: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("※ エンベロープ図は設定パラメータ(TL/AR/D1R/D1L/D2R/RR)から描いた理論上の模式図であり、実際の発音タイミング・位相・音量とは一致しません")
            Text("※ 簡易モデルのため KS・LFO(AMS)・CSM は考慮していません。横軸はレートを線形に割り当てた相対尺度で、キーオフ位置は固定です")
            Text("※ AR/D1R/D2R(0-31)・RR(0-15)はレジスタの生値です。OP 列は信号の流れ順(M1/C1/M2/C2)で、レジスタのスロット順(M1/M2/C1/C2)とは異なります")
            Text("※ 「--」はそのオペレータのレジスタにまだ一度も書込みが無いことを示します")
            Text("※ CSM(reg $14)使用時はキーオン表示が実チップと一致しない場合があります")
        }
        .font(.caption)
        .foregroundColor(.secondary)
    }
}
