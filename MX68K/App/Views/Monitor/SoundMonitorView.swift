import SwiftUI

/// P484/P485 — サウンドモニタ(Docs/03_GUI設計.md §5.2 Section A/B/C 相当)。
/// P484 で ADPCM(MSM6258)セクション、P485 で OPM(YM2151)8ch の
/// コンパクト表示(音名 + キーオン + ALG/FB/PAN)を実装した。
///
/// 更新頻度: 既存9モニタの1秒ゲートではなく、このパネルが表示されている間だけ
/// 動く 0.1 秒ゲート(EmulatorEngine.soundMonitorVisible)で駆動される。
struct SoundMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    /// C の固定長配列は Swift へタプルとして取り込まれるため、描画用に [Int16] へ展開する。
    private var samples: [Int16] {
        withUnsafeBytes(of: emulatorViewModel.adpcmStatus.waveform) { raw in
            Array(raw.bindMemory(to: Int16.self))
        }
    }

    /// P485 — C の固定長配列 `MX68K_OPMChannel ch[8]` は Swift へタプルとして
    /// 取り込まれるため、行の反復用に配列へ展開する。
    private var opmChannels: [MX68K_OPMChannel] {
        withUnsafeBytes(of: emulatorViewModel.opmStatus.ch) { raw in
            Array(raw.bindMemory(to: MX68K_OPMChannel.self))
        }
    }

    /// P491 — `MX68K_MercuryOPNChannel fm[6]` / `MX68K_MercurySSGChannel ssg[3]` も
    /// 同じくタプルとして取り込まれるため配列へ展開する。
    private var mercuryFMChannels: [MX68K_MercuryOPNChannel] {
        withUnsafeBytes(of: emulatorViewModel.mercuryOPNStatus.fm) { raw in
            Array(raw.bindMemory(to: MX68K_MercuryOPNChannel.self))
        }
    }

    private var mercurySSGChannels: [MX68K_MercurySSGChannel] {
        withUnsafeBytes(of: emulatorViewModel.mercuryOPNStatus.ssg) { raw in
            Array(raw.bindMemory(to: MX68K_MercurySSGChannel.self))
        }
    }

    var body: some View {
        let s = emulatorViewModel.adpcmStatus
        // ★ViewBuilder の子ビュー上限(10)に収めるため、セクション単位の
        //   computed property に分割している。
        VStack(alignment: .leading, spacing: 8) {
            Text("ADPCM (MSM6258)").font(.headline)
            Divider()
            statusSection(s)
            Divider()
            waveformSection(s)
            Divider()
            opmSection
            Divider()
            // P635 — Mercury は OPN + PCM の 2 セクションだが、親 VStack の子は
            //   1 個のまま(mercurySection が内部でまとめている)。
            mercurySection
            // ★ViewBuilder の子ビュー上限(10)ぎりぎりのため、P549 セクションは
            //   自前の Divider を内包させて子 1 個に収めている。
            audioBufferSection
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 380, minHeight: 620, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.soundMonitorVisible = true }
        .onDisappear { emulatorViewModel.engine.soundMonitorVisible = false }
    }

    /// 再生状態・サンプルレート・PAN。
    private func statusSection(_ s: MX68K_ADPCMStatus) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 24) {
                Text(s.playing ? "Status: PLAYING" : "Status: STOPPED")
                    .foregroundColor(s.playing ? .green : .secondary)
                Text("DMA: \(s.dma_active ? "ACTIVE" : "idle")")
            }
            HStack(spacing: 24) {
                Text("Rate: \(s.sample_rate_hz)Hz")
                Text("Clock: \(s.clock_divider)")
            }
            // PAN は PPI ポートC bit0-3。bit0/bit1 は「0 で出力有効」の負論理
            // (Core/px68k/x68k/adpcm.c:222-229)、bit2-3 はクロック選択に回る。
            HStack(spacing: 24) {
                Text("PAN: 0x" + String(s.pan, radix: 16, uppercase: true))
                Text("L: \((s.pan & 2) == 0 ? "on" : "off")")
                Text("R: \((s.pan & 1) == 0 ? "on" : "off")")
            }
        }
    }

    /// 波形プレビューとピークレベルメータ。
    private func waveformSection(_ s: MX68K_ADPCMStatus) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Waveform (直近256サンプル / Lch)")
            waveformView
                .frame(height: 96)
                .background(Color.black.opacity(0.85))
                .clipShape(RoundedRectangle(cornerRadius: 4))
            Text("Peak Level")
            levelMeter(peak: s.peak_level)
                .frame(height: 12)
        }
    }

    /// 折れ線による単純な波形プレビュー。縦軸は Int16 フルスケール固定
    /// (自動ゲインをかけると無音とフルスケールが同じ見た目になるため)。
    private var waveformView: some View {
        Canvas { context, size in
            let data = samples
            guard data.count > 1, size.width > 0, size.height > 0 else { return }
            let midY = size.height / 2
            let scale = midY / 32768.0
            let dx = size.width / CGFloat(data.count - 1)

            var path = Path()
            for (i, v) in data.enumerated() {
                let x = CGFloat(i) * dx
                let y = midY - CGFloat(v) * scale
                if i == 0 { path.move(to: CGPoint(x: x, y: y)) }
                else { path.addLine(to: CGPoint(x: x, y: y)) }
            }
            // 中央線(0レベル)
            var zero = Path()
            zero.move(to: CGPoint(x: 0, y: midY))
            zero.addLine(to: CGPoint(x: size.width, y: midY))
            context.stroke(zero, with: .color(.gray.opacity(0.5)), lineWidth: 0.5)
            context.stroke(path, with: .color(.green), lineWidth: 1)
        }
    }

    /// P485 — OPM(YM2151)8ch のコンパクト表示。値は全て CPU がチップへ書いた
    /// レジスタ値のシャドウから導出しており、Core 内部の実行時状態(EG位相など)は
    /// 読めない(OPM::GetReg は宣言のみで未定義)。
    /// ★ViewBuilder の子ビュー上限(10)に収めるため、行の生成は opmRow(_:_:) へ分けている。
    private var opmSection: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("OPM (YM2151 FM 8ch)").font(.headline)
            ForEach(Array(opmChannels.enumerated()), id: \.offset) { idx, c in
                opmRow(index: idx, ch: c)
            }
            // 既知の制約(Bridge/EmulatorBridge.c の mx68k_get_opm_status() 参照)。
            Text("※ CSM(reg $14)使用時はキーオン表示が実チップと一致しない場合があります")
                .font(.caption)
                .foregroundColor(.secondary)
            // P631 — P630 でここに置いていたピアノ鍵盤ビューは、独立ウィンドウ
            // (OPMSynthesizerView、⌘⌥Y)へ移設した。このセクションは P630 以前と
            // 同じテキスト一覧のみの構成に戻っている。
        }
    }

    /// 1チャンネル分の行: `CH1 ON  C#4  ALG:3 FB:5 PAN:LR`
    private func opmRow(index: Int, ch c: MX68K_OPMChannel) -> some View {
        HStack(spacing: 12) {
            Text("CH\(index + 1)")
            Text(c.keyon != 0 ? "ON " : "---")
                .foregroundColor(c.keyon != 0 ? .green : .secondary)
            Text(Self.noteName(note: c.note, octave: c.octave))
                .frame(width: 36, alignment: .leading)
            Text("ALG:\(c.alg)")
            Text("FB:\(c.fb)")
            Text("PAN:\(Self.panName(c.pan))")
        }
        .font(.system(.caption, design: .monospaced))
    }

    /// P491 — Mercury Unit の FM 部(YMF288 = OPN3-L)。OPM セクションと同じく、値は全て
    /// CPU がチップへ書いたレジスタ値のシャドウ由来(Y288::GetReg は FM/リズムに対して
    /// 常に 0 を返すため、チップからは読み戻せない)。
    /// ★ゲストが到達できるのは 2 個ある YMF288 のうち 1 個だけ(上流 px68k 由来の既知の
    ///   制約)なので、表示も FM 6ch + SSG 3ch の 1 チップ分に限る。
    private var mercuryOPNSection: some View {
        let m = emulatorViewModel.mercuryOPNStatus
        return VStack(alignment: .leading, spacing: 4) {
            Text("Mercury OPN (YMF288)").font(.headline)
            if !m.installed {
                Text("Mercury Unit 未装着")
                    .font(.caption)
                    .foregroundColor(.secondary)
            } else {
                ForEach(Array(mercuryFMChannels.enumerated()), id: \.offset) { idx, c in
                    mercuryFMRow(index: idx, ch: c)
                }
                ForEach(Array(mercurySSGChannels.enumerated()), id: \.offset) { idx, c in
                    mercurySSGRow(index: idx, ch: c)
                }
                mercuryRhythmRow(m)
                // 「まだ一度も書込みが無い」と「書込み済みだが今は無音」を区別するための分母。
                Text("Writes: \(m.write_count)")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundColor(.secondary)
            }
        }
    }

    /// P635 / P636 — Mercury Unit の PCM 部。OPN 側と違いシャドウ経由ではなく、
    /// Core の非 static 変数(Mcry_Status / Mcry_ClockRate / Mcry_OutDataL/R)を
    /// Bridge が直読した値をそのまま表示する。
    /// ★Activity は Bridge 側の活動カウンタで、「PCM は動いているが無音」と
    ///   「そもそも PCM 活動が一度も無い」を区別するための分母。P636 以降は
    ///   サンプル値をスキャンラインごとにポーリングして変化を検出する方式なので、
    ///   CPU 書込み・DMAC 書込みのいずれの経路でも検出できる
    ///   (PCM8PP のような DMAC ch2 駆動の転送も捕捉できる)。
    private var mercuryPCMSection: some View {
        let p = emulatorViewModel.mercuryPCMStatus
        return VStack(alignment: .leading, spacing: 4) {
            Divider()
            Text("Mercury PCM").font(.headline)
            if !p.installed {
                Text("Mercury Unit 未装着")
                    .font(.caption)
                    .foregroundColor(.secondary)
            } else {
                HStack(spacing: 12) {
                    Text(p.stereo ? "Mode:STEREO" : "Mode:MONO  ")
                    Text("L:\(p.l_enabled ? "on " : "off")")
                        .foregroundColor(p.l_enabled ? .green : .secondary)
                    Text("R:\(p.r_enabled ? "on " : "off")")
                        .foregroundColor(p.r_enabled ? .green : .secondary)
                    Text("Clock:\(p.clock_rate)Hz")
                }
                .font(.system(.caption, design: .monospaced))
                mercuryPCMSampleRow(label: "L", value: p.last_out_l, enabled: p.l_enabled)
                mercuryPCMSampleRow(label: "R", value: p.last_out_r, enabled: p.r_enabled)
                // P636: 表示は「書込み回数」ではなく「サンプル値変化 = 活動」回数。
                // フィールド名 write_count は C 構造体の後方互換で据え置き。
                Text("Activity: \(p.write_count)")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundColor(.secondary)
            }
        }
    }

    /// 直近 PCM サンプル 1 チャンネル分: 数値 + 符号付きの簡易バー。
    /// 縦軸ならぬ横軸は Int16 フルスケール固定(ADPCM の波形表示と同じ方針 —
    /// 自動ゲインをかけると無音とフルスケールが同じ見た目になるため)。
    private func mercuryPCMSampleRow(label: String, value: Int16, enabled: Bool) -> some View {
        HStack(spacing: 12) {
            Text("PCM \(label):")
            Text(String(format: "%6d", Int(value)))
            mercuryPCMLevelBar(value: value, enabled: enabled)
                .frame(width: 120, height: 10)
        }
        .font(.system(.caption, design: .monospaced))
    }

    /// 中央 0 の符号付きバー。無音(0)でも中央線が残るので「更新が来ていない」
    /// ことと「振幅が 0」であることを見分けられる。
    private func mercuryPCMLevelBar(value: Int16, enabled: Bool) -> some View {
        Canvas { context, size in
            guard size.width > 0, size.height > 0 else { return }
            let midX = size.width / 2
            var zero = Path()
            zero.move(to: CGPoint(x: midX, y: 0))
            zero.addLine(to: CGPoint(x: midX, y: size.height))
            context.stroke(zero, with: .color(.gray.opacity(0.5)), lineWidth: 0.5)

            let ratio = min(1.0, abs(Double(value)) / 32768.0)
            let w = midX * ratio
            guard w > 0 else { return }
            let rect = CGRect(x: value < 0 ? midX - w : midX,
                              y: 0, width: w, height: size.height)
            context.fill(Path(rect), with: .color(enabled ? .green : .gray))
        }
    }

    /// P635 — Mercury 関連 2 セクションの束ね。
    /// ★body の親 VStack は ViewBuilder の子ビュー上限(10)に達しているため、
    ///   PCM セクションを子として直接足すことはできない。OPN と PCM をこの
    ///   computed property でまとめ、body 側は 1 子のまま差し替える。
    private var mercurySection: some View {
        VStack(alignment: .leading, spacing: 8) {
            mercuryOPNSection
            mercuryPCMSection
        }
    }

    /// FM 1チャンネル分: `FM CH1 ON  FNum:0472 Blk:4 ALG:3 FB:5 PAN:LR`
    /// F-Num/Block は生値表示(YMF288 は F-Number×Block の周波数表現で、音名への
    /// 変換式を一次情報源から確定できていないため — Docs 上の未確定事項)。
    private func mercuryFMRow(index: Int, ch c: MX68K_MercuryOPNChannel) -> some View {
        HStack(spacing: 12) {
            Text("FM CH\(index + 1)")
            Text(c.keyon != 0 ? "ON " : "---")
                .foregroundColor(c.keyon != 0 ? .green : .secondary)
            Text("FNum:" + String(format: "%04d", Int(c.fnum)))
            Text("Blk:\(c.block)")
            Text("ALG:\(c.alg)")
            Text("FB:\(c.fb)")
            Text("PAN:\(Self.opnPanName(c.pan))")
        }
        .font(.system(.caption, design: .monospaced))
    }

    /// SSG 1チャンネル分: `SSG-A  T:on N:off  Period:0345  Vol:12(ENV)`
    private func mercurySSGRow(index: Int, ch c: MX68K_MercurySSGChannel) -> some View {
        let name = ["A", "B", "C"][min(index, 2)]
        return HStack(spacing: 12) {
            Text("SSG-\(name)")
            Text("T:\(c.tone_on ? "on " : "off")")
                .foregroundColor(c.tone_on ? .green : .secondary)
            Text("N:\(c.noise_on ? "on " : "off")")
                .foregroundColor(c.noise_on ? .green : .secondary)
            Text("Period:" + String(format: "%04d", Int(c.period)))
            Text("Vol:" + String(format: "%2d", Int(c.volume)) + (c.env_on ? "(ENV)" : "     "))
        }
        .font(.system(.caption, design: .monospaced))
    }

    /// リズム + ノイズ周期: `RHY: BD SD -- HH -- --  TL:48  Noise:12`
    private func mercuryRhythmRow(_ m: MX68K_MercuryOPNStatus) -> some View {
        // bit0-5 = BD,SD,TOP,HH,TOM,RIM (opna.cpp:2095-2100 のコメントが出典)。
        let names = ["BD", "SD", "TOP", "HH", "TOM", "RIM"]
        let keys = names.indices.map { i -> String in
            let on = (m.rhythmkey & (UInt8(1) << UInt8(i))) != 0
            return on ? names[i] : String(repeating: "-", count: names[i].count)
        }.joined(separator: " ")
        return HStack(spacing: 12) {
            Text("RHY: \(keys)")
            Text("TL:\(m.rhythm_total_level)")
            Text("Noise:\(m.noise_period)")
        }
        .font(.system(.caption, design: .monospaced))
    }

    /// P549 — CoreAudio バッファのアンダーラン統計(セッション累積)。
    /// 「コールバック総数」が分母として常に表示されるため、「アンダーラン 0 件」と
    /// 「そもそも一度もコールバックされていない」を表示だけで区別できる。
    /// ★値は Bridge 側で 3 つの独立したアトミックから個別に読むため厳密には
    ///   同一瞬間のスナップショットではない — 割合は 100% でクランプする。
    private var audioBufferSection: some View {
        let a = emulatorViewModel.audioBufferStatus
        return VStack(alignment: .leading, spacing: 4) {
            Divider()
            Text("Audio Buffer (CoreAudio)").font(.headline)
            HStack(spacing: 12) {
                Text("Callbacks:")
                Text("\(a.callbacks_total)")
            }
            .font(.system(.caption, design: .monospaced))
            HStack(spacing: 12) {
                Text("Underruns:")
                Text("\(a.underrun_callbacks)")
                    .foregroundColor(a.underrun_callbacks > 0 ? .orange : .secondary)
                Text("(\(Self.underrunRatioText(a)))")
                    .foregroundColor(.secondary)
            }
            .font(.system(.caption, design: .monospaced))
            HStack(spacing: 12) {
                Text("Zero-filled:")
                Text("\(a.samples_zero_filled) samples")
            }
            .font(.system(.caption, design: .monospaced))
            Text("※ セッション累積(リセットされません)")
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }

    /// 分母が 0(まだ一度もコールバックされていない)の場合は割合を出さず「—」。
    /// 3 値が別々のアトミック由来で瞬間的に underrun > total になり得るため 100% でクランプする。
    private static func underrunRatioText(_ a: MX68K_AudioBufferStatus) -> String {
        guard a.callbacks_total > 0 else { return "—" }
        let pct = min(100.0, Double(a.underrun_callbacks) / Double(a.callbacks_total) * 100.0)
        return String(format: "%.2f%%", pct)
    }

    /// OPN の PAN 2bit は OPM と割り当てが逆: 0=mute, 1=R, 2=L, 3=L+R
    /// (opna.cpp:1149-1166 の Mix6 — pan==2 が dest[0]=L、pan==1 が dest[1]=R に入る)。
    private static func opnPanName(_ pan: UInt8) -> String {
        switch pan {
        case 1:  return "R "
        case 2:  return "L "
        case 3:  return "LR"
        default: return "--"
        }
    }

    private static let noteNames = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

    /// note == -1(無効nibble または KC 未書込み)は「--」。
    private static func noteName(note: Int8, octave: Int8) -> String {
        guard note >= 0, note < 12 else { return "--" }
        return noteNames[Int(note)] + String(octave)
    }

    /// PAN 2bit の意味(opm.cpp:481-502 のミキシングから逆算): 0=mute, 1=L, 2=R, 3=L+R。
    private static func panName(_ pan: UInt8) -> String {
        switch pan {
        case 1:  return "L "
        case 2:  return "R "
        case 3:  return "LR"
        default: return "--"
        }
    }

    /// ピーク値のバー表示。0-32768 を全幅にマップする。
    private func levelMeter(peak: Int32) -> some View {
        GeometryReader { geo in
            let ratio = min(1.0, max(0.0, Double(peak) / 32768.0))
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 2)
                    .fill(Color.gray.opacity(0.25))
                RoundedRectangle(cornerRadius: 2)
                    .fill(ratio > 0.9 ? Color.red : Color.green)
                    .frame(width: geo.size.width * ratio)
            }
        }
    }
}
