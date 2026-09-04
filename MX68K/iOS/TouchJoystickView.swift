//
//  TouchJoystickView.swift
//  MX68K-iOS
//
//  P714: 固定位置のオンスクリーン仮想ジョイスティック(D-pad 4 方向 + TRIG1/TRIG2)。
//  P715: D-pad をアナログスティック風の `AnalogStickPad`(円形ベース + ドラッグ可能な
//        ノブ)へ置換し、トリガーボタンを 68 → 82pt へ拡大。
//  P716: トリガーボタンのサイズを 3 段階(Small 82 / Medium 103 / Large 123pt)から
//        選べるようにし、可変になったサイズに応じた幅ゲート
//        (`requiredWidth(forTriggerLevel:)`)を `MX68KiOSApp` へ提供する。
//        ★送出されるビット自体は P714 から不変 —— X68000 のジョイスティックポートは
//          デジタル信号(方向ビット 4 本)のみでアナログ値の概念が無いため、
//          「アナログスティック風」は**見た目とドラッグ操作の体験のみ**を指す。
//          ノブのオフセットはデッドゾーン `dz` で 4 方向の量子化ビットへ落とす。
//
//  ★規範(normative reference)は **MX68K 自身の macOS 実装**
//    `MX68K/App/Services/InputManager.swift:791-810`(負論理ビット合成)である。
//    ビット位置・idle 値は 1 つも発明しておらず、既存の確立済み定数の再利用のみ
//    (P714 計画 §記号表)。権威ソースは `Bridge/EmulatorBridge.h` の
//    `mx68k_joy_set` コメント / upstream joystick.h。
//
//  ★P705 の `IOSKeyboardInput` と同じ事情でこのファイルが必要になっている:
//    `InputManager.swift` は `import AppKit` / `import Carbon` を持つ **macOS 専用**
//    ファイルであり、iOS ターゲットの Sources phase に入っていない
//    (`ruby Scripts/add_ios_target.rb dump-sources MX68K-iOS` で確認済み)。
//    したがって P714 計画 §変更内容 5 が指示する「`InputManager.swift` へ
//    `setVirtualPadBit` / `releaseVirtualPad` を追加する」は iOS からは到達不能で、
//    P705 が `IOSKeyboardInput` を新設したのと同じ形 —— iOS 側に等価物を置く ——
//    で実装している(§実装差分 D-1、オーケストレーターへ報告済み)。
//

import SwiftUI

// MARK: - 入力状態(ビット合成の唯一の集約点)

/// 仮想パッドの押下状態を 1 バイトへ合成し、`mx68k_joy_set` へ送出する。
///
/// P714 計画 §変更内容 5 —— 「複数ボタン同時押下時の OR/AND マスク計算を View 側に
/// 持たせない」ための集約点。View は「どのビットが押された/離された」だけを伝える。
///
/// ★シングルトンである理由: この状態は SwiftUI のオーバーレイ
/// (`TouchJoystickView`)と UIKit 側のライフサイクルフック
/// (`EmulatorMTKView_iOS` のバックグラウンド遷移監視)の **両方**から触られる。
/// 後者はビュー階層を通らない経路なので、`@StateObject` を親から配るだけでは
/// 届かない。macOS 側 `InputManager.shared` と同じ形をとる。
///
/// ★スレッド規約: すべてメインスレッドから呼ぶ。SwiftUI のジェスチャは
/// メインスレッドで配送され、バックグラウンド遷移の通知観測も `queue: .main` で
/// 登録されている(`EmulatorMetalView_iOS.swift` の既存規約)。
final class TouchJoystickInput {

    static let shared = TouchJoystickInput()

    private init() {}

    // MARK: - ビット定数(InputManager.swift:801-806 からの逐語転写)
    //
    // 負論理: idle = 0xFF を起点に、押下されたビットを **クリア**する。
    // bit4 / bit7 は常時 1(未使用)。

    /// bit0 = Up (`InputManager.swift:801`)
    static let bitUp: UInt8 = 0x01
    /// bit1 = Down (`InputManager.swift:802`)
    static let bitDown: UInt8 = 0x02
    /// bit2 = Left (`InputManager.swift:803`)
    static let bitLeft: UInt8 = 0x04
    /// bit3 = Right (`InputManager.swift:804`)
    static let bitRight: UInt8 = 0x08
    /// bit5 = TRIG2 (`InputManager.swift:806`)
    static let bitTrig2: UInt8 = 0x20
    /// bit6 = TRIG1 (`InputManager.swift:805`)
    static let bitTrig1: UInt8 = 0x40

    /// 全ビット未押下の既定値(`InputManager.clearGamepadPort` の
    /// `mx68k_joy_set(port, 0xFF)` と同一)。
    private static let idle: UInt8 = 0xFF

    /// 現在ゲストへ送出済みの bank0 バイト。
    private var virtualPadState: UInt8 = TouchJoystickInput.idle

    /// 1 ビット分の押下/解放を反映し、変化したときだけゲストへ送出する。
    ///
    /// - Parameters:
    ///   - bit: 上の `bitUp` 〜 `bitTrig1` のいずれか(負論理でクリアされる側)。
    ///   - pressed: 押下なら `true`。
    ///   - port: 送出先ポート。仮想パッドは **port0 (JOY1) 固定**
    ///     (2 台目の仮想パッドは P714 のスコープ外)。
    func setVirtualPadBit(_ bit: UInt8, pressed: Bool, port: Int32 = 0) {
        let next = pressed ? (virtualPadState & ~bit) : (virtualPadState | bit)
        // 同じ値を毎フレーム送り直さない(SoftKeyButton の isDown ガードと同趣旨)。
        guard next != virtualPadState else { return }
        virtualPadState = next
        mx68k_joy_set(port, next)
    }

    // MARK: - オートファイア(連射)状態(P724)

    /// ビットごとのオートファイアタイマー。`releaseVirtualPad` が一括停止できるよう
    /// **このシングルトン内に集約**する(`TouchPadButton` の `@State` には持たせない)。
    ///
    /// ★集約する理由: 押しっぱなし解除(`releaseVirtualPad(reason:)`)は
    ///   バックグラウンド遷移・マウスモード ON・パッド非表示・幅ゲート等、
    ///   計 6 箇所の契機から呼ばれる(`EmulatorMetalView_iOS.swift` の
    ///   `didEnterBackground` / `pressesCancelled` / `resignFirstResponder` の 3 箇所 +
    ///   `MX68KiOSApp.swift` の `mouseModeOn` / `padHidden` / `padWidthGate` の 3 箇所)。
    ///   タイマーを個々の `TouchPadButton` の `@State` に持たせると、これらの契機で
    ///   `releaseVirtualPad` が呼ばれてもタイマー自体は生き残り、指を離していない/
    ///   離せない状況でも周期的な押下/解放ビットが送出され続ける「暴走連射」という
    ///   新種の押しっぱなしバグを生む。ここに集約すれば、既存 6 箇所の呼び出し契機を
    ///   **1 行も変更せずに**オートファイアの停止も自動的にカバーできる。
    private var autoFireTimers: [UInt8: Timer] = [:]

    /// 直近に送出したフェーズ(true=押下 / false=解放)。次回のトグル先を決める。
    private var autoFirePhase: [UInt8: Bool] = [:]

    /// フェーズ(押下/解放)の継続時間。フルサイクル(押下+解放)はこの 2 倍 ——
    /// 33ms × 2 ≒ 66ms ≒ 15.2Hz(P724 計画 §記号表)。
    ///
    /// ★実機 X68000 のジョイスティックポートはデジタル信号のみで連射機構そのものが
    ///   存在しないため、この値に模倣元となる一次情報源は無い。一般的なターボファイア
    ///   周期(10〜30Hz)のほぼ中央値として本サイクルが新規に定めたホスト側 UI 規約であり、
    ///   周期の可変設定(スライダー等)は本サイクルのスコープ外。
    private static let autoFirePhaseInterval: TimeInterval = 0.033

    /// このビットのオートファイアを開始する。既に開始済みなら何もしない
    /// (`TouchPadButton.onChanged` は 1 回の押下で複数回発火し得るため、
    ///  `isDown` ガードと合わせた二重の安全策)。
    func startAutoFire(bit: UInt8, port: Int32 = 0) {
        guard autoFireTimers[bit] == nil else { return }
        setVirtualPadBit(bit, pressed: true, port: port)
        autoFirePhase[bit] = true
        // ★`Timer.scheduledTimer(withTimeInterval:repeats:)` は生成と同時に現在の
        //   RunLoop へ `.default` モードで自動登録される。その後さらに
        //   `RunLoop.main.add(_:forMode:.common)` を呼ぶと同一タイマーの二重登録になる。
        //   非自動登録の `Timer(timeInterval:repeats:block:)` で生成し、
        //   `.common` モードへの登録を 1 回だけ行う。
        let timer = Timer(timeInterval: Self.autoFirePhaseInterval, repeats: true) { [weak self] _ in
            guard let self else { return }
            let next = !(self.autoFirePhase[bit] ?? true)
            self.autoFirePhase[bit] = next
            self.setVirtualPadBit(bit, pressed: next, port: port)
        }
        // ★`.common` モードで登録する —— 既定の `.default` モードは、UIKit の
        //   イベント追跡が `.tracking` ランループモードへ切り替わる文脈下では
        //   一時停止し得る(古典的な Timer の落とし穴)。ドラッグジェスチャ保持中に
        //   確実に発火させるための保険。
        RunLoop.main.add(timer, forMode: .common)
        autoFireTimers[bit] = timer
        mx68k_log(String(format: "[Swift][iOS][P724-AUTOFIRE] start bit=0x%02X", Int(bit)))
    }

    /// このビットのオートファイアを停止し、確実に解放状態(ビット OFF)へ戻す。
    ///
    /// ★停止時に必ず `pressed: false` を送る —— タイマーが押下フェーズの途中で
    ///   止まった場合、ビットが立ったまま取り残される(押しっぱなしと同じ状態)。
    func stopAutoFire(bit: UInt8, port: Int32 = 0) {
        guard autoFireTimers[bit] != nil else { return }
        autoFireTimers[bit]?.invalidate()
        autoFireTimers.removeValue(forKey: bit)
        autoFirePhase.removeValue(forKey: bit)
        setVirtualPadBit(bit, pressed: false, port: port)
        mx68k_log(String(format: "[Swift][iOS][P724-AUTOFIRE] stop bit=0x%02X", Int(bit)))
    }

    /// 保持中の全ビットを idle へ戻す(押しっぱなし解除)。
    ///
    /// P714 計画 §変更内容 5(Code Review R-2)—— これを怠ると、指を離す前に
    /// 「物理コントローラ接続 / マウスモード ON / バックグラウンド遷移」が起きた
    /// 場合に方向・トリガービットが送出され続け、「キャラクターが勝手に動き続ける」
    /// という体感上明確な不具合になる。P705 の `IOSKeyboardInput.releaseAll(reason:)`
    /// と同じ役割・同じ呼び出し点。
    ///
    /// ★bank1(`mx68k_joy_set1`)は送らない —— `setVirtualPadBit` が bank1 を
    ///   一度も触らないため、戻すべき状態が存在しない(P714 レビュー指摘の簡素化)。
    func releaseVirtualPad(reason: String, port: Int32 = 0) {
        // P724 — 進行中のオートファイアを全て停止する。これを怠ると、この関数を
        // 呼ぶ既存 6 箇所の契機(バックグラウンド遷移・マウスモード ON・パッド非表示・
        // 幅ゲート等)の後もタイマーが生き残り、指を離していないのに周期的な
        // 押下/解放が送出され続ける(`autoFireTimers` の設計理由と対になる安全策)。
        // ★個別に `stopAutoFire` を呼ばない —— 直後の idle リセットが全ビットを
        //   まとめて OFF にするため、ビットごとの解放送出は冗長。
        for timer in autoFireTimers.values { timer.invalidate() }
        autoFireTimers.removeAll()
        autoFirePhase.removeAll()

        // ★ログには**解放前**の状態を出す(解放後は常に 0xFF になり証拠にならない)。
        //   `IOSKeyboardInput.releaseAll` と同じ流儀。
        let had = virtualPadState
        virtualPadState = Self.idle
        mx68k_joy_set(port, Self.idle)
        mx68k_log(String(format: "[Swift][iOS][P714-PADRELEASE] reason=%@ was=0x%02X port=%d",
                         reason, Int(had), Int(port)))
    }
}

// MARK: - 1 個のタッチボタン

/// `DragGesture(minimumDistance: 0)` でタッチの down/up を読む押しボタン。
///
/// ★SwiftUI の `Button` を使わない —— P712f で「同一行に隣接して並べた `Button` が
///   同時発火する」不具合を経験している(P714 当時は 4 個密集する D-pad が
///   まさにその構図だった。P715 で D-pad は `AnalogStickPad` へ置換されたが、
///   隣接して並ぶ TRIG2/TRIG1 には同じ理由がそのまま当てはまる)。
///   `SoftKeyButton`(`SoftKeyboardView.swift:456-475`)が既に採用している
///   DragGesture 方式をそのまま踏襲し、構造的に回避する(P714 計画 §変更内容 1)。
private struct TouchPadButton: View {
    let bit: UInt8
    let label: String
    /// SF Symbol 名。nil ならテキスト `label` を出す。
    let iconName: String?
    let diameter: CGFloat
    /// 角丸矩形(D-pad)か円(トリガー)か。
    let isCircle: Bool
    /// P724 — true ならこのボタンは押している間オートファイア(連射)する。
    /// 呼び出し側(`TouchJoystickView.triggers`)が `@AppStorage` の設定値を渡す。
    ///
    /// ★見た目は分岐させない —— オートファイア中も通常の押下と同じ
    ///   `isDown ? Color.accentColor : ...` のハイライトのみで、タイマーによる
    ///   内部的なフェーズ切替を UI 上で点滅させたりはしない(ユーザーから要望の
    ///   無い視覚効果を発明しない)。
    let autoFireEnabled: Bool

    /// このジェスチャセッション中に既に down を送ったか
    /// (`SoftKeyButton.isDown`(`SoftKeyboardView.swift:362`)の逐語移植 ——
    ///  `.onChanged` は 1 回のタッチで何度も発火する)。
    @State private var isDown = false

    var body: some View {
        let shape = isCircle ? TouchPadShape(Circle())
                             : TouchPadShape(RoundedRectangle(cornerRadius: 10))
        return shape
            .fill(isDown ? Color.accentColor : Color.black.opacity(0.55))
            .overlay(shape.stroke(Color.white.opacity(0.7), lineWidth: 1.5))
            .overlay(glyph)
            .frame(width: diameter, height: diameter)
            // ★塗りつぶし済みの図形なので既定でヒットテストされるが、
            //   縁取りだけの状態でも指が必ず拾えるよう明示する。
            .contentShape(shape)
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        // 最初の 1 回だけ down を送る。SwiftUI はジェスチャ開始後も
                        // セッションを追跡し続ける(ライブなヒットテストではない)ため、
                        // 指がボタン外へ出ても onEnded は確実に発火する
                        // (SoftKeyboardView.swift:462-465 と同じ根拠)。
                        if !isDown {
                            isDown = true
                            // P724 — 連射 ON のボタンだけタイマー駆動へ分岐する。
                            // OFF 側は P714 からの既存経路をそのまま通る。
                            if autoFireEnabled {
                                TouchJoystickInput.shared.startAutoFire(bit: bit)
                            } else {
                                TouchJoystickInput.shared.setVirtualPadBit(bit, pressed: true)
                            }
                        }
                    }
                    .onEnded { _ in
                        isDown = false
                        if autoFireEnabled {
                            TouchJoystickInput.shared.stopAutoFire(bit: bit)
                        } else {
                            TouchJoystickInput.shared.setVirtualPadBit(bit, pressed: false)
                        }
                    }
            )
            .accessibilityLabel(label)
    }

    @ViewBuilder private var glyph: some View {
        if let iconName {
            Image(systemName: iconName)
                .font(.system(size: diameter * 0.42, weight: .semibold))
                .foregroundColor(.white)
        } else {
            Text(label)
                .font(.system(size: diameter * 0.26, weight: .bold, design: .rounded))
                .foregroundColor(.white)
                // 現行のラベルは "A" / "B" の 1 文字だけなので円の内接幅には余裕がある。
                // 以下 3 行は P717 以前の "TRIG1"(5 文字)時代の名残だが、短い文字列でも
                // 無害なため、将来ラベルが長くなった場合の保険としてそのまま残す。
                .minimumScaleFactor(0.5)
                .lineLimit(1)
                .padding(.horizontal, 4)
        }
    }
}

/// 形状を 1 つの値として持ち回るための型消去 `Shape`。`fill` / `stroke` /
/// `contentShape` の 3 箇所で **同一の形**を使うためだけのもの(形状が 3 箇所へ
/// 分裂すると、見た目とタップ領域が静かにズレる)。
///
/// ★SwiftUI の `AnyShape` は iOS 17+ でしか使えず(本ターゲットの
///   `IPHONEOS_DEPLOYMENT_TARGET` は 16.0)、かつ同名で定義すると標準型と
///   名前が衝突して解決が実装依存になる。別名の自前実装にしてある。
private struct TouchPadShape: Shape {
    private let pathBuilder: (CGRect) -> Path

    init<S: Shape>(_ shape: S) {
        pathBuilder = { rect in shape.path(in: rect) }
    }

    func path(in rect: CGRect) -> Path { pathBuilder(rect) }
}

// MARK: - アナログスティック風の方向入力(P715)

/// 円形ベース + ドラッグ可能な円形ノブ。見た目と操作感はアナログスティックだが、
/// ゲストへ送るのは **P714 と同じ 4 方向のデジタルビット**である
/// (X68000 のジョイスティックポートにアナログ値の概念は無い)。
///
/// ノブのオフセットを正規化(-1...1)し、デッドゾーン `dz` を超えた軸だけ
/// 該当する方向ビットを ON にする —— 両軸が同時に閾値を超えれば 2 ビットが
/// 同時に立ち、斜め入力は自然に成立する(`TouchJoystickInput` 側の既存の
/// ビット合成がそのまま扱うため、斜め専用のロジックは追加しない)。
private struct AnalogStickPad: View {

    /// ベース円の直径。
    let baseDiameter: CGFloat
    /// ノブ円の直径。
    let knobDiameter: CGFloat

    /// ノブ中心が中央から離れられる最大距離。
    /// ベース内へノブが完全に収まる条件そのもの(半径差)。
    private var maxRadius: CGFloat { (baseDiameter - knobDiameter) / 2 }

    /// 方向ビット ON のしきい値(正規化オフセットの絶対値)。
    ///
    /// ★`InputManager.swift:795` の `let dz: Float = 0.5`(物理 GameController の
    ///   左スティック量子化で使用中)からの値の再利用 —— 物理コントローラと
    ///   タッチ操作で感度の一貫性を保つ。**値だけ**の引用であり、符号規約は
    ///   同一ではない(下記 `apply(_:)` を参照)。
    private static let dz: CGFloat = 0.5

    /// 現在のノブ表示オフセット(クランプ済み)。
    @State private var knobOffset: CGSize = .zero
    /// ドラッグ中か(ノブのハイライト表示に使う)。
    @State private var isDragging = false

    var body: some View {
        ZStack {
            Circle()
                .fill(Color.black.opacity(0.55))
                .overlay(Circle().stroke(Color.white.opacity(0.7), lineWidth: 1.5))
            Circle()
                .fill(isDragging ? Color.accentColor : Color.white.opacity(0.32))
                .overlay(Circle().stroke(Color.white.opacity(0.7), lineWidth: 1.5))
                .frame(width: knobDiameter, height: knobDiameter)
                .offset(knobOffset)
        }
        .frame(width: baseDiameter, height: baseDiameter)
        // ★ベース**全体**がジェスチャ開始領域 —— ノブに正確に触れなくても
        //   ベース内であればドラッグを開始できる(実際のアナログスティックの操作感)。
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    isDragging = true
                    let clamped = Self.clampToBase(dx: value.translation.width,
                                                   dy: value.translation.height,
                                                   maxRadius: maxRadius)
                    knobOffset = clamped
                    apply(clamped)
                }
                .onEnded { _ in
                    isDragging = false
                    withAnimation(.easeOut(duration: 0.12)) {
                        knobOffset = .zero
                    }
                    // 4 方向すべてを解放。`setVirtualPadBit` は値が変化した分だけ
                    // 実際に送出するため、既に OFF のビットへの重複指定は無害。
                    let input = TouchJoystickInput.shared
                    input.setVirtualPadBit(TouchJoystickInput.bitUp, pressed: false)
                    input.setVirtualPadBit(TouchJoystickInput.bitDown, pressed: false)
                    input.setVirtualPadBit(TouchJoystickInput.bitLeft, pressed: false)
                    input.setVirtualPadBit(TouchJoystickInput.bitRight, pressed: false)
                }
        )
        .accessibilityLabel("Analog stick")
    }

    /// 生のドラッグオフセットを、ノブがベースからはみ出さない範囲へ収める。
    ///
    /// ★**ベクトル長**でクランプする(軸ごとに独立にクランプしてはならない)——
    ///   軸独立クランプでは斜め方向で中心からの距離が最大 `maxRadius * √2`
    ///   (45pt なら約 63.6pt)まで伸び、「ノブがベース内に収まる」という
    ///   性質そのものが破れる。
    private static func clampToBase(dx: CGFloat, dy: CGFloat, maxRadius: CGFloat) -> CGSize {
        let dist = sqrt(dx * dx + dy * dy)
        guard dist > maxRadius else { return CGSize(width: dx, height: dy) }
        // `dist > maxRadius` かつ `maxRadius > 0` なので dist は必ず正 —— 0 除算は起きない。
        let scale = maxRadius / dist
        return CGSize(width: dx * scale, height: dy * scale)
    }

    /// クランプ済みオフセットを 4 方向のデジタルビットへ量子化して送出する。
    ///
    /// ★符号規約 —— SwiftUI の `DragGesture` の `translation.height` は
    ///   **正 = 下方向**である一方、`InputManager.swift:797` が使う
    ///   `GCExtendedGamepad.leftThumbstick.yAxis` は Apple の規約で **正 = 上方向**。
    ///   したがって y 軸の比較は `InputManager.swift` とは**逆**になる
    ///   (`ny > dz` → Down、`ny < -dz` → Up)。x 軸は符号が一致するのでそのまま。
    ///   `dz` の値 0.5 自体は `InputManager.swift:795` からの引用で不変。
    private func apply(_ offset: CGSize) {
        let nx = offset.width / maxRadius   // -1...1、正 = 右
        let ny = offset.height / maxRadius  // -1...1、正 = 下
        let dz = Self.dz
        let input = TouchJoystickInput.shared
        input.setVirtualPadBit(TouchJoystickInput.bitLeft,  pressed: nx < -dz)
        input.setVirtualPadBit(TouchJoystickInput.bitRight, pressed: nx >  dz)
        input.setVirtualPadBit(TouchJoystickInput.bitUp,    pressed: ny < -dz)
        input.setVirtualPadBit(TouchJoystickInput.bitDown,  pressed: ny >  dz)
    }
}

// MARK: - オーバーレイ本体

/// 画面左下にアナログスティック(P715、P714 では D-pad)、右下に TRIG2/TRIG1 を
/// 固定配置する半透明オーバーレイ。
///
/// ★このビューは **レイアウトに参加しない**(P710 のソフトキーボード帯と同じ設計原則)。
///   `ZStack` の一員として重なるだけで、Metal ビューの寸法も `geo.size` も変えない。
///
/// ★背景を敷かない —— コンテナ全体に `Color.clear` 等を敷くと画面全域が
///   ヒットテスト対象になり、ボタン以外の場所のタッチまで飲み込んでしまう。
///   描画物のない領域は SwiftUI のヒットテストを素通りする。
struct TouchJoystickView: View {

    /// P714 計画 §変更内容 6 —— 透明度はホスト側の純粋な表示設定なので
    /// `config.json`(ハードリセット同期対象)ではなく `@AppStorage` に置く。
    /// Hardware 設定タブのスライダーと **同じキー**を読むことでリアルタイムに連動する。
    @AppStorage("virtualPadOpacity") private var virtualPadOpacity: Double = 0.6

    /// P716 — トリガーボタンのサイズ段階(0=Small / 1=Medium / 2=Large)。
    /// 透明度と同じ理由で `@AppStorage`(ホスト側の純粋な表示設定であり
    /// `config.json` 管轄外)。Hardware 設定タブの Picker と **同じキー**を読む。
    @AppStorage("triggerButtonSizeLevel") private var triggerButtonSizeLevel: Int = 0

    /// P724 — トリガー A / B それぞれのオートファイア(連射)有効/無効。
    /// 既定 false は既存挙動(P714-P717 相当)をそのまま維持するための opt-in 設計。
    /// `triggerButtonSizeLevel` と同じ理由で `@AppStorage`(ホスト側の純粋な操作設定で
    /// あり `config.json` 管轄外)。Hardware 設定タブのトグルと **同じキー**を読むため、
    /// 切り替えると即座に反映される。
    @AppStorage("triggerAutoFireA") private var triggerAutoFireA: Bool = false
    @AppStorage("triggerAutoFireB") private var triggerAutoFireB: Bool = false

    /// 帯(P708 の側方配置時の左右ピラーボックス列)と重ならないための左右インセット。
    /// ルートビューが `DisplayViewport.face()` から得た `sideW` をそのまま渡す ——
    /// ここで幾何計算を再発明しない(単一情報源)。
    let sideInset: CGFloat

    /// アナログスティックのベース円の直径(P715)。
    ///
    /// ★P716 で `private let` → `static let` へ格上げ: `MX68KiOSApp` 側の幅ゲート
    ///   (`requiredWidth(forTriggerLevel:)`)がこの値を必要とするため。値は不変。
    static let stickBaseDiameter: CGFloat = 150
    /// アナログスティックのノブ円の直径(P715)。
    /// ★ベース径とは独立した専用の定数 —— 他のボタン径から派生させない。
    private let stickKnobDiameter: CGFloat = 60

    /// TRIG2 / TRIG1 の間隔(P715 のハードコード値 14 を P716 で名前付き定数へ切り出し)。
    /// ★`triggers` の `HStack(spacing:)` と幅ゲートの必要幅計算の **両方**がこれを読む ——
    ///   値を 2 箇所に書かない(P710 §C-5 の閾値単一情報源の規律と同型)。
    static let triggerSpacing: CGFloat = 14

    /// P716 — 段階からトリガーボタンの径を算出する唯一の関数。
    /// `trigButtonSize`(描画側)と `requiredWidth(forTriggerLevel:)`(幅ゲート側)の
    /// 両方がここを呼ぶため、サイズ定義が 2 箇所へ分裂しない。
    ///
    /// 倍率 `[1.0, 1.25, 1.5]` × 基準 82pt = 82 / 103(102.5 の四捨五入)/ 123pt。
    /// ★`UserDefaults` に想定外の値が入っていても `min(max(level, 0), 2)` で
    ///   配列範囲内へ収める(0 未満・3 以上は両端へ丸められ、既定の「小」相当か
    ///   最大の「大」へフォールバックする)。
    static func triggerSize(forLevel level: Int) -> CGFloat {
        let multipliers: [CGFloat] = [1.0, 1.25, 1.5]
        let index = min(max(level, 0), 2)
        return (baseTrigButtonSize * multipliers[index]).rounded()
    }

    /// 「小」= P715 の `trigButtonSize` 固定値(P716 の基準値)。
    private static let baseTrigButtonSize: CGFloat = 82

    /// P716 — 仮想パッド帯を表示するのに必要な最小コンテナ幅。
    ///
    /// 内訳: スティックのベース径 + (トリガー 2 個 + その間隔) + 左右 padding。
    /// 末尾の `32` は `body` の `.padding(.horizontal, sideInset + 16)` の左右分
    /// `16 × 2` である。縦向きでは `DisplayViewport.face()` が `x == 0` を返す
    /// (`MX68K/App/Views/MainWindow/DisplayGeometry.swift:58-66` —— 縦向きは
    /// `vpH = dw / (4/3) < dh` となり `vpW = dw`、したがって `x = (dw - vpW)/2 = 0`)
    /// ため `sideInset` は 0 で、この式がそのままワーストケースの必要幅になる。
    /// 横向きの側方配置では `sideInset` が上乗せされるが、横向きは画面幅自体が
    /// 大きいためワーストケースにはならない。
    static func requiredWidth(forTriggerLevel level: Int) -> CGFloat {
        stickBaseDiameter + (triggerSize(forLevel: level) * 2 + triggerSpacing) + 32
    }

    /// トリガーボタンの径(P715 で 68 → 82 へ拡大、P716 で 3 段階の可変値へ)。
    /// ★格納定数ではなく **計算プロパティ** —— `triggerButtonSizeLevel` の変更が
    ///   `@AppStorage` 経由でそのまま再描画へ伝わる。
    private var trigButtonSize: CGFloat { Self.triggerSize(forLevel: triggerButtonSizeLevel) }

    var body: some View {
        HStack(alignment: .bottom) {
            AnalogStickPad(baseDiameter: Self.stickBaseDiameter,
                           knobDiameter: stickKnobDiameter)
            Spacer(minLength: 0)
            triggers
        }
        .padding(.horizontal, sideInset + 16)
        // 上下の帯(topBand / bottomBand)へ被らないための余白。
        .padding(.bottom, 44)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
        // ★コンテナ全体へ 1 回だけ掛ける —— 背景とグリフが同じ係数で連動して薄くなる
        //   (P714 計画 §変更内容 1、ユーザー指摘)。`.opacity()` は SwiftUI の
        //   ヒットテスト領域を変えないため、薄くしてもタップ領域は不変。
        .opacity(virtualPadOpacity)
    }

    /// TRIG2 / TRIG1。実機のジョイスティックは A(TRIG1)が親指側に来るため、
    /// 右端(親指に近い側)を TRIG1 にする。
    private var triggers: some View {
        HStack(spacing: Self.triggerSpacing) {
            // ★連射設定はラベルに対応させる —— B ボタン(bitTrig2)は
            //   `triggerAutoFireB`、A ボタン(bitTrig1)は `triggerAutoFireA`。
            TouchPadButton(bit: TouchJoystickInput.bitTrig2, label: "B",
                           iconName: nil,
                           diameter: trigButtonSize, isCircle: true,
                           autoFireEnabled: triggerAutoFireB)
            TouchPadButton(bit: TouchJoystickInput.bitTrig1, label: "A",
                           iconName: nil,
                           diameter: trigButtonSize, isCircle: true,
                           autoFireEnabled: triggerAutoFireA)
        }
    }
}
