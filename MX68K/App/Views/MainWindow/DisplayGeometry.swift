//
//  DisplayGeometry.swift
//  MX68K
//
//  P703: EmulatorMetalView.swift から**内容を変えずに**切り出した、表示ジオメトリの
//  純粋な数学部分(Vertex / DisplayGeometry / DisplayViewport)。AppKit にも UIKit にも
//  依存しないため、macOS ターゲットと iOS ターゲットの両方が同じこのファイルを
//  コンパイルする。
//
//  ★DisplayFilter は意図的にここへ移していない —— macOS の設定 UI からしか参照されず、
//    レンダラ側は UserDefaults のキー文字列を直読みしているため、共有面を増やすだけで
//    利得がない(P703 計画 §B-1)。EmulatorMetalView.swift に残してある。
//

import Foundation
import CoreGraphics
import simd

struct Vertex {
    var position: SIMD2<Float>
    var texCoord: SIMD2<Float>
}

/// P595 (D-55) — Bridge が公開する表示ジオメトリ。単位は「標準表示窓 = 1.0」で、
/// 4:3 面(`DisplayViewport.face`)の中で実際の映像が占める矩形を表す。
/// 標準ラスタでは恒等(1.0/1.0/0.0/0.0)= P212 と完全に同じ表示になる。
/// `geoMode` は診断・モニタ表示専用で矩形計算には使わない
/// (0=恒等 / 1=標準R00・R04 / 2=非標準R00の対称中央寄せ / 9=妥当性外・R04非標準)。
struct DisplayGeometry: Equatable {
    var hScale: CGFloat = 1.0
    var vScale: CGFloat = 1.0
    var offX: CGFloat = 0.0
    var offY: CGFloat = 0.0
    var geoMode: Int = 0

    static let identity = DisplayGeometry()
}

/// P212 — X68000 の**標準的な画面モード**(256×256/512×512/768×512 等)は、いずれも
/// 物理モニタ上で同一の矩形を占める(テクニカルデータブック 表2-10/表2-12 の相互検算、
/// P592/P595 調査で `[一次資料]` として確定)。よって MX は画面モードによらず固定 4:3 の
/// 「面」へ表示する。★P595 訂正: 旧コメントの「XM6/XEiJ 一致」という注記は無出典で、
/// P592 の XM6 ソース直読(`XM6:` `vm/crtc.cpp` / `mfc/mfc_draw.cpp`)と一致しない——
/// XM6/XEiJ は R00 を幾何に使わない走査速度一定モデルで、標準ラスタでは MX と同じ結果に
/// なるが「全モードを固定 4:3 へ引き伸ばす」実装ではない。固定 4:3 面の根拠は一次資料の
/// 方であり、参照実装の一致ではない。
/// draw() の viewport とマウス写像(InputManager.handleMouseMoved)が **同一の矩形**を
/// 使うための単一情報源。container(draw()=drawableSize[px] / mouse=viewSize[pt])に
/// 対し 4:3 を維持してフィットした面を返す。両座標系は backingScale の比例関係ゆえ
/// 同一の式で幾何的に一致する = rect を別々に min(...) で再計算しないことで P196 の
/// マウスずれ回帰を構造的に防ぐ。
enum DisplayViewport {
    static let targetAspect: CGFloat = 4.0 / 3.0

    /// container 座標系での 4:3 レターボックス「面」(originX, originY, width, height)。
    /// P212 の固定 4:3 面そのもの(標準的な画面モードは全て物理モニタ上で同一の矩形を
    /// 占める = テクニカルデータブック 表2-10/表2-12、P592/P595 調査で確定)。
    static func face(container: CGSize) -> (x: CGFloat, y: CGFloat, w: CGFloat, h: CGFloat) {
        let dw = container.width
        let dh = container.height
        guard dw > 0, dh > 0 else { return (0, 0, 0, 0) }
        var vpW = dw
        var vpH = dw / targetAspect
        if vpH > dh { vpH = dh; vpW = dh * targetAspect }   // アスペクト維持レターボックス
        return ((dw - vpW) / 2, (dh - vpH) / 2, vpW, vpH)
    }

    /// P595: 4:3 面の中に geom が示す矩形を配置する(Metal ビューポート = 上基準)。
    static func fitMetal(container: CGSize, geom: DisplayGeometry) -> (x: CGFloat, y: CGFloat, w: CGFloat, h: CGFloat) {
        let f = face(container: container)
        return (f.x + f.w * geom.offX, f.y + f.h * geom.offY, f.w * geom.hScale, f.h * geom.vScale)
    }

    /// P595: 同じ矩形を AppKit 座標系(左下原点)で返す。マウス写像用。
    /// y は上基準の offY を下基準へ反転した `1 - offY - vScale` を使う。
    static func fitAppKit(container: CGSize, geom: DisplayGeometry) -> (x: CGFloat, y: CGFloat, w: CGFloat, h: CGFloat) {
        let f = face(container: container)
        return (f.x + f.w * geom.offX, f.y + f.h * (1 - geom.offY - geom.vScale), f.w * geom.hScale, f.h * geom.vScale)
    }
}

/// P703 — X68KRenderer が結果を publish する相手の最小面。
/// macOS は EmulatorViewModel、iOS は MX68KiOSViewModel が適合する。
/// weak 参照で保持するため AnyObject 制約が必須。
protocol RendererHost: AnyObject {
    var framebufferSize: CGSize { get set }
    var displayGeometry: DisplayGeometry { get set }
}
