import SwiftUI
import Foundation   // P204: sin() for lamp blink

struct StatusBarView: View {
    @EnvironmentObject var viewModel: EmulatorViewModel

    var body: some View {
        // P204/S2 — 点滅が必要なとき(電源OFF 遷移中 or HDD アクセス中)は
        // ライブ描画が要る。P568 でスケジュールを `.animation`(~60Hz)から
        // `.periodic` 20Hz へ間引いた。
        // P569 — if/else による構造分岐(TimelineView ブランチ ↔ 素の lampsRow
        // ブランチ)を撤廃し、常に同一の TimelineView でラップして View identity を
        // 不変にする。切り替えるのは periodic 間隔だけ(アクティブ 20Hz /
        // アイドル 1Hz)。点滅の見た目(3.5Hz / 1Hz→12Hz)は不変。
        // (D-49 残存症状 = HDD ランプの緑↔赤遷移の瞬間に Monitor サブメニューが
        // 自動的に閉じる、への対処)。
        // P653 — TIMER-LED が高速点滅(CLKOUT=4:16Hz / 5:1Hz)のときも
        // ライブ描画が要る。CLKOUT=6(1/60Hz、30秒周期トグル)はアイドル 1Hz で十分。
        let clkout = viewModel.status.rtc_clkout_select
        let timerBlinking = (clkout == 4 || clkout == 5)   // 16Hz/1Hz は高速リフレッシュが要る
        let isActive = viewModel.powerState == .poweringOff || viewModel.status.hdd_busy || timerBlinking
        TimelineView(.periodic(from: .now, by: isActive ? 0.05 : 1.0)) { ctx in
            lampsRow(date: ctx.date)
        }
    }

    /// P204 — ステータスバー本体。`date` を時間基準に点滅ランプが opacity を計算する。
    /// 静的分岐では固定 Date() を、点滅分岐では TimelineView の date を渡す。
    private func lampsRow(date: Date) -> some View {
        // P204-fix: 電源OFF(.off)ではディスクランプは全消灯(実機同様)。
        // .poweringOff 中はまだ電源ON扱い = フェード完了で .off になるまで点灯。
        let lampsLive = viewModel.powerState != .off
        return HStack(spacing: 16) {
            // P691: 常設の主要7項目(CPU/MEM/Speed/FDD0/FDD1/HDD/TIMER)は優先度1で死守し、
            // 幅不足時は機種名(-1)/一時メッセージ(-1)から先に切り詰められるようにする。
            Text("CPU: \(Int(viewModel.status.clock_mhz))MHz")
                .font(.caption)
                .layoutPriority(1)

            Text("MEM: \(Int(viewModel.status.memory_mb))MB")
                .font(.caption)
                .layoutPriority(1)

            // P658: 実行速度%。電源OFF時は最後の測定値が凍結表示されるのを避けるため、
            // 既存のlampsLive(disk lampsと同じ電源状態判定)で "--" にフォールバックする。
            // P659: 値部分を固定幅にし、桁数変化で後続のFDD0以降がガタつくのを防ぐ。
            HStack(spacing: 2) {
                Text("Speed:")
                    .font(.caption)
                    .lineLimit(1)
                Text(lampsLive ? "\(Int(viewModel.speedPercent.rounded()))%" : "--")
                    .font(.caption)
                    .monospacedDigit()
                    .lineLimit(1)
                    .frame(width: 35, alignment: .trailing)
            }
            .layoutPriority(1)

            HStack(spacing: 4) {
                Text("FDD0:")
                    .font(.caption)
                let fdd0Show = lampsLive && viewModel.status.fdd0_inserted
                Text(fdd0Show ? "●" : "○")
                    .font(.caption)
                    .foregroundColor(!fdd0Show ? .gray : (viewModel.status.fdd0_active ? .red : .green))
            }
            .layoutPriority(1)

            HStack(spacing: 4) {
                Text("FDD1:")
                    .font(.caption)
                let fdd1Show = lampsLive && viewModel.status.fdd1_inserted
                Text(fdd1Show ? "●" : "○")
                    .font(.caption)
                    .foregroundColor(!fdd1Show ? .gray : (viewModel.status.fdd1_active ? .red : .green))
            }
            .layoutPriority(1)

            // P204-fix — HDD ランプ: 未マウント/電源OFF=グレー ○ / マウント&アイドル=
            // 緑 ●(点灯)/ アクセス中(hdd_busy)=赤 ● を ~3.5Hz で点滅。
            HStack(spacing: 4) {
                Text("HDD:")
                    .font(.caption)
                let hddShow = lampsLive && (viewModel.status.hdd0_inserted || viewModel.status.hdd1_inserted)
                let hddBusy = hddShow && viewModel.status.hdd_busy
                Text(hddShow ? "●" : "○")
                    .font(.caption)
                    .foregroundColor(!hddShow ? .gray : (hddBusy ? .red : .green))
                    .opacity(hddBusy ? blinkOpacity(date, 3.5) : 1.0)
            }
            .layoutPriority(1)

            // P653 — TIMER-LED: RTC CLKOUTセレクトレジスタ(BANK1 $E8A001)連動。
            // Inside X68000 p150 / テクニカルデータブック p191 で確認済みの実機仕様。
            // 000-011=常時点灯(高速すぎ目視不可) / 100=16Hz点滅 / 101=1Hz点滅 /
            // 110=1/60Hz(30秒点灯30秒消灯) / 111=常時消灯。
            HStack(spacing: 4) {
                Text("TIMER:")
                    .font(.caption)
                let clkout = viewModel.status.rtc_clkout_select
                let (lit, blinkFreq): (Bool, Double?) = {
                    switch clkout {
                    case 0, 1, 2, 3: return (true, nil)
                    case 4:          return (true, 16.0)
                    case 5:          return (true, 1.0)
                    case 6:          return ((Int(date.timeIntervalSinceReferenceDate) % 60) < 30, nil)
                    default:         return (false, nil)   // 7 = L固定(消灯)
                    }
                }()
                let show = lampsLive && lit
                Text(show ? "●" : "○")
                    .font(.caption)
                    .foregroundColor(show ? .red : .gray)
                    .opacity((show && blinkFreq != nil) ? blinkOpacity(date, blinkFreq!) : 1.0)
            }
            .layoutPriority(1)

            // P691: 機種名は常設操作情報ではないため一時メッセージと同じ優先度(-1)へ格下げ。
            Text("Machine: \(viewModel.status.machineDisplayName)")
                .font(.caption)
                .lineLimit(1)
                .layoutPriority(-1)

            Spacer()

            // P189 — スクショ完了等の一時メッセージ。数秒後に自動で消える。
            // P661: 常設ステータス項目(CPU/MEM/Speed/ランプ等、優先度0)より低い優先度
            // (-1)を明示し、横幅不足時は一時メッセージ側が先に切り詰められるようにする
            // (P660で「速度:」が省略記号化した根本原因——全子要素が優先度0で対等に
            // 縮小負担を分担していたため)。
            if let msg = viewModel.transientMessage {
                Text(msg)
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .lineLimit(1)
                    .layoutPriority(-1)
                    .transition(.opacity)
            }

            // P204 — 電源ランプ: ON=緑 / OFF=赤 / 遷移中=緑を遅→速点滅。
            powerLamp(date: date)

            // P204 — 電源ボタン(XM6 下部 POWER 相当)。遷移中(4.0s フェード)は無効。
            Button {
                viewModel.togglePower()
            } label: {
                Image(systemName: "power")
            }
            .buttonStyle(.plain)
            .help(String(localized: "Power"))
            .disabled(viewModel.powerState == .poweringOff)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 2)
        .background(Color(nsColor: .controlBackgroundColor))
    }

    /// P204 — 電源ランプ。遷移中は経過時間で点滅周波数を上げる(遅 1Hz → 速 2Hz)。
    /// 経過は TimelineView の `date` を powerOffStart との差で測る(Date() 直読みだと
    /// アニメしない)。t=2.0s で 1Hz/2Hz どちらの sin も 0 = 位相連続。
    @ViewBuilder
    private func powerLamp(date: Date) -> some View {
        switch viewModel.powerState {
        case .on:
            Text("●").font(.caption).foregroundColor(.green)
        case .off:
            Text("●").font(.caption).foregroundColor(.red)
        case .poweringOff:
            let elapsed = date.timeIntervalSince(viewModel.powerOffStart ?? date)
            // P204-fix: slow 1Hz (first 2s) → fast 12Hz. Accumulated phase keeps
            // it continuous at the 2s boundary (no opacity jump).
            let phase = elapsed < 2.0
                ? (2 * .pi * 1.0 * elapsed)
                : (2 * .pi * 1.0 * 2.0 + 2 * .pi * 12.0 * (elapsed - 2.0))
            Text("●")
                .font(.caption)
                .foregroundColor(.green)
                .opacity(0.5 + 0.5 * sin(phase))
        }
    }

    /// P204 — Date から点滅 opacity(0..1)。`0.5 + 0.5*sin(2π·freq·t)`。
    private func blinkOpacity(_ date: Date, _ freqHz: Double) -> Double {
        let t = date.timeIntervalSinceReferenceDate
        return 0.5 + 0.5 * sin(t * 2 * .pi * freqHz)
    }
}
