//
//  X68KRenderer.swift
//  MX68K
//
//  P703: EmulatorMetalView.swift から切り出した Metal レンダラ。**純粋な再配置**であり、
//  描画ロジックには一切手を入れていない。macOS ターゲットと iOS ターゲットの両方が
//  同じこのファイルをコンパイルする。
//
//  移設にともなう変更は次の 2 点だけ(P703 計画 §B-2):
//   - `viewModel` の静的型を `EmulatorViewModel?` → `(any RendererHost)?` へ。
//     プロパティ名は `viewModel` のまま(呼び出し側は無改変)。
//   - AppKit 結合の 3 行(EmulatorMTKView.hostView への供給 2 行と
//     InputManager.requestMouseHoming())を `#if os(macOS)` で囲む。`#else` は空 ——
//     iOS にはマウス写像もホーミングも存在しないため。macOS 側の生成コードは
//     `#if` 導入前とバイト同一になる(常に真の分岐)。
//

import Foundation
import Metal
import MetalKit

class X68KRenderer: NSObject, MTKViewDelegate {
    var device: MTLDevice
    var commandQueue: MTLCommandQueue
    var pipelineState: MTLRenderPipelineState
    var vertexBuffer: MTLBuffer
    // P199 — 表示フィルタ Smooth/Sharp の切替用サンプラ(init で 1 度だけ生成)。
    let nearestSampler: MTLSamplerState
    let linearSampler: MTLSamplerState
    var texture: MTLTexture?
    var textureWidth: Int = 0
    var textureHeight: Int = 0
    /// P190 — framebuffer 実寸を公開するための参照。weak(循環参照回避)。
    /// P703 — 具体型ではなく `RendererHost` 越しに publish する
    /// (macOS = EmulatorViewModel / iOS = MX68KiOSViewModel)。
    weak var viewModel: (any RendererHost)?
    /// P595 (D-55) — Bridge から毎フレーム取得する表示ジオメトリ。`draw(in:)` は
    /// Metal スレッドで実行されるため、ここは `@Published` ではないプレーンな var で
    /// よい(読み書きは常にそのスレッド内で完結する)。
    var currentGeom: DisplayGeometry = .identity

    init?(device: MTLDevice) {
        self.device = device
        guard let queue = device.makeCommandQueue() else { return nil }
        self.commandQueue = queue

        guard let library = device.makeDefaultLibrary() else { return nil }
        guard let vertexFn = library.makeFunction(name: "x68kVertex"),
              let fragmentFn = library.makeFunction(name: "x68kFragment") else { return nil }

        let pipelineDesc = MTLRenderPipelineDescriptor()
        pipelineDesc.vertexFunction = vertexFn
        pipelineDesc.fragmentFunction = fragmentFn
        pipelineDesc.colorAttachments[0].pixelFormat = .bgra8Unorm

        let vertexDesc = MTLVertexDescriptor()
        vertexDesc.attributes[0].format = .float2
        vertexDesc.attributes[0].offset = 0
        vertexDesc.attributes[0].bufferIndex = 0
        vertexDesc.attributes[1].format = .float2
        vertexDesc.attributes[1].offset = MemoryLayout<SIMD2<Float>>.stride
        vertexDesc.attributes[1].bufferIndex = 0
        vertexDesc.layouts[0].stride = MemoryLayout<Vertex>.stride
        vertexDesc.layouts[0].stepFunction = .perVertex
        pipelineDesc.vertexDescriptor = vertexDesc

        do {
            self.pipelineState = try device.makeRenderPipelineState(descriptor: pipelineDesc)
        } catch {
            print("Failed to create pipeline state: \(error)")
            return nil
        }

        let vertices: [Vertex] = [
            Vertex(position: SIMD2(-1,  1), texCoord: SIMD2(0, 0)),
            Vertex(position: SIMD2(-1, -1), texCoord: SIMD2(0, 1)),
            Vertex(position: SIMD2( 1,  1), texCoord: SIMD2(1, 0)),
            Vertex(position: SIMD2( 1, -1), texCoord: SIMD2(1, 1)),
        ]
        self.vertexBuffer = device.makeBuffer(bytes: vertices, length: vertices.count * MemoryLayout<Vertex>.stride, options: [])!

        // P199 — 表示フィルタ用の 2 サンプラを生成。端は clampToEdge で letterbox 境界の
        // にじみ/黒枠混入を防ぐ。生成失敗時は既存パターンどおり nil を返す。
        let nd = MTLSamplerDescriptor()
        nd.minFilter = .nearest; nd.magFilter = .nearest
        nd.sAddressMode = .clampToEdge; nd.tAddressMode = .clampToEdge
        guard let nearest = device.makeSamplerState(descriptor: nd) else { return nil }
        self.nearestSampler = nearest

        let ld = MTLSamplerDescriptor()
        ld.minFilter = .linear; ld.magFilter = .linear
        ld.sAddressMode = .clampToEdge; ld.tAddressMode = .clampToEdge
        guard let linear = device.makeSamplerState(descriptor: ld) else { return nil }
        self.linearSampler = linear

        super.init()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let drawable = view.currentDrawable,
              let renderPassDesc = view.currentRenderPassDescriptor else { return }

        updateTextureIfNeeded()
        // P595 (D-55): 表示ジオメトリの取得はテクスチャ再生成の可否とは**無関係に**
        // 毎フレーム行う。Phalanx 症状2(中央帯 → 上寄せ帯)のように、テクスチャ寸法
        // (256×128)が変わらずオフセットだけが変わるケースを取りこぼさないため。
        updateGeometry()

        guard let texture = texture else { return }

        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: renderPassDesc) else { return }

        encoder.setRenderPipelineState(pipelineState)
        // P212: 固定 4:3 面へフィット(P172 のレターボックスは維持・framebuffer 実解像度
        // アスペクト tw:th には依存しない)。MTKView は drawable 全体を黒 clearColor で
        // クリアするので余白は黒帯になり、viewport が全画面クワッド(UV 0-1)を中央・4:3 の
        // 矩形に閉じ込める = texture を viewport にフルストレッチ(横ドット数によらず 4:3 に
        // 伸長 → MMDSP 等の狭幅モードも標準テキストと同じ 4:3 面を占める)。
        // ★DisplayViewport.face/fitMetal がマウス写像(InputManager)と共有の単一情報源。
        // P595 (D-55): 4:3 面はそのまま維持しつつ、その面の中で Bridge が算出した
        // 表示ジオメトリ(標準ラスタでは恒等 = 従来とビット同一)の矩形へ閉じ込める。
        let vp = DisplayViewport.fitMetal(container: view.drawableSize, geom: currentGeom)
        if vp.w > 0 && vp.h > 0 {
            encoder.setViewport(MTLViewport(
                originX: Double(vp.x), originY: Double(vp.y),
                width: Double(vp.w), height: Double(vp.h), znear: 0, zfar: 1))
        }
        encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
        encoder.setFragmentTexture(texture, index: 0)
        // P199 — 表示フィルタを毎フレーム UserDefaults 直読み(@AppStorage と同一キー・
        // インメモリ辞書ルックアップゆえ 60fps でも無視できるコスト・reset 不要で即反映)。
        let smooth = (UserDefaults.standard.string(forKey: "displayFilter") ?? "smooth") != "sharp"
        encoder.setFragmentSamplerState(smooth ? linearSampler : nearestSampler, index: 0)
        // P664: 走査線エフェクト。displayFilterと同じ毎フレーム直読みパターン
        // (@AppStorageと同一キー、60fpsでも無視できるコスト)。
        var scanlineFlag: UInt32 = UserDefaults.standard.bool(forKey: "scanlineEffect") ? 1 : 0
        encoder.setFragmentBytes(&scanlineFlag, length: MemoryLayout<UInt32>.size, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
        commandBuffer.present(drawable)
        commandBuffer.commit()
    }

    /// P595 (D-55) — 公開フレームと同一スナップショットの表示ジオメトリを毎フレーム
    /// 取得する。`vm.displayGeometry` への公開は既存の `framebufferSize` と同じ
    /// メインスレッド dispatch パターンだが、**直前フレームと値が変わったときだけ**
    /// 行う(標準ラスタでは変化しないので通常フレームでは dispatch されない)。
    func updateGeometry() {
        var w: Int32 = 0
        var h: Int32 = 0
        var hs: Float = 1.0
        var vs: Float = 1.0
        var ox: Float = 0.0
        var oy: Float = 0.0
        guard mx68k_get_framebuffer_geom(&w, &h, &hs, &vs, &ox, &oy) != nil else { return }
        guard hs.isFinite, vs.isFinite, ox.isFinite, oy.isFinite, hs > 0, vs > 0 else { return }
        let newGeom = DisplayGeometry(hScale: CGFloat(hs), vScale: CGFloat(vs),
                                      offX: CGFloat(ox), offY: CGFloat(oy),
                                      geoMode: Int(mx68k_get_display_geo_mode()))
        guard newGeom != currentGeom else { return }
        currentGeom = newGeom
        let vm = viewModel
        DispatchQueue.main.async {
            vm?.displayGeometry = newGeom
            #if os(macOS)
            // P196/P595 — マウス写像用にメインスレッドで供給する(fbSize と同じ規約)。
            EmulatorMTKView.hostView?.displayGeom = newGeom
            #endif
        }
    }

    func updateTextureIfNeeded() {
        var w: Int32 = 0
        var h: Int32 = 0
        guard let ptr = mx68k_get_framebuffer(&w, &h) else {
            mx68k_log("[Swift] updateTextureIfNeeded: mx68k_get_framebuffer returned nil")
            return
        }
        let width = Int(w)
        let height = Int(h)

        if width <= 0 || height <= 0 {
            mx68k_log("[Swift] updateTextureIfNeeded: invalid size \(width)x\(height)")
            return
        }

        if texture == nil || textureWidth != width || textureHeight != height {
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm,
                width: width,
                height: height,
                mipmapped: false
            )
            desc.usage = .shaderRead
            desc.storageMode = .shared
            texture = device.makeTexture(descriptor: desc)
            textureWidth = width
            textureHeight = height
            mx68k_log("[Swift] updateTextureIfNeeded: created texture \(width)x\(height)")
            // P190 — サイズ変化時のみ publish(毎フレームは publish しない)。
            // draw は Metal スレッドなのでメインスレッドへ回す。
            let size = CGSize(width: width, height: height)
            let vm = viewModel
            DispatchQueue.main.async {
                vm?.framebufferSize = size
                #if os(macOS)
                // P196 — 絶対座標追従の写像用に fb 実寸をメインスレッドで供給する。
                // サイズ変化でゲスト座標系が変わるため原点合わせを要求。
                EmulatorMTKView.hostView?.fbSize = size
                InputManager.shared.requestMouseHoming()
                #endif
            }
        }

        guard let texture = texture else { return }
        let region = MTLRegionMake2D(0, 0, width, height)
        let bytesPerRow = width * 4
        texture.replace(region: region, mipmapLevel: 0, withBytes: ptr, bytesPerRow: bytesPerRow)
    }
}
