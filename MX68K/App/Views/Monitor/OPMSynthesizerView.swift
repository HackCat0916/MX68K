import SwiftUI

/// P631 — OPM(YM2151)シンセサイザーパネル(独立ウィンドウ)。
///
/// P630 ではサウンドモニタ内に 8ch 合成の鍵盤を 1 段だけ置いていたが、ユーザーの
/// 設計変更要望(2026-08-18)により **独立ウィンドウ + 1ch につき 1 段の鍵盤 8 段 +
/// 各段の詳細情報** という XM6 系「サウンドシンセサイザー」パネル風の構成へ移設した。
/// P630 の鍵盤描画ロジック(白鍵/黒鍵の形状描画)はここへ移設して再利用しており、
/// SoundMonitorView 側からは撤去済み(OPM セクションはテキスト一覧のみに戻した)。
///
/// 更新頻度: サウンドモニタとは **別の** 可視性フラグ
/// (EmulatorEngine.opmSynthVisible)で駆動される毎フレーム更新。サウンドモニタを
/// 閉じてもこちらの更新は止まらず、逆もまた同じ。
///
/// ★参照実装バインディング上の注記: 参考にした「サウンドシンセサイザー」パネルは
/// XM6 "本家" には存在せず(TypeG 等の後発版固有でソース非公開)、一次情報源は
/// ユーザー提供のスクリーンショットのみ。表示式の一部は推定を含む(下記 V: の注記)。
struct OPMSynthesizerView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    /// C の固定長配列 `MX68K_OPMDetailChannel ch[8]` は Swift へタプルとして
    /// 取り込まれるため、行の反復用に配列へ展開する(SoundMonitorView と同じ手法)。
    private var channels: [MX68K_OPMDetailChannel] {
        withUnsafeBytes(of: emulatorViewModel.opmDetailStatus.ch) { raw in
            Array(raw.bindMemory(to: MX68K_OPMDetailChannel.self))
        }
    }

    // MARK: - 鍵盤の寸法・配色(P630 から移設。8 段積むため P630 より小さめ)

    /// 鍵盤の表示範囲。`octave` が取りうる 0-7 の全域(EmulatorBridge.h の octave
    /// フィールドのコメント)を 1 オクターブも切り捨てずに表示するため 8 オクターブ。
    private static let keyboardOctaves = 0...7
    /// 白鍵に対応する音番号(C D E F G A B)。
    private static let whiteNotes = [0, 2, 4, 5, 7, 9, 11]
    private static let whiteKeyWidth: CGFloat = 10
    private static let whiteKeyHeight: CGFloat = 34
    private static let blackKeyWidth: CGFloat = 7
    private static let blackKeyHeight: CGFloat = 21
    /// 鍵盤行の行高。
    private static let rowHeight: CGFloat = 38
    /// 情報行の幅(鍵盤の上に配置。`CH8 KCF:$1214 V:122 LR` が収まる幅)。
    private static let infoWidth: CGFloat = 210

    /// CH1〜CH8 の識別色。並びは行の「CH1」〜「CH8」と同じ(index 0 = CH1)。
    private static let channelColors: [Color] = [
        .red, .orange, .yellow, .green, .cyan, .blue, .purple, .pink,
    ]

    /// 黒鍵の位置。`whiteIndex` はその黒鍵が右上に載る白鍵のインデックス(0始まり)。
    private struct BlackKeySlot: Identifiable {
        let whiteIndex: Int
        let note: Int
        var id: Int { note }
    }

    private static let blackKeySlots: [BlackKeySlot] = [
        BlackKeySlot(whiteIndex: 0, note: 1),   // C#
        BlackKeySlot(whiteIndex: 1, note: 3),   // D#
        BlackKeySlot(whiteIndex: 3, note: 6),   // F#
        BlackKeySlot(whiteIndex: 4, note: 8),   // G#
        BlackKeySlot(whiteIndex: 5, note: 10),  // A#
    ]

    private static let octaveWidth: CGFloat = whiteKeyWidth * CGFloat(whiteNotes.count)
    private static let keyboardWidth: CGFloat = octaveWidth * CGFloat(keyboardOctaves.count)

    // MARK: - body

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("OPM Synthesizer (YM2151 FM 8ch)").font(.headline)
            Divider()
            // 1ch = 「ステータス → 鍵盤 → オクターブ目盛り」の縦積みブロック。
            // 情報欄を対応する鍵盤の真上に置くことで、横幅が広がっても両者が離れない。
            ScrollView(.vertical, showsIndicators: true) {
                VStack(alignment: .leading, spacing: 8) {
                    ForEach(Array(channels.enumerated()), id: \.offset) { idx, c in
                        VStack(alignment: .leading, spacing: 0) {
                            infoRow(index: idx, ch: c)
                            keyboardRow(index: idx, ch: c)
                            octaveRuler
                        }
                    }
                }
            }
            Divider()
            footnotes
        }
        .font(.system(.body, design: .monospaced))
        .padding()
        .frame(minWidth: 600, minHeight: 300, alignment: .topLeading)
        // ★可視性フラグはサウンドモニタ(soundMonitorVisible)とは独立。
        //   こちらを開閉してもサウンドモニタの更新には一切影響しない。
        .onAppear { emulatorViewModel.engine.opmSynthVisible = true }
        .onDisappear { emulatorViewModel.engine.opmSynthVisible = false }
    }

    // MARK: - 1ch 分の情報欄

    /// `CH1 KCF:$1214 V:122 LR` — ユーザー提供の XM6 系スクリーンショットの行構成に倣う。
    /// まだ一度も KC 書込みが無いチャンネル(written == false)は、生の 0 埋め値を
    /// 意味のあるレジスタ値であるかのように見せないため全項目を「-」で表示する
    /// (OPM は全レジスタ 0 でリセットされるため 0 と未設定は区別できない)。
    private func infoRow(index: Int, ch c: MX68K_OPMDetailChannel) -> some View {
        HStack(spacing: 8) {
            Text("CH\(index + 1)")
                .foregroundColor(c.keyon != 0 ? Self.channelColors[index % Self.channelColors.count]
                                              : .secondary)
            Text("KCF:\(Self.kcfText(c))")
            Text("V:\(Self.volumeText(c))")
            Text(c.written ? Self.panName(c.pan) : "--")
        }
        .font(.system(.caption, design: .monospaced))
        .frame(width: Self.infoWidth, alignment: .leading)
    }

    /// KCF は KC 生バイトと KF 生バイトを単純連結した 16 進 4 桁
    /// (ユーザー提供スクリーンショットの 3 例 $1214/$32C8/$2AD8 を、KF 生バイトの
    ///  構造的制約[下位 2bit = 0、opm.cpp:243 が data>>2 を保持]と照合して導いた式)。
    /// ★この連結式自体が推定であり、hands-on 比較で最終確認する対象。
    private static func kcfText(_ c: MX68K_OPMDetailChannel) -> String {
        guard c.written else { return "$----" }
        let v = (Int(c.kc_raw) << 8) | Int(c.kf_raw)
        return String(format: "$%04X", v)
    }

    /// ★V(音量)は **推定値であり確定仕様ではない**。
    /// Bridge 側 `mx68k_get_opm_detail_status()` が `127 - min(キャリアのTL)` で算出して
    /// いるが、参照した XM6 系パネルはソース非公開のため実際の算出式を検証できていない
    /// (詳細は EmulatorBridge.c の volume_est のコメント)。実チップの音量は EG 位相・
    /// LFO・KS にも依存するので、ここに出るのは「レジスタ設定上の最大音量の目安」。
    private static func volumeText(_ c: MX68K_OPMDetailChannel) -> String {
        guard c.written else { return "---" }
        return String(format: "%03d", c.volume_est)
    }

    /// PAN 2bit の意味(opm.cpp:481-502 のミキシングから逆算): 0=mute, 1=L, 2=R, 3=L+R。
    /// SoundMonitorView.panName と同じ表(あちらは private なため同じ導出で再定義)。
    private static func panName(_ pan: UInt8) -> String {
        switch pan {
        case 1:  return "L "
        case 2:  return "R "
        case 3:  return "LR"
        default: return "--"
        }
    }

    // MARK: - 1ch 分の鍵盤(P630 の描画ロジックを移設)

    /// そのチャンネルが今鳴らしている鍵番号(octave * 12 + note)。鳴っていなければ nil。
    /// 判定条件は P630 / 既存 opmRow と同じ(keyon != 0 かつ note が有効範囲)。
    private static func activeKey(_ c: MX68K_OPMDetailChannel) -> Int? {
        guard c.keyon != 0, c.note >= 0, c.note < 12 else { return nil }
        guard keyboardOctaves.contains(Int(c.octave)) else { return nil }
        return Int(c.octave) * 12 + Int(c.note)
    }

    /// 96 鍵 1 段。発音中の鍵だけをそのチャンネルの識別色で塗る。
    private func keyboardRow(index: Int, ch c: MX68K_OPMDetailChannel) -> some View {
        let active = Self.activeKey(c)
        let color = Self.channelColors[index % Self.channelColors.count]
        return HStack(spacing: 0) {
            ForEach(Array(Self.keyboardOctaves), id: \.self) { oct in
                octaveView(octave: oct, activeKey: active, color: color)
            }
        }
        .frame(width: Self.keyboardWidth, height: Self.rowHeight, alignment: .topLeading)
    }

    /// 1 オクターブ分(白鍵 7 + 黒鍵 5)。黒鍵は白鍵の上に重ねて描く。
    private func octaveView(octave: Int, activeKey: Int?, color: Color) -> some View {
        let w = Self.whiteKeyWidth
        return ZStack(alignment: .topLeading) {
            HStack(spacing: 0) {
                ForEach(Self.whiteNotes, id: \.self) { note in
                    keyView(octave: octave, note: note, isBlack: false,
                            activeKey: activeKey, color: color)
                        .frame(width: w, height: Self.whiteKeyHeight)
                }
            }
            ForEach(Self.blackKeySlots) { slot in
                keyView(octave: octave, note: slot.note, isBlack: true,
                        activeKey: activeKey, color: color)
                    .frame(width: Self.blackKeyWidth, height: Self.blackKeyHeight)
                    .offset(x: CGFloat(slot.whiteIndex + 1) * w - Self.blackKeyWidth / 2)
            }
        }
        .frame(width: Self.octaveWidth, height: Self.whiteKeyHeight, alignment: .topLeading)
    }

    /// 1 鍵。この段のチャンネルが発音中の鍵ならチャンネル色で塗る。
    private func keyView(octave: Int, note: Int, isBlack: Bool,
                         activeKey: Int?, color: Color) -> some View {
        let isActive = (activeKey == octave * 12 + note)
        return Rectangle()
            .fill(isActive ? color : (isBlack ? Color.black : Color.white))
            .overlay(Rectangle().stroke(Color.gray.opacity(0.6), lineWidth: 0.5))
    }

    /// 各チャンネルの鍵盤の直下に置くオクターブ目盛り。
    private var octaveRuler: some View {
        HStack(spacing: 0) {
            ForEach(Array(Self.keyboardOctaves), id: \.self) { oct in
                Text("C\(oct)")
                    .font(.system(size: 8, design: .monospaced))
                    .foregroundColor(.secondary)
                    .frame(width: Self.octaveWidth, alignment: .leading)
            }
        }
        .frame(width: Self.keyboardWidth, height: 12, alignment: .leading)
    }

    // MARK: - 注記

    private var footnotes: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("※ V(音量)は推定値です — キャリアの TL から算出した「設定上の最大音量の目安」であり、実チップのエンベロープ位相は反映されません")
            Text("※ CSM(reg $14)使用時はキーオン表示が実チップと一致しない場合があります")
            Text("※ KCF は KC(reg $28+ch)と KF(reg $30+ch)の生バイトを連結した値です")
        }
        .font(.caption)
        .foregroundColor(.secondary)
    }
}
