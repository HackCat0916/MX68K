//
//  TouchMouseView.swift
//  MX68K-iOS
//
//  P714: 画面全体を **相対トラックパッド**として扱うタッチマウス。
//
//  ★規範(normative reference)は **MX68K 自身の macOS 実装**
//    `MX68K/App/Services/InputManager.swift:669-698`
//    (`relativeMove` の感度スケーリング + 端数持ち越し、`handleMouseDown/Up`)である。
//    絶対座標モード(`absoluteMove`, `:629-666`)は **持ち込まない** ——
//    あちらはホストカーソルの *継続的な位置* を前提に差分を計算しており、
//    タッチの「触れて→離す」という離散的な性質とはモデルが噛み合わない
//    (P714 Code Investigation §B-3、計画は推奨 B-1 = 相対方式を採用)。
//
//  ★UIKit のジェスチャ認識器を使う理由(P714 計画 §変更内容 2 からの実装上の是正):
//    計画は 2 本指タップを `SpatialTapGesture` の `count` で取ると書いているが、
//    SwiftUI の `count` は **タップ回数**であって **指の本数**ではない
//    (2 本指タップを表現できるジェスチャは SwiftUI に存在しない)。
//    `UITapGestureRecognizer.numberOfTouchesRequired` が唯一の正しい表現手段
//    なので、計画の *意図*(1 本指ドラッグ=移動 / タップ=左 / 2 本指タップ=右)を
//    そのまま満たす形で UIKit 側へ降ろしている(§実装差分 D-2)。
//    副次的な利点として、`UIPanGestureRecognizer` の
//    `translation` / `setTranslation(.zero)` は「前回からの差分」を認識器自身が
//    保持してくれるため、SwiftUI `DragGesture` の累積 translation から
//    フレーム間差分を自前で引き算するより素直で、取りこぼしが無い。
//
//  ★キー入力への影響は無い —— `UIPress` は**ヒットテストではなくレスポンダチェーン**を
//    辿って配送される(`EmulatorMetalView_iOS.swift:26-29` に既述の事実)。
//    このオーバーレイは first responder にならないただの `UIView` なので、
//    最前面に重なっても物理キーボード入力は `EmulatorMTKView_iOS` へ届き続ける。
//

import SwiftUI
import UIKit

/// Metal ビュー全域を覆う透明なジェスチャレイヤー。**表示物を一切持たない**
/// (見た目の変化はトグルボタンの状態だけで表す — 計画 §変更内容 2)。
struct TouchMouseView: UIViewRepresentable {

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> UIView {
        let view = TouchMouseSurface()
        view.backgroundColor = .clear
        view.isMultipleTouchEnabled = true

        let coordinator = context.coordinator

        // 1 本指ドラッグ = 相対移動。
        let pan = UIPanGestureRecognizer(target: coordinator,
                                         action: #selector(Coordinator.handlePan(_:)))
        pan.minimumNumberOfTouches = 1
        pan.maximumNumberOfTouches = 1
        view.addGestureRecognizer(pan)

        // 1 本指タップ = 左クリック。
        let leftTap = UITapGestureRecognizer(target: coordinator,
                                             action: #selector(Coordinator.handleLeftTap(_:)))
        leftTap.numberOfTouchesRequired = 1
        view.addGestureRecognizer(leftTap)

        // 2 本指タップ = 右クリック。
        let rightTap = UITapGestureRecognizer(target: coordinator,
                                              action: #selector(Coordinator.handleRightTap(_:)))
        rightTap.numberOfTouchesRequired = 2
        view.addGestureRecognizer(rightTap)

        // 2 本指タップが成立し得る間は 1 本指タップを待たせる —— そうしないと
        // 2 本指を「わずかにずれた 2 回の 1 本指タップ」として拾い、右クリックの
        // つもりが左クリックになる取りこぼしが出る。
        leftTap.require(toFail: rightTap)

        mx68k_log("[Swift][iOS][P714-MOUSE] surface installed")
        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        // 状態を持たないレイヤーなので更新するものは無い。
    }

    /// レイヤーが外れる(= マウスモード OFF / アプリ終了)ときに、押しっぱなしの
    /// ボタンが残らないよう保険を掛ける。
    static func dismantleUIView(_ uiView: UIView, coordinator: Coordinator) {
        coordinator.releaseButtons(reason: "dismantle")
    }

    // MARK: - ジェスチャ処理

    final class Coordinator: NSObject {

        /// P194 の端数持ち越し(`InputManager.swift:362-363, 671-680`)と同じ仕組み。
        /// 感度 1.0 未満でも入力が丸めで消えないようにする。
        private var accX = 0.0
        private var accY = 0.0

        /// 初期感度は macOS 側の既定値(`InputManager.mouseSensitivity = 1.0`,
        /// `InputManager.swift:209`)をそのまま流用する —— タッチ用に新しい数値を
        /// 発明しない(計画 §残留リスク: 感度調整が要ると判明した場合は次サイクル)。
        private let sensitivity = 1.0

        /// 押下中のボタン(押しっぱなし残留を検知するため保持する)。
        private var heldButtons = Set<Int32>()

        /// クリックのパルス幅(秒)。押下と解放が同一フレーム内で潰れると
        /// ゲスト側(SCC 経由のマウスパケットをポーリングする)が取りこぼし得るため、
        /// 実際の人間のクリックに近い長さを与える。
        private static let clickPulseSeconds = 0.05

        @objc func handlePan(_ recognizer: UIPanGestureRecognizer) {
            guard let view = recognizer.view else { return }
            switch recognizer.state {
            case .began:
                // 認識成立までの移動量は捨てる(始点をここに置き直す)。
                recognizer.setTranslation(.zero, in: view)
            case .changed:
                let t = recognizer.translation(in: view)
                // 読んだぶんは毎回ゼロへ戻す = ここで得られる値は常に「前回からの差分」。
                recognizer.setTranslation(.zero, in: view)
                sendRelative(dx: Double(t.x), dy: Double(t.y))
            case .ended, .cancelled, .failed:
                accX = 0
                accY = 0
            default:
                break
            }
        }

        /// `InputManager.relativeMove`(`:669-684`)の逐語移植。
        /// 感度を掛けて整数部だけ送り、端数は次回へ持ち越す。送出値のクランプ
        /// (±10000)も同じ(`InputManager.swift:682-683`)。
        private func sendRelative(dx: Double, dy: Double) {
            accX += dx * sensitivity
            accY += dy * sensitivity
            let ix = Int(accX)
            let iy = Int(accY)
            guard ix != 0 || iy != 0 else { return }
            accX -= Double(ix)
            accY -= Double(iy)
            mx68k_mouse_move(Int32(min(max(ix, -10000), 10000)),
                             Int32(min(max(iy, -10000), 10000)))
        }

        @objc func handleLeftTap(_ recognizer: UITapGestureRecognizer) {
            pulse(button: 0)
        }

        @objc func handleRightTap(_ recognizer: UITapGestureRecognizer) {
            pulse(button: 1)
        }

        /// `handleMouseDown/Up`(`InputManager.swift:686-698`)と同じ 2 呼び出しを、
        /// タッチの離散性に合わせて down → 短い待ち → up のパルスとして出す。
        private func pulse(button: Int32) {
            // 既に押下中なら二重に down を送らない(取りこぼした up の保険)。
            guard !heldButtons.contains(button) else { return }
            heldButtons.insert(button)
            mx68k_mouse_button(button, true)
            DispatchQueue.main.asyncAfter(deadline: .now() + Self.clickPulseSeconds) { [weak self] in
                guard let self, self.heldButtons.remove(button) != nil else { return }
                mx68k_mouse_button(button, false)
            }
        }

        /// 押しっぱなしのボタンを確実に解放する(レイヤー撤去時の保険)。
        func releaseButtons(reason: String) {
            guard !heldButtons.isEmpty else { return }
            let released = heldButtons.sorted()
            heldButtons.removeAll()
            for button in released { mx68k_mouse_button(button, false) }
            let list = released.map(String.init).joined(separator: ",")
            mx68k_log("[Swift][iOS][P714-MOUSE] release reason=\(reason) buttons=\(list)")
        }
    }
}

/// 描画物を持たないビューは既定ではタッチを受けない場合があるため、
/// 背景色を透明のまま **矩形全域を自分の領域として主張する**サブクラス。
/// (`UIView` は `backgroundColor` が `.clear` でも `point(inside:)` は
///  bounds 判定なのでそのまま通るが、意図を明示するために型を分けてある。)
private final class TouchMouseSurface: UIView {
    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        bounds.contains(point)
    }
}
