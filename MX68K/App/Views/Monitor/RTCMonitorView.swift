import SwiftUI

// P694 — RTC モニタ(RTC Viewer)。RICOH RP5C15($E8A000-)の制御/アラーム
// レジスタと、ゲストが日時レジスタを読んだときに返る値を 1 画面で見せる。
// CLKOUT の下位 3bit だけは P653 以降ステータスバーの TIMER-LED 点滅に使われて
// いたが、それ以外(日時本体・アラーム・ADJ・12/24h・うるう年・TEST/ALARM 出力
// 制御)はどこにも可視化されていなかった。
//
// ★設計方針(Fix Plan「自己反証可能性」節 + Code Review 指摘): このチップの
//   px68k 実装には「ゲストが書いた値が読み戻されない」箇所が 2 つある。パネルは
//   その 2 つを黙って 1 つの数字として見せない —— どちらも「何を見ているのか」を
//   その場に書き、ゼロ/食い違いに対する解釈が 1 つしか残らないようにする:
//
//   (1) 日時本体 —— Core の RTC_Read() はゲストが日時レジスタを読む度に
//       time(NULL)/localtime() で **ホストの現在時刻** を都度算出して返す。
//       チップ内部にエミュレータ独自の日時カウンタは無く、ゲストが日時レジスタへ
//       書いた値は読み出しに一切反映されない。ここを注記なしに出すと
//       「ゲスト側で時刻カウンタが正しく進んでいる」と誤読される。
//   (2) うるう年カウンタ —— ゲストの書込み先(RTC_Regs[1][11])と読み側
//       (BANK1 オフセット 0x17)が別物で、読み側はホストの年から都度計算する。
//       書込み値と実効値を **両方** 並べて表示する。
struct RTCMonitorView: View {
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    var body: some View {
        let rtc = emulatorViewModel.rtcMonitorStatus

        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 8) {
                    Text("RTC Status (RP5C15 $E8A000)").font(.headline)
                    Spacer()
                    Text("Bank:")
                        .foregroundColor(.secondary)
                    Text(verbatim: "\(rtc.bank)")
                }
                Divider()

                // ---- (1) Date / time = HOST clock proxy ----
                Text("Date / time").font(.subheadline)
                Text("⚠ These 7 values are a snapshot of the HOST system clock, not emulated guest state. The chip keeps no date/time counter of its own: every guest read of a date/time register is answered from the host clock, and anything the guest writes there is stored but never read back.")
                    .foregroundColor(.orange)
                    .fixedSize(horizontal: false, vertical: true)
                row("date:", "\(1980 + Int(rtc.year))-\(pad2(rtc.mon))-\(pad2(rtc.mday))")
                row("year (since 1980):", "\(rtc.year)")
                row("time:", hourText(rtc))
                row("min / sec:", "\(pad2(rtc.minute)) / \(pad2(rtc.sec))")
                row("day of week:", "\(rtc.wday) (\(weekdayName(rtc.wday)))")

                Divider()

                // ---- Clock output / ADJ / hour mode ----
                Text("Clock output & mode").font(.subheadline)
                row("CLKOUT select:", "\(rtc.clkout_select) (\(clkoutLabel(rtc.clkout_select)))")
                // ステータスバーの TIMER-LED と同じ生値をここに置き、ランプの
                // 見え方をパネル単独で照合できるようにする(点灯条件の解釈は
                // StatusBarView の P653 実装と同一)。
                row("TIMER lamp:", rtc.clkout_select == 7 ? "off (held low)" : "lit")
                row("ADJ:", "\(rtc.adj)")
                row("hour mode:", rtc.hour_mode_24 == 1 ? "24-hour" : "12-hour")

                Divider()

                // ---- Alarm registers ----
                Text("Alarm registers").font(.subheadline)
                row("alarm time:", "\(pad2(rtc.alarm_hour)):\(pad2(rtc.alarm_min))")
                row("alarm day:", "\(rtc.alarm_day)")
                row("alarm day of week:", "\(rtc.alarm_wday) (\(weekdayName(rtc.alarm_wday)))")

                Divider()

                // ---- (2) Leap-year counter: written value vs. value actually read back ----
                Text("Leap-year counter").font(.subheadline)
                row("written by guest:", "\(rtc.leap_year_ctr)")
                row("returned on read:", "\(rtc.leap_year_effective)")
                Text("⚠ This register is effectively write-only. The guest's written value is stored, but a read never returns it — the chip recomputes (host year − 1980) mod 4 every time. The two values above can therefore disagree, and that is not a defect.")
                    .foregroundColor(.orange)
                    .fixedSize(horizontal: false, vertical: true)

                Divider()

                // ---- Bank / test / alarm output control (raw) ----
                Text("Bank / test / alarm output control").font(.subheadline)
                row("$1B bank+enable:", hex8(rtc.reg_bank_ctrl))
                row("$1D test mode:", hex8(rtc.reg_test))
                row("$1F alarm output:", hex8(rtc.reg_alarm_out))
                // 生値だけでは読み取れない「今どのタイマ出力が止められているか」を
                // 併記する(派生値は必ず生値と並べる — referent-binding 規律)。
                // ビット位置は新規解釈ではなく Core/px68k/x68k/rtc.c の RTC_Timer()
                // が実際に分岐に使っている条件そのもの:
                //   :182 `if (!(RTC_Regs[0][15] & 0x08))` = 1Hz 出力 enable
                //   :194 `if (!(RTC_Regs[0][15] & 0x04))` = 16Hz 出力 enable
                //   :206 `if (!(RTC_Regs[0][13] & 0x04))` = AlarmEN
                row("1Hz output:", (rtc.reg_alarm_out & 0x08) != 0 ? "disabled" : "enabled")
                row("16Hz output:", (rtc.reg_alarm_out & 0x04) != 0 ? "disabled" : "enabled")
                row("alarm enable:", (rtc.reg_bank_ctrl & 0x04) != 0 ? "off" : "on")
            }
            .font(.system(.body, design: .monospaced))
            .padding()
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .frame(minWidth: 560, minHeight: 560, alignment: .topLeading)
        .onAppear { emulatorViewModel.engine.rtcMonitorVisible = true }
        .onDisappear { emulatorViewModel.engine.rtcMonitorVisible = false }
    }

    // MARK: - Rows

    @ViewBuilder
    private func row(_ label: String, _ value: String) -> some View {
        HStack(spacing: 8) {
            Text(verbatim: label)
                .frame(width: 200, alignment: .leading)
                .foregroundColor(.secondary)
            Text(verbatim: value)
            Spacer()
        }
    }

    // MARK: - Helpers

    private func hex8(_ v: UInt8) -> String { String(format: "0x%02X", v) }

    /// ★String(format:) に Int を渡して %d で受けるのは 64bit/32bit の幅が
    ///   食い違うため使わない(PaletteMonitorView.swift:46 と同じ既定)。
    private func pad2(_ v: UInt8) -> String { v < 10 ? "0\(v)" : "\(v)" }

    /// 時刻表示。★12 時間制では、チップ自身が「午後は +20」というエンコードで
    /// 時レジスタを返す(Core/px68k/x68k/rtc.c:49-52)。生値をそのまま出しつつ、
    /// その場で意味を書き添える —— 20〜31 という値を見て壊れていると誤読させない。
    private func hourText(_ rtc: MX68K_RTCStatus) -> String {
        if rtc.hour_mode_24 == 1 {
            return "\(pad2(rtc.hour)) (24-hour)"
        }
        let isPM = rtc.hour >= 20
        let display = isPM ? rtc.hour - 20 : rtc.hour
        return "\(pad2(display)) (12-hour, raw \(rtc.hour) = \(isPM ? "PM (+20)" : "AM"))"
    }

    /// CLKOUT セレクト(RTC_Regs[1][0] の下位 3bit)。★値の意味は新規解釈では
    /// なく、P653 で Inside X68000 p150 / テクニカルデータブック p191 を根拠に
    /// 確定済みの実機仕様をそのまま引用したもの —— StatusBarView の TIMER-LED
    /// 実装(P653、:93-108)と同一の分類を使う(2 箇所で解釈が食い違わないよう、
    /// 値の並びまで揃えてある)。
    private func clkoutLabel(_ v: UInt8) -> String {
        switch v {
        case 0, 1, 2, 3: return String(localized: "always lit (too fast to see)")
        case 4:          return String(localized: "16 Hz blink")
        case 5:          return String(localized: "1 Hz blink")
        case 6:          return String(localized: "1/60 Hz (30s on / 30s off)")
        default:         return String(localized: "held low (lamp off)")
        }
    }

    private func weekdayName(_ v: UInt8) -> String {
        switch v {
        case 0: return String(localized: "Sun")
        case 1: return String(localized: "Mon")
        case 2: return String(localized: "Tue")
        case 3: return String(localized: "Wed")
        case 4: return String(localized: "Thu")
        case 5: return String(localized: "Fri")
        case 6: return String(localized: "Sat")
        default: return "?"
        }
    }
}
