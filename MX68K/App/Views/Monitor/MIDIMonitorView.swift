import SwiftUI

// P693 — MIDI モニタ(MIDI Viewer)。MIDIボード(CZ-6BM1 相当 / YM3802、
// $EAFA00-$EAFA0F)の配線状態・送受信カウンタ・レジスタ生値・現在設定を
// 1 画面で見せる。P488/P490 で機能自体は完全実装済みだったが、ランプもモニタも
// 無く有効化後は完全に不可視だった(Docs/01 が「owed monitor」として明記)。
//
// ★設計方針(Fix Plan「自己反証可能性」節): カウンタは必ず「分母」と並べる。
//   wired(配線確定)→ device count(送り先の数)→ tx_messages(送信を試みた
//   回数)→ tx_bytes(実際に CoreMIDI へ渡せたバイト数)を必ず同一画面に
//   置くことで、
//     - wired=false          → ハードリセット待ち
//     - device count=0       → この Mac に MIDI ポートが無い
//     - tx_messages=0        → ゲストがそもそも送っていない
//     - messages>0, bytes=0  → 送出層で落ちている(デバイス未オープン等)
//     - bytes>0 なのに無音   → 外部機器側
//   と、ゼロ表示に対して解釈が 1 つしか残らないようにする。
//
// ★デバイス *名* はモニタの毎フレーム tick から取りに行かない。名前一覧は
//   Core 側が MIDI_Init()(init / ハードリセット)でしか埋め直さないため、
//   設定画面と同じく .onAppear で refreshMidiDeviceList() を 1 回だけ呼び、
//   既存の published 配列から読む。更新されない値をライブ値であるかのように
//   毎フレーム描き直すのは誤解を招くだけで情報量が増えない。
//
// ★YM3802 レジスタ節に MIDI_IntFlag / MIDI_IntVect は含まれない。この 2 つは
//   CoreMIDI コールバックスレッドとエミュレーションスレッドから無同期に
//   read-modify-write されている既知の競合(Docs/09 の D-73、本サイクルの
//   スコープ外)で、モニタから読むと 4 本目の読み手を足すことになるため。
struct MIDIMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    @EnvironmentObject var configManager: ConfigManager

    var body: some View {
        let ext = configManager.config.extensions
        let midi = emulatorViewModel.midiStatus
        let txLast = MIDILastMessage(packed: midi.txLast)
        let rxLast = MIDILastMessage(packed: midi.rxLast)

        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 8) {
                    Text("MIDI Status").font(.headline)
                    Spacer()
                    Text("Wired:")
                    Text(midi.wired ? "●" : "○")
                        .foregroundColor(midi.wired ? .green : .gray)
                }
                Divider()

                // ---- Board / devices ----
                Text("Board (CZ-6BM1 / YM3802 $EAFA00)").font(.subheadline)
                if !ext.midiEnabled {
                    Text("(not installed — enable it in Settings → Audio)")
                        .foregroundColor(.secondary)
                } else if !midi.wired {
                    Text("(configured but not wired — press ⌘R for a hard reset)")
                        .foregroundColor(.orange)
                }
                row("out devices:", "\(midi.outputDeviceCount)")
                row("out selected:", deviceName(emulatorViewModel.midiOutputDeviceNames,
                                                index: ext.midiOutDeviceIndex))
                row("in devices:", "\(midi.inputDeviceCount)")
                row("in selected:", deviceName(emulatorViewModel.midiInputDeviceNames,
                                               index: ext.midiInDeviceIndex))

                Divider()

                // ---- TX (guest -> real MIDI device) ----
                Text("TX (guest → device)").font(.subheadline)
                row("messages:", "\(midi.txMessages)")
                row("bytes:", "\(midi.txBytes)")
                lastMessageRows(txLast, packed: midi.txLast)

                Divider()

                // ---- RX (real MIDI device -> guest) ----
                Text("RX (device → guest)").font(.subheadline)
                // ★rx_messages の単位は CoreMIDI パケット。1 パケットに複数
                //   メッセージが載りうるので、名前の通りの「メッセージ数」では
                //   ないことをその場に書いておく(あとで別画面を見に行かせない)。
                Text("(messages = CoreMIDI packets; one packet may carry several MIDI messages)")
                    .foregroundColor(.secondary)
                row("messages:", "\(midi.rxMessages)")
                row("bytes:", "\(midi.rxBytes)")
                lastMessageRows(rxLast, packed: midi.rxLast)
                if (midi.regs.r35 & 0x01) == 0 {
                    // R35 bit0 = Rx-FIFO 受信許可。0 のときは受信バイトが
                    // そもそも計上されないので、rx が 0 の理由をここで明示する。
                    Text("⚠ Rx FIFO disabled by the guest (R35 bit0 = 0) — incoming bytes are not counted.")
                        .foregroundColor(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }

                Divider()

                // ---- YM3802 registers ----
                Text("YM3802 registers").font(.subheadline)
                row("RegHigh (bank):", hex8(midi.regs.reg_high))
                row("Vector:", hex8(midi.regs.vector))
                row("IntEnable:", hex8(midi.regs.int_enable))
                row("R05:", hex8(midi.regs.r05))
                row("R35:", hex8(midi.regs.r35))
                row("R55:", hex8(midi.regs.r55))
                row("Tx FIFO used:", "\(midi.regs.tx_fifo_used)")

                Divider()

                // ---- Current settings (Swift config, no new bridge API) ----
                Text("Settings").font(.subheadline)
                row("send reset on init:", ext.midiResetOnInit ? "on" : "off")
                row("reset command:", resetTypeLabel(ext.midiResetType))
                row("output delay:", "\(ext.midiDelayMs) ms")
            }
            .font(.system(.body, design: .monospaced))
            .padding()
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .frame(minWidth: 520, minHeight: 520, alignment: .topLeading)
        .onAppear {
            // 一覧は MIDI_Init() でしか埋まらないので、パネルを開いた時点で 1 回だけ取り込む。
            emulatorViewModel.refreshMidiDeviceList()
            emulatorViewModel.engine.midiVisible = true
        }
        .onDisappear { emulatorViewModel.engine.midiVisible = false }
    }

    // MARK: - Rows

    @ViewBuilder
    private func row(_ label: String, _ value: String) -> some View {
        HStack(spacing: 8) {
            Text(verbatim: label)
                .frame(width: 170, alignment: .leading)
                .foregroundColor(.secondary)
            Text(verbatim: value)
            Spacer()
        }
    }

    /// 直近メッセージ。★展開した派生値だけでなく、元になったパック生値も
    /// 併記する(どちらか一方だけを見せない — referent-binding 規律)。
    @ViewBuilder
    private func lastMessageRows(_ msg: MIDILastMessage, packed: UInt32) -> some View {
        row("last message:", msg.hexDescription)
        if msg.isEmpty {
            row("last decoded:", "--")
        } else {
            let channelText = msg.channel.map { " ch\($0)" } ?? ""
            row("last decoded:", "\(msg.statusName)\(channelText)")
            // len は min(実長, 0xff) にクランプされた表示用の目安値であって
            // 実長そのものではない(SysEx は最大 1024 バイト)。
            row("last length:", msg.length == 0xff ? "255+ (clamped)" : "\(msg.length)")
        }
        row("last raw:", String(format: "0x%08X", packed))
    }

    // MARK: - Helpers

    private func hex8(_ v: UInt8) -> String { String(format: "0x%02X", v) }

    /// 選択中デバイス名。一覧が空、または設定インデックスが一覧の範囲外の
    /// ときは、その事実をそのまま出す(存在しない名前を作らない)。
    private func deviceName(_ names: [String], index: Int) -> String {
        guard !names.isEmpty else { return String(localized: "(none)") }
        guard index >= 0 && index < names.count else {
            return String(localized: "(index \(index) out of range)")
        }
        return names[index]
    }

    private func resetTypeLabel(_ type: Int) -> String {
        switch type {
        case 0:  return "LA (MT-32)"
        case 1:  return "GM"
        case 2:  return "GS"
        case 3:  return "XG"
        default: return "?(\(type))"
        }
    }
}
