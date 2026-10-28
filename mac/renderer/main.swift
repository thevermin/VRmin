// QuestRenderer - etapa 2 de QuestVRBridge.
// Recibe la pose del Quest (vía server.py), renderiza la escena estéreo con Metal,
// la codifica en H.264 con el codificador por hardware del M2 (VideoToolbox)
// y devuelve cada cuadro al servidor para que lo mande al visor.
//
// Compilar:  xcrun swiftc -O renderer/main.swift -o renderer/QuestRenderer

import Foundation
import Security
import Metal
import CoreVideo
import CoreMedia
import VideoToolbox
import simd
import Accelerate
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
import CoreAudio
import AudioToolbox
import CoreText
import ScreenCaptureKit
import ApplicationServices
import Compression
import AppKit
import AVFoundation
import Vision
import CoreML
import CoreImage
import MetalFX

/// Idioma de los mensajes que se ven en el Quest: inglés (por defecto) o español (VRMIN_LANG=es, lo pasa VRmin Host)
let IDIOMA_ES = ProcessInfo.processInfo.environment["VRMIN_LANG"] == "es"
func tr(_ es: String, _ en: String) -> String { IDIOMA_ES ? es : en }

setvbuf(stdout, nil, _IOLBF, 0)
signal(SIGPIPE, SIG_IGN)

// MARK: - Configuración (se puede cambiar con variables de entorno)
let env = ProcessInfo.processInfo.environment
let EYE = Int(env["VR_EYE"] ?? "") ?? 1024          // resolución por ojo (cuadrada)
// Foveated encoding (VR_FOV = "nivel,centro,cx izq,cx der,cy"; nivel 0 = apagado): antes de codificar, el borde de
// cada ojo se achica y el centro queda igual; el Quest lo vuelve a estirar. Se codifica una imagen más chica:
// menos demora y más calidad en el centro con los mismos Mbps. El juego dibuja igual (EYE por ojo).
struct Fovea {
    var level = 0, size = 1
    var cx: [Double] = [0.5, 0.5], cy = 0.5
    init(_ s: String) {
        let p = s.split(separator: ",").map { Double($0.trimmingCharacters(in: .whitespaces)) ?? 0 }
        if p.count >= 1 { level = max(0, min(3, Int(p[0]))) }
        if p.count >= 2 { size = max(0, min(2, Int(p[1]))) }
        if p.count >= 5 { cx = [min(0.8, max(0.2, p[2])), min(0.8, max(0.2, p[3]))]; cy = min(0.8, max(0.2, p[4])) }
    }
    var on: Bool { level > 0 }
    var text: String { String(format: "%d,%d,%.4f,%.4f,%.4f", level, size, cx[0], cx[1], cy) }
    // La misma cuenta que en el Quest: densidad 1 en el centro, que baja suave hasta «m» en el borde
    var params: (m: Double, r0: Double, r1: Double) {
        let m = [1, 0.62, 0.48, 0.36][level]
        let r = [(0.10, 0.30), (0.16, 0.38), (0.24, 0.46)][size]
        return (m, r.0, r.1)
    }
    /// E(s): cuántos píxeles codificados hay desde 0 hasta s (2049 valores, sin normalizar)
    func cumulative(_ c: Double) -> [Double] {
        let (m, r0, r1) = params, n = 2048
        var out = [Double](repeating: 0, count: n + 1), acc = 0.0
        for i in 0..<n {
            let t = (Double(i) + 0.5) / Double(n), d = abs(t - c)
            let x = max(0, min(1, (d - r0) / (r1 - r0))), sm = x * x * (3 - 2 * x)
            acc += (1 - (1 - m) * sm) / Double(n)
            out[i + 1] = acc
        }
        return out
    }
    func ratio(_ c: Double) -> Double { cumulative(c).last ?? 1 }
    /// Del codificado (0..1) a la imagen del juego (0..1): 1024 valores
    func forward(_ c: Double) -> [Float] {
        let E = cumulative(c), tot = E.last ?? 1, n = E.count - 1
        var out = [Float](repeating: 0, count: 1024), j = 0
        for i in 0..<1024 {
            let e = Double(i) / 1023 * tot
            while j < n - 1 && E[j + 1] < e { j += 1 }
            let a = E[j], b = E[j + 1], k = b > a ? (e - a) / (b - a) : 0
            out[i] = Float((Double(j) + k) / Double(n))
        }
        return out
    }
}
let FOVEA = Fovea(env["VR_FOV"] ?? "0")
/// (de a 64 píxeles: los tamaños «redondos» son los que el codificador por hardware acepta siempre)
func round16(_ v: Double) -> Int { max(256, Int((v / 64).rounded()) * 64) }
let SW = EYE * 2, SH = EYE                            // lo que dibuja el juego (los dos ojos, lado a lado)
let W = FOVEA.on ? round16(Double(EYE) * FOVEA.ratio(0.5)) * 2 : SW   // lo que se codifica y se manda
let H = FOVEA.on ? round16(Double(EYE) * FOVEA.ratio(FOVEA.cy)) : SH

/// Achica el borde de cada ojo (foveated encoding) en la GPU: de la imagen completa (con mipmaps, para que lo
/// achicado no titile) a la imagen codificada
final class Foveator {
    /// Un panel flotante del juego (menú, HUD) tal como lo manda el runtime: su imagen y sus 4 esquinas en cada ojo
    /// (posiciones de recorte: abajo-izq., abajo-der., arriba-izq., arriba-der.). flags: 1 mezcla, 2 premult., 4 dado vuelta, 8 RGBA
    struct GameQuad { let w: Int, h: Int, flags: UInt32, clip: [[SIMD4<Float>]], pixels: UnsafeRawPointer, offset: Int }   // (offset: en la memoria compartida)
    struct QuadVertex { var pos: SIMD4<Float>; var uv: SIMD2<Float>; var pad: SIMD2<Float> }
    /// (siempre: también agranda con MetalFX la imagen del juego dibujada más chica y la ubica si se recortó el campo de visión)
    static let shared: Foveator? = Foveator()
    let dev: MTLDevice, queue: MTLCommandQueue, pipe: MTLRenderPipelineState, copyPipe: MTLRenderPipelineState, quadPipe: MTLRenderPipelineState, lut: MTLTexture
    let cache: CVMetalTextureCache
    private var src: MTLTexture?
    init?() {
        guard let d = MTLCreateSystemDefaultDevice(), let q = d.makeCommandQueue() else { return nil }
        dev = d; queue = q
        let code = """
        #include <metal_stdlib>
        using namespace metal;
        struct FO { float4 pos [[position]]; };
        vertex FO fvert(uint vid [[vertex_id]]) {
            float2 p = float2(float((vid << 1) & 2), float(vid & 2));
            FO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o;
        }
        static float lutAt(texture2d<float, access::read> l, float e, uint row, thread float &d) {
            float f = clamp(e, 0.0, 1.0) * 1023.0; uint i = min(uint(f), 1022u); float t = f - float(i);
            float a = l.read(uint2(i, row)).r, b = l.read(uint2(i + 1, row)).r;
            d = (b - a) * 1023.0; return mix(a, b, t);
        }
        // copiar una imagen a una parte de otra (el tamaño lo da el viewport)
        struct FC { float4 pos [[position]]; float2 uv; };
        vertex FC cvert(uint vid [[vertex_id]]) {
            float2 p = float2(float((vid << 1) & 2), float(vid & 2));
            FC o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.uv = float2(p.x, 1.0 - p.y); return o;
        }
        fragment float4 fcopy(FC in [[stage_in]], texture2d<float> src [[texture(0)]]) {
            constexpr sampler smp(filter::linear, address::clamp_to_edge);
            return src.sample(smp, in.uv);
        }
        // panel flotante del juego (menú, HUD): 4 esquinas ya proyectadas en el ojo; fl: 1 mezcla, 2 premultiplicado
        struct QV { float4 pos; float2 uv; float2 pad; };
        vertex FC qvert(uint vid [[vertex_id]], constant QV *v [[buffer(0)]]) {
            FC o; o.pos = v[vid].pos; o.uv = v[vid].uv; return o;
        }
        fragment float4 fquad(FC in [[stage_in]], texture2d<float> src [[texture(0)]], constant uint &fl [[buffer(0)]]) {
            constexpr sampler smp(filter::linear, address::clamp_to_edge);
            float4 c = src.sample(smp, in.uv);
            if ((fl & 1u) == 0u) return float4(c.rgb, 1.0);
            if ((fl & 2u) == 0u) c.rgb *= c.a;
            return c;
        }
        // p: ancho y alto codificados de un ojo, ancho y alto de un ojo en la imagen del juego
        fragment float4 ffov(FO in [[stage_in]], texture2d<float> src [[texture(0)]], texture2d<float, access::read> lut [[texture(1)]],
                             constant float4 &p [[buffer(0)]]) {
            float2 g = in.pos.xy;
            uint eye = g.x >= p.x ? 1u : 0u;
            float ex = (g.x - float(eye) * p.x) / p.x, ey = g.y / p.y;
            float dx, dy;
            float sx = lutAt(lut, ex, eye, dx), sy = lutAt(lut, ey, 2u, dy);
            float lod = max(log2(max(1.0, dx * p.z / p.x)), log2(max(1.0, dy * p.w / p.y)));
            constexpr sampler smp(filter::linear, mip_filter::linear, address::clamp_to_edge);
            float hx = 0.5 / p.z;
            float u = (float(eye) + clamp(sx, hx, 1.0 - hx)) * 0.5;
            return src.sample(smp, float2(u, sy), level(lod));
        }
        """
        do {
            let lib = try d.makeLibrary(source: code, options: nil)
            let pd = MTLRenderPipelineDescriptor()
            pd.vertexFunction = lib.makeFunction(name: "fvert")
            pd.fragmentFunction = lib.makeFunction(name: "ffov")
            pd.colorAttachments[0].pixelFormat = .bgra8Unorm
            pipe = try d.makeRenderPipelineState(descriptor: pd)
            let cd = MTLRenderPipelineDescriptor()
            cd.vertexFunction = lib.makeFunction(name: "cvert")
            cd.fragmentFunction = lib.makeFunction(name: "fcopy")
            cd.colorAttachments[0].pixelFormat = .bgra8Unorm
            copyPipe = try d.makeRenderPipelineState(descriptor: cd)
            // (los paneles flotantes del juego, encima: alfa premultiplicado)
            let qd = MTLRenderPipelineDescriptor()
            qd.vertexFunction = lib.makeFunction(name: "qvert")
            qd.fragmentFunction = lib.makeFunction(name: "fquad")
            qd.colorAttachments[0].pixelFormat = .bgra8Unorm
            qd.colorAttachments[0].isBlendingEnabled = true
            qd.colorAttachments[0].sourceRGBBlendFactor = .one; qd.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
            qd.colorAttachments[0].sourceAlphaBlendFactor = .one; qd.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
            quadPipe = try d.makeRenderPipelineState(descriptor: qd)
        } catch { print("ERROR (foveated encoding): \(error)"); return nil }
        let ld = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: 1024, height: 3, mipmapped: false)
        ld.usage = .shaderRead
        guard let l = d.makeTexture(descriptor: ld) else { return nil }
        for (row, c) in [FOVEA.cx[0], FOVEA.cx[1], FOVEA.cy].enumerated() {
            // (sin foveated encoding, la tabla no cambia nada: cada punto va a su mismo lugar)
            let v = FOVEA.on ? FOVEA.forward(c) : (0..<1024).map { Float($0) / 1023 }
            v.withUnsafeBytes { l.replace(region: MTLRegionMake2D(0, row, 1024, 1), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 4096) }
        }
        lut = l
        var tc: CVMetalTextureCache?
        CVMetalTextureCacheCreate(nil, nil, d, nil, &tc)
        guard let t = tc else { return nil }
        cache = t
        if FOVEA.on { print("[juego] Foveated encoding: nivel \(FOVEA.level), centro \(FOVEA.size) · se codifica \(W)x\(H) en lugar de \(SW)x\(SH)") }
    }
    /// La imagen completa (se rehace si cambia el tamaño o el formato)
    func source(_ w: Int, _ h: Int, _ fmt: MTLPixelFormat) -> MTLTexture? {
        if let s = src, s.width == w, s.height == h, s.pixelFormat == fmt { return s }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: fmt, width: w, height: h, mipmapped: true)
        td.usage = [.shaderRead, .renderTarget]
        td.storageMode = .private   // (solo la usa la GPU: privada es más rápida que compartida)
        src = dev.makeTexture(descriptor: td)
        return src
    }
    /// De la imagen completa a la codificada (en el mismo buffer de comandos)
    func warp(_ cb: MTLCommandBuffer, _ s: MTLTexture, into dst: MTLTexture) {
        if let b = cb.makeBlitCommandEncoder() { b.generateMipmaps(for: s); b.endEncoding() }
        let rp = MTLRenderPassDescriptor()
        rp.colorAttachments[0].texture = dst
        rp.colorAttachments[0].loadAction = .dontCare
        rp.colorAttachments[0].storeAction = .store
        guard let e = cb.makeRenderCommandEncoder(descriptor: rp) else { return }
        e.setRenderPipelineState(pipe)
        e.setFragmentTexture(s, index: 0)
        e.setFragmentTexture(lut, index: 1)
        var p = SIMD4<Float>(Float(W / 2), Float(H), Float(s.width / 2), Float(s.height))
        e.setFragmentBytes(&p, length: 16, index: 0)
        e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        e.endEncoding()
    }
    // Cada ojo del juego, tal como llega, y (si hay que agrandarlo) lo que sale de MetalFX
    private var eyeIn: [MTLTexture?] = [nil, nil], eyeUp: [MTLTexture?] = [nil, nil]
    private var scalers: [String: MTLFXSpatialScaler] = [:]
    private var loggedUp = ""
    private func tex(_ w: Int, _ h: Int, _ fmt: MTLPixelFormat, _ usage: MTLTextureUsage, _ mode: MTLStorageMode, reuse: MTLTexture?) -> MTLTexture? {
        if let t = reuse, t.width == w, t.height == h, t.pixelFormat == fmt, t.storageMode == mode { return t }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: fmt, width: w, height: h, mipmapped: false)
        td.usage = usage; td.storageMode = mode
        return dev.makeTexture(descriptor: td)
    }
    private func scaler(_ iw: Int, _ ih: Int, _ ow: Int, _ oh: Int, _ fmt: MTLPixelFormat) -> MTLFXSpatialScaler? {
        let k = "\(iw)x\(ih)>\(ow)x\(oh)/\(fmt.rawValue)"
        if let s = scalers[k] { return s }
        let d = MTLFXSpatialScalerDescriptor()
        d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = ow; d.outputHeight = oh
        d.colorTextureFormat = fmt; d.outputTextureFormat = fmt
        d.colorProcessingMode = .perceptual
        guard MTLFXSpatialScalerDescriptor.supportsDevice(dev), let s = d.makeSpatialScaler(device: dev) else { return nil }
        if scalers.count > 6 { scalers.removeAll() }
        scalers[k] = s
        return s
    }

    /// Cuadro del juego (en memoria): lo sube a la GPU; si el juego lo dibujó más chico (escala de render) lo agranda con
    /// MetalFX; si se recortó el campo de visión lo ubica en su lugar (con el borde negro); y le achica el borde
    /// (foveated encoding, si está activado). Lo deja en pb, listo para codificar.
    /// rects: dónde va cada ojo dentro de la imagen completa de un ojo (x, y, ancho, alto, en 0..1)
    /// quads: paneles flotantes del juego (menú, HUD) que se dibujan encima, ya a la resolución del visor
    /// gpu: la memoria compartida vista por la GPU y dónde empieza el cuadro: la imagen se copia en la GPU directo desde
    /// ahí (sin pasar por el procesador: menos espera). Sin ella, se copia con el procesador.
    func warpPixels(_ ptr: UnsafeRawPointer, ew: Int, eh: Int, isRGBA: Bool, rects: [SIMD4<Float>], into pb: CVPixelBuffer,
                    quads: [GameQuad] = [], gpu: (MTLBuffer, Int)? = nil) -> Bool {
        let fmt: MTLPixelFormat = isRGBA ? .rgba8Unorm : .bgra8Unorm
        let mode: MTLStorageMode = gpu != nil ? .private : .shared
        var qt: [(MTLTexture, GameQuad)] = []
        for (i, q) in quads.enumerated() where i < 8 {
            let qf: MTLPixelFormat = q.flags & 8 != 0 ? .rgba8Unorm : .bgra8Unorm
            guard let tx = tex(q.w, q.h, qf, [.shaderRead], mode, reuse: quadTex[i]) else { continue }
            quadTex[i] = tx
            if gpu == nil { tx.replace(region: MTLRegionMake2D(0, 0, q.w, q.h), mipmapLevel: 0, withBytes: q.pixels, bytesPerRow: q.w * 4) }
            qt.append((tx, q))
        }
        return warpCore(ew: ew, eh: eh, store: fmt, view: fmt, inMode: mode, rects: rects, into: pb, quads: qt) { cb, e, inT in
            guard let g = gpu else {
                inT.replace(region: MTLRegionMake2D(0, 0, ew, eh), mipmapLevel: 0, withBytes: ptr + e * ew * 4, bytesPerRow: ew * 2 * 4)
                return
            }
            let (buf, off) = g
            guard let b = cb.makeBlitCommandEncoder() else { return }
            b.copy(from: buf, sourceOffset: off + e * ew * 4, sourceBytesPerRow: ew * 2 * 4, sourceBytesPerImage: ew * 2 * 4 * eh,
                   sourceSize: MTLSize(width: ew, height: eh, depth: 1), to: inT, destinationSlice: 0, destinationLevel: 0,
                   destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
            if e == 0 {   // (los paneles, una vez)
                for (tx, q) in qt {
                    b.copy(from: buf, sourceOffset: q.offset, sourceBytesPerRow: q.w * 4, sourceBytesPerImage: q.w * 4 * q.h,
                           sourceSize: MTLSize(width: q.w, height: q.h, depth: 1), to: tx, destinationSlice: 0, destinationLevel: 0,
                           destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
                }
            }
            b.endEncoding()
        }
    }
    private var quadTex: [MTLTexture?] = Array(repeating: nil, count: 8)
    /// Lo mismo, con el cuadro ya en la GPU (copia directa: una textura compartida con el juego, los dos ojos lado a lado)
    func warpTexture(_ src: MTLTexture, ew: Int, eh: Int, rects: [SIMD4<Float>], into pb: CVPixelBuffer) -> Bool {
        // (se lee como colores sin convertir, igual que los píxeles por memoria)
        let view: MTLPixelFormat = src.pixelFormat == .rgba8Unorm_srgb ? .rgba8Unorm : src.pixelFormat == .bgra8Unorm_srgb ? .bgra8Unorm : src.pixelFormat
        return warpCore(ew: ew, eh: eh, store: src.pixelFormat, view: view, inMode: .private, rects: rects, into: pb) { cb, e, inT in
            guard let b = cb.makeBlitCommandEncoder() else { return }
            b.copy(from: src, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: e * ew, y: 0, z: 0), sourceSize: MTLSize(width: ew, height: eh, depth: 1),
                   to: inT, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
            b.endEncoding()
        }
    }
    private var eyeView: [MTLTexture?] = [nil, nil]
    /// store: formato de la imagen de cada ojo · view: cómo se lee (sin pasar de sRGB a lineal) · fill: la llena (en cb)
    /// inMode: memoria de la imagen de cada ojo (compartida si la llena la CPU; privada si la llena la GPU)
    private func warpCore(ew: Int, eh: Int, store: MTLPixelFormat, view: MTLPixelFormat, inMode: MTLStorageMode, rects: [SIMD4<Float>], into pb: CVPixelBuffer,
                          quads: [(MTLTexture, GameQuad)] = [], fill: (MTLCommandBuffer, Int, MTLTexture) -> Void) -> Bool {
        let fmt = view
        guard let full = source(SW, SH, .bgra8Unorm) else { fail("no se pudo crear la imagen completa"); return false }
        var cvt: CVMetalTexture?
        let cr = CVMetalTextureCacheCreateTextureFromImage(nil, cache, pb, nil, .bgra8Unorm, W, H, 0, &cvt)
        guard let c = cvt, let dst = CVMetalTextureGetTexture(c) else { fail("la imagen a codificar no se pudo usar en la GPU (\(cr))"); return false }
        guard let cb = queue.makeCommandBuffer() else { fail("sin buffer de comandos"); return false }
        var draw: [(MTLTexture, MTLViewport)] = []
        for e in 0..<2 {
            let r = rects[e]
            let tx = Int((r.x * Float(EYE)).rounded()), ty = Int((r.y * Float(SH)).rounded())
            let tw = max(16, Int((r.z * Float(EYE)).rounded())), th = max(16, Int((r.w * Float(SH)).rounded()))
            guard let inStore = tex(ew, eh, store, store == view ? [.shaderRead] : [.shaderRead, .pixelFormatView], inMode, reuse: eyeIn[e]) else { fail("no se pudo crear la imagen del ojo"); return false }
            eyeIn[e] = inStore
            fill(cb, e, inStore)
            if store != view && (eyeView[e] == nil || eyeView[e]?.parent !== inStore) { eyeView[e] = inStore.makeTextureView(pixelFormat: view) }
            guard let inT = store == view ? inStore : eyeView[e] else { fail("no se pudo leer la imagen del ojo"); return false }
            var use: MTLTexture = inT
            // Más chico de lo que va en pantalla: MetalFX lo agranda (mejor que estirarlo)
            if tw > ew + 8 || th > eh + 8, let sc = scaler(ew, eh, tw, th, fmt),
               let up = tex(tw, th, fmt, sc.outputTextureUsage.union([.shaderRead, .renderTarget]), .private, reuse: eyeUp[e]) {
                eyeUp[e] = up
                sc.colorTexture = inT
                sc.outputTexture = up
                sc.inputContentWidth = ew; sc.inputContentHeight = eh
                sc.encode(commandBuffer: cb)
                use = up
                let msg = "\(ew)x\(eh) → \(tw)x\(th)"
                if msg != loggedUp { loggedUp = msg; print("[juego] Escala de render: el juego dibuja \(ew)x\(eh) por ojo y MetalFX lo agranda a \(tw)x\(th)") }
            }
            draw.append((use, MTLViewport(originX: Double(e * EYE + tx), originY: Double(ty), width: Double(tw), height: Double(th), znear: 0, zfar: 1)))
        }
        // La imagen completa de los dos ojos (negro donde el campo de visión se recortó)
        let rp = MTLRenderPassDescriptor()
        rp.colorAttachments[0].texture = full
        rp.colorAttachments[0].loadAction = .clear
        rp.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        rp.colorAttachments[0].storeAction = .store
        if let enc = cb.makeRenderCommandEncoder(descriptor: rp) {
            enc.setRenderPipelineState(copyPipe)
            for (t, vp) in draw {
                enc.setViewport(vp)
                enc.setFragmentTexture(t, index: 0)
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            }
            if !quads.isEmpty {   // los paneles flotantes, proyectados en cada ojo (en su rectángulo)
                enc.setRenderPipelineState(quadPipe)
                for (e, (_, vp)) in draw.enumerated() {
                    enc.setViewport(vp)
                    for (tx, q) in quads {
                        let c = q.clip[e]
                        guard c.allSatisfy({ $0.w > 1e-3 }) else { continue }   // (alguna esquina detrás del ojo)
                        let v0: Float = q.flags & 4 != 0 ? 0 : 1, v1: Float = 1 - v0
                        var vs: [QuadVertex] = [QuadVertex(pos: c[0], uv: SIMD2(0, v0), pad: .zero), QuadVertex(pos: c[1], uv: SIMD2(1, v0), pad: .zero),
                                                QuadVertex(pos: c[2], uv: SIMD2(0, v1), pad: .zero), QuadVertex(pos: c[3], uv: SIMD2(1, v1), pad: .zero)]
                        var fl = q.flags
                        enc.setVertexBytes(&vs, length: MemoryLayout<QuadVertex>.stride * 4, index: 0)
                        enc.setFragmentBytes(&fl, length: 4, index: 0)
                        enc.setFragmentTexture(tx, index: 0)
                        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    }
                }
            }
            enc.endEncoding()
        }
        warp(cb, full, into: dst)
        cb.commit()
        cb.waitUntilCompleted()
        withExtendedLifetime(c) { }
        if cb.status == .error { fail("la GPU dio un error: \(cb.error.map { "\($0)" } ?? "?")"); return false }
        okFrames += 1
        if okFrames == 1 { print("[juego] Imagen del juego en la GPU: \(ew)x\(eh) por ojo → \(W)x\(H) para el visor") }
        return true
    }
    private var okFrames = 0, failCount = 0, lastFail = ""
    private func fail(_ why: String) {
        failCount += 1
        if why != lastFail || failCount % 300 == 1 { lastFail = why; print("[juego] ERROR armando la imagen del juego: \(why) (\(failCount) veces)") }
    }
}
/// Copia directa: abre las texturas que el juego comparte (DXMT las registra en el sistema con un nombre)
enum GpuShare {
    private typealias LookUp = @convention(c) (mach_port_t, UnsafePointer<CChar>, UnsafeMutablePointer<mach_port_t>) -> kern_return_t
    private typealias AllocFn = @convention(c) (AnyClass, Selector) -> UnsafeMutableRawPointer?
    private typealias InitFn = @convention(c) (UnsafeMutableRawPointer, Selector, mach_port_t) -> UnsafeMutableRawPointer?
    static func open(_ name: String, device: MTLDevice) -> MTLTexture? {
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "bootstrap_look_up") else { print("[juego] Copia directa: no está bootstrap_look_up"); return nil }
        var bp: mach_port_t = 0
        guard task_get_special_port(mach_task_self_, TASK_BOOTSTRAP_PORT, &bp) == KERN_SUCCESS else { return nil }
        var port: mach_port_t = 0
        let kr = name.withCString { unsafeBitCast(sym, to: LookUp.self)(bp, $0, &port) }
        guard kr == KERN_SUCCESS, port != 0 else { print("[juego] Copia directa: no encontré «\(name)» (\(kr))"); return nil }
        guard let cls = NSClassFromString("MTLSharedTextureHandle"),
              let am = class_getClassMethod(cls, NSSelectorFromString("alloc")),
              let im = class_getInstanceMethod(cls, NSSelectorFromString("initWithMachPort:")) else { print("[juego] Copia directa: esta versión de macOS no permite abrir la textura"); return nil }
        guard let raw = unsafeBitCast(method_getImplementation(am), to: AllocFn.self)(cls, NSSelectorFromString("alloc")),
              let h = unsafeBitCast(method_getImplementation(im), to: InitFn.self)(raw, NSSelectorFromString("initWithMachPort:"), port) else { return nil }
        let handle = Unmanaged<MTLSharedTextureHandle>.fromOpaque(h).takeRetainedValue()
        return device.makeSharedTexture(handle: handle)
    }
}
let FPS = Int(env["VR_FPS"] ?? "") ?? 72
let BITRATE = (Int(env["VR_MBPS"] ?? "") ?? 25) * 1_000_000
/// Códec del video de los juegos: HEVC (H.265, por defecto) o H.264 (VR_CODEC=h264). Si la Mac no puede HEVC, se usa H.264.
var CODEC_HEVC = (env["VR_CODEC"] ?? "hevc") != "h264"
/// Los ajustes generales de los juegos (los de «Todos los juegos»). Los que corren ahora (VR_EYE…) pueden ser los de un juego.
let GEN_EYE = Int(env["VR_GEN_EYE"] ?? "") ?? Int(env["VR_EYE"] ?? "") ?? 1024
let GEN_FPS = Int(env["VR_GEN_FPS"] ?? "") ?? Int(env["VR_FPS"] ?? "") ?? 72
let GEN_MBPS = Int(env["VR_GEN_MBPS"] ?? "") ?? Int(env["VR_MBPS"] ?? "") ?? 25
let GEN_FOV = env["VR_GEN_FOV"] ?? env["VR_FOV"] ?? "0"
let EXTRA_CUBES = Int(env["VR_CUBES"] ?? "") ?? 0    // carga extra para la GPU (pruebas)
// Escritorio y ventanas (se cambian en vivo desde Ajustes en el Quest)
var DESK_MAX_W = 2560, DESK_FPS = 60, DESK_MBPS = 20
var DESK_AUDIO = 0   // sonido de la Mac fuera de los juegos: 0 solo en la Mac, 1 solo en el visor, 2 en los dos
let SERVER_PORT = UInt16(env["RENDER_PORT"] ?? "") ?? 9000
let GAME_PORT = UInt16(env["GAME_PORT"] ?? "") ?? 9002

// MARK: - Estado compartido
struct EyeView { var proj: simd_float4x4; var view: simd_float4x4 }
struct Pose { var id: UInt32; var eyes: [EyeView] }

final class Shared {
    private let lock = NSLock()
    private var _pose: Pose?
    private var _forceKey = true
    private var _inFlight = 0

    var pose: Pose? { lock.lock(); defer { lock.unlock() }; return _pose }
    func setPose(_ p: Pose) { lock.lock(); _pose = p; lock.unlock() }
    func requestKey() { lock.lock(); _forceKey = true; lock.unlock() }
    func takeKey() -> Bool { lock.lock(); defer { lock.unlock() }; let k = _forceKey; _forceKey = false; return k }
    func beginFrame() -> Bool {
        lock.lock(); defer { lock.unlock() }
        if _inFlight >= 2 { return false }
        _inFlight += 1
        return true
    }
    func endFrame() { lock.lock(); _inFlight = max(0, _inFlight - 1); lock.unlock() }
    var inFlightCount: Int { lock.lock(); defer { lock.unlock() }; return _inFlight }

    private var _frameNo: Int64 = 0
    func nextFrameNo() -> Int64 { lock.lock(); defer { lock.unlock() }; _frameNo += 1; return _frameNo }

    private var _xr: [Float] = []
    var xr: [Float] { lock.lock(); defer { lock.unlock() }; return _xr }
    func setXR(_ a: [Float]) { lock.lock(); _xr = a; lock.unlock() }

    private var _lastGameFrame: UInt64 = 0
    func markGameFrame() { lock.lock(); _lastGameFrame = DispatchTime.now().uptimeNanoseconds; lock.unlock() }
    /// true si el juego mandó un cuadro en el último medio segundo
    var gameActive: Bool {
        lock.lock(); defer { lock.unlock() }
        return _lastGameFrame != 0 && DispatchTime.now().uptimeNanoseconds - _lastGameFrame < 500_000_000
    }
}

func mat4(_ a: [Float]) -> simd_float4x4? {
    guard a.count == 16 else { return nil }
    return simd_float4x4(columns: (
        SIMD4<Float>(a[0], a[1], a[2], a[3]),
        SIMD4<Float>(a[4], a[5], a[6], a[7]),
        SIMD4<Float>(a[8], a[9], a[10], a[11]),
        SIMD4<Float>(a[12], a[13], a[14], a[15])))
}

func trs(_ x: Float, _ y: Float, _ z: Float, _ sx: Float, _ sy: Float, _ sz: Float) -> simd_float4x4 {
    simd_float4x4(columns: (
        SIMD4<Float>(sx, 0, 0, 0), SIMD4<Float>(0, sy, 0, 0),
        SIMD4<Float>(0, 0, sz, 0), SIMD4<Float>(x, y, z, 1)))
}

func rotY(_ a: Float) -> simd_float4x4 {
    let c = cos(a), s = sin(a)
    return simd_float4x4(columns: (
        SIMD4<Float>(c, 0, -s, 0), SIMD4<Float>(0, 1, 0, 0),
        SIMD4<Float>(s, 0, c, 0), SIMD4<Float>(0, 0, 0, 1)))
}

func rotX(_ a: Float) -> simd_float4x4 {
    let c = cos(a), s = sin(a)
    return simd_float4x4(columns: (
        SIMD4<Float>(1, 0, 0, 0), SIMD4<Float>(0, c, s, 0),
        SIMD4<Float>(0, -s, c, 0), SIMD4<Float>(0, 0, 0, 1)))
}

// WebXR usa profundidad de -1 a 1 (OpenGL); Metal usa 0 a 1.
let zFix = simd_float4x4(columns: (
    SIMD4<Float>(1, 0, 0, 0), SIMD4<Float>(0, 1, 0, 0),
    SIMD4<Float>(0, 0, 0.5, 0), SIMD4<Float>(0, 0, 0.5, 1)))

// MARK: - Conexión con server.py (TCP local)
final class Link {
    private var fd: Int32 = -1
    private let wlock = NSLock()

    var isConnected: Bool { fd >= 0 }

    func open(port: UInt16) -> Bool {
        let s = socket(AF_INET, SOCK_STREAM, 0)
        if s < 0 { return false }
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        let r = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if r != 0 { Darwin.close(s); return false }
        var one: Int32 = 1
        setsockopt(s, Int32(IPPROTO_TCP), TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
        wlock.lock(); fd = s; wlock.unlock()
        return true
    }

    /// También se manda a la app nativa del Quest (si está conectada)
    static var mirror: ((Data) -> Void)?

    func send(_ payload: Data) {
        Link.mirror?(payload)
        wlock.lock(); defer { wlock.unlock() }
        guard fd >= 0 else { return }
        var len = UInt32(payload.count).bigEndian
        var buf = Data(bytes: &len, count: 4)
        buf.append(payload)
        buf.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            var off = 0
            while off < raw.count {
                let n = Darwin.write(fd, raw.baseAddress! + off, raw.count - off)
                if n <= 0 { break }
                off += n
            }
        }
    }

    /// Lee líneas (JSON) hasta que se corta la conexión.
    func readLines(_ onLine: (Data) -> Void) {
        var pending = Data()
        var chunk = [UInt8](repeating: 0, count: 65536)
        while true {
            let n = chunk.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress!, $0.count) }
            if n <= 0 { break }
            pending.append(contentsOf: chunk[0..<n])
            while let i = pending.firstIndex(of: 0x0A) {
                let line = pending.subdata(in: pending.startIndex..<i)
                pending.removeSubrange(pending.startIndex...i)
                if !line.isEmpty { onLine(line) }
            }
        }
        wlock.lock(); Darwin.close(fd); fd = -1; wlock.unlock()
    }
}

// MARK: - Codificador H.264 por hardware
final class Encoder {
    private var session: VTCompressionSession!
    private let link: Link
    private let shared: Shared

    init?(link: Link, shared: Shared) {
        self.link = link
        self.shared = shared
        if !makeSession(lowLatency: true) { print("ERROR: no se pudo crear el codificador de video"); return nil }
    }

    /// Si el codificador descarta todos los cuadros (le pasa a algunos tamaños en el modo de baja latencia), se arma de
    /// nuevo sin ese modo. Cuenta lo que sale para el registro.
    private let statLock = NSLock()
    private var outs = 0, drops = 0, errs = 0, inARow = 0, rebuilt = 0, statAt = DispatchTime.now().uptimeNanoseconds
    private func note(ok: Bool, dropped: Bool) {
        statLock.lock()
        if ok { outs += 1; inARow = 0 } else { if dropped { drops += 1 } else { errs += 1 }; inARow += 1 }
        let bad = inARow >= 20 && rebuilt < 2
        let now = DispatchTime.now().uptimeNanoseconds
        let report = now - statAt > 5_000_000_000 && (drops + errs > 0)
        let o = outs, d = drops, e = errs
        if report { statAt = now; outs = 0; drops = 0; errs = 0 }
        if bad { inARow = 0; rebuilt += 1 }
        let r = rebuilt
        statLock.unlock()
        if report { print("[mac] Codificador \(W)x\(H): \(o) cuadros codificados, \(d) descartados, \(e) con error (últimos 5 s)") }
        if bad {
            print("[mac] ERROR: el codificador descartó 20 cuadros seguidos a \(W)x\(H). Lo armo de nuevo\(r == 1 ? " sin el modo de baja latencia" : "")…")
            DispatchQueue.global().async { [self] in
                if let s = session { VTCompressionSessionInvalidate(s) }
                session = nil
                _ = makeSession(lowLatency: false)
                shared.requestKey()
            }
        }
    }

    @discardableResult
    private func makeSession(lowLatency ll: Bool) -> Bool {
        let lowLatency = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true] as CFDictionary
        // HEVC primero (si se eligió); si la Mac no puede, H.264
        for hevc in (CODEC_HEVC ? [true, false] : [false]) {
            for spec in (ll ? [lowLatency, nil] : [nil]) as [CFDictionary?] {
                var s: VTCompressionSession?
                let st = VTCompressionSessionCreate(
                    allocator: nil, width: Int32(W), height: Int32(H),
                    codecType: hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264, encoderSpecification: spec,
                    imageBufferAttributes: nil, compressedDataAllocator: nil,
                    outputCallback: nil, refcon: nil, compressionSessionOut: &s)
                if st == noErr, let s = s {
                    session = s
                    CODEC_HEVC = hevc
                    print("Codificador: \(hevc ? "HEVC" : "H.264") por hardware" + (spec != nil ? ", modo baja latencia" : " (sin modo baja latencia)"))
                    break
                }
            }
            if session != nil { break }
        }
        if session == nil { return false }

        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        if CODEC_HEVC {
            set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel, quiet: true)
        } else if !set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_ConstrainedHigh_AutoLevel, quiet: true) {
            set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel, quiet: true)
        }
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        // Menos espera por cuadro: velocidad antes que calidad (la calidad la da la tasa de bits) y sin modo de ahorro
        set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue, quiet: true)
        set(kVTCompressionPropertyKey_MaximizePowerEfficiency, kCFBooleanFalse, quiet: true)
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: BITRATE))
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: FPS))
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: FPS * 2))
        VTCompressionSessionPrepareToEncodeFrames(session)
        return true
    }

    @discardableResult
    private func set(_ key: CFString, _ value: CFTypeRef, quiet: Bool = false) -> Bool {
        let r = VTSessionSetProperty(session, key: key, value: value)
        if r != noErr && !quiet { print("Aviso: el codificador no aceptó \(key) (\(r))") }
        return r == noErr
    }

    func encode(_ pb: CVPixelBuffer, frameNo: Int64, poseId: UInt32, forceKey: Bool, start: UInt64) {
        let props: CFDictionary? = forceKey
            ? [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary : nil
        let pts = CMTime(value: frameNo, timescale: CMTimeScale(FPS))
        let link = self.link, shared = self.shared
        let encT = DispatchTime.now().uptimeNanoseconds
        guard let session else { shared.endFrame(); return }
        let st = VTCompressionSessionEncodeFrame(
            session, imageBuffer: pb, presentationTimeStamp: pts, duration: .invalid,
            frameProperties: props, infoFlagsOut: nil
        ) { [weak self] status, flags, sb in
            defer { shared.endFrame() }
            self?.note(ok: status == noErr && sb != nil, dropped: flags.contains(.frameDropped))
            guard status == noErr, let sb = sb, let res = Encoder.toAnnexB(sb) else { return }
            Perf.shared.addEnc(Double(DispatchTime.now().uptimeNanoseconds - encT) / 1e6)
            let annexB = res.0, isKey = res.1
            let elapsed = Double(DispatchTime.now().uptimeNanoseconds - start) / 100_000.0 // décimas de ms
            var payload = Data([1])
            var idBE = poseId.bigEndian
            payload.append(Data(bytes: &idBE, count: 4))
            payload.append(isKey ? 1 : 0)
            var msBE = UInt16(min(65535.0, elapsed)).bigEndian
            payload.append(Data(bytes: &msBE, count: 2))
            payload.append(annexB)
            link.send(payload)
        }
        if st != noErr { shared.endFrame() }
    }

    /// Convierte la salida de VideoToolbox (AVCC) a Annex-B, que es lo que espera WebCodecs.
    static func toAnnexB(_ sb: CMSampleBuffer) -> (Data, Bool)? {
        var isKey = true
        if let arr = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [NSDictionary],
           let first = arr.first,
           let notSync = first[kCMSampleAttachmentKey_NotSync as String] as? Bool {
            isKey = !notSync
        }
        let startCode: [UInt8] = [0, 0, 0, 1]
        var out = Data()
        if isKey, let fmt = CMSampleBufferGetFormatDescription(sb) {
            // (HEVC trae VPS, SPS y PPS; H.264, SPS y PPS)
            let hevc = CMFormatDescriptionGetMediaSubType(fmt) == kCMVideoCodecType_HEVC
            let getPS = { (i: Int, p: UnsafeMutablePointer<UnsafePointer<UInt8>?>?, sz: UnsafeMutablePointer<Int>?, n: UnsafeMutablePointer<Int>?) -> OSStatus in
                hevc ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, parameterSetIndex: i, parameterSetPointerOut: p, parameterSetSizeOut: sz,
                                                                           parameterSetCountOut: n, nalUnitHeaderLengthOut: nil)
                     : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, parameterSetIndex: i, parameterSetPointerOut: p, parameterSetSizeOut: sz,
                                                                           parameterSetCountOut: n, nalUnitHeaderLengthOut: nil)
            }
            var count = 0
            _ = getPS(0, nil, nil, &count)
            for i in 0..<count {
                var ptr: UnsafePointer<UInt8>?
                var size = 0
                _ = getPS(i, &ptr, &size, nil)
                if let ptr = ptr {
                    out.append(contentsOf: startCode)
                    out.append(ptr, count: size)
                }
            }
        }
        guard let bb = CMSampleBufferGetDataBuffer(sb) else { return nil }
        let total = CMBlockBufferGetDataLength(bb)
        var raw = [UInt8](repeating: 0, count: total)
        let copied = raw.withUnsafeMutableBytes {
            CMBlockBufferCopyDataBytes(bb, atOffset: 0, dataLength: total, destination: $0.baseAddress!)
        }
        guard copied == kCMBlockBufferNoErr else { return nil }
        var off = 0
        while off + 4 <= total {
            let n = Int(raw[off]) << 24 | Int(raw[off + 1]) << 16 | Int(raw[off + 2]) << 8 | Int(raw[off + 3])
            off += 4
            guard n > 0, off + n <= total else { break }
            out.append(contentsOf: startCode)
            out.append(contentsOf: raw[off..<(off + n)])
            off += n
        }
        return (out, isKey)
    }
}

// MARK: - Renderizador Metal
struct Inst { var model: simd_float4x4; var color: SIMD4<Float> }

let shaderSource = """
#include <metal_stdlib>
using namespace metal;
struct Inst { float4x4 model; float4 color; };
struct VOut { float4 pos [[position]]; float4 color; };
vertex VOut vmain(uint vid [[vertex_id]], uint iid [[instance_id]],
                  const device packed_float3* verts [[buffer(0)]],
                  const device Inst* insts [[buffer(1)]],
                  constant float4x4& vp [[buffer(2)]]) {
    float3 p = float3(verts[vid]);
    VOut o;
    o.pos = vp * insts[iid].model * float4(p, 1.0);
    float shade = 0.75 + 0.5 * p.y + 0.3 * p.x;
    o.color = float4(insts[iid].color.rgb * shade, 1.0);
    return o;
}
fragment float4 fmain(VOut in [[stage_in]]) { return in.color; }
struct QOut { float4 pos [[position]]; float2 uv; };
vertex QOut vquad(uint vid [[vertex_id]], constant float4* c [[buffer(0)]], constant float4x4& vp [[buffer(1)]]) {
    uint idx[6] = {0, 1, 2, 0, 2, 3};
    float2 uvs[4] = {float2(0, 1), float2(1, 1), float2(1, 0), float2(0, 0)};
    uint i = idx[vid];
    QOut o; o.pos = vp * c[i]; o.uv = uvs[i]; return o;
}
fragment float4 fquad(QOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return float4(t.sample(s, in.uv).rgb, 1.0);
}
"""

final class Renderer {
    let device: MTLDevice
    let queue: MTLCommandQueue
    let pipeline: MTLRenderPipelineState
    let quadPipeline: MTLRenderPipelineState
    var lobby: Lobby?
    let depthState: MTLDepthStencilState
    let depthTex: MTLTexture
    let cubeBuf: MTLBuffer
    var instBuffers: [MTLBuffer] = []
    var bufIndex = 0
    var frameNo: Int64 = 0
    var pool: CVPixelBufferPool!
    var texCache: CVMetalTextureCache!
    let encoder: Encoder
    let shared: Shared
    let extra: [(SIMD3<Float>, Float, SIMD4<Float>)]

    init?(encoder: Encoder, shared: Shared) {
        guard let dev = MTLCreateSystemDefaultDevice(), let q = dev.makeCommandQueue() else {
            print("ERROR: Metal no disponible"); return nil
        }
        device = dev; queue = q
        self.encoder = encoder; self.shared = shared
        do {
            let lib = try dev.makeLibrary(source: shaderSource, options: nil)
            let pd = MTLRenderPipelineDescriptor()
            pd.vertexFunction = lib.makeFunction(name: "vmain")
            pd.fragmentFunction = lib.makeFunction(name: "fmain")
            pd.colorAttachments[0].pixelFormat = .bgra8Unorm
            pd.depthAttachmentPixelFormat = .depth32Float
            pipeline = try dev.makeRenderPipelineState(descriptor: pd)
            let qd = MTLRenderPipelineDescriptor()
            qd.vertexFunction = lib.makeFunction(name: "vquad")
            qd.fragmentFunction = lib.makeFunction(name: "fquad")
            qd.colorAttachments[0].pixelFormat = .bgra8Unorm
            qd.depthAttachmentPixelFormat = .depth32Float
            quadPipeline = try dev.makeRenderPipelineState(descriptor: qd)
        } catch {
            print("ERROR compilando shaders: \(error)"); return nil
        }
        let dd = MTLDepthStencilDescriptor()
        dd.depthCompareFunction = .lessEqual
        dd.isDepthWriteEnabled = true
        depthState = dev.makeDepthStencilState(descriptor: dd)!
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float, width: SW, height: SH, mipmapped: false)
        td.usage = .renderTarget
        td.storageMode = .private
        depthTex = dev.makeTexture(descriptor: td)!

        // Cubo unitario: 6 caras x 2 triángulos
        let faces: [[Float]] = [
            [-1,-1,1, 1,-1,1, 1,1,1, -1,1,1], [1,-1,-1, -1,-1,-1, -1,1,-1, 1,1,-1],
            [-1,-1,-1, -1,-1,1, -1,1,1, -1,1,-1], [1,-1,1, 1,-1,-1, 1,1,-1, 1,1,1],
            [-1,1,1, 1,1,1, 1,1,-1, -1,1,-1], [-1,-1,-1, 1,-1,-1, 1,-1,1, -1,-1,1]]
        var verts: [Float] = []
        for f in faces { for i in [0, 1, 2, 0, 2, 3] { verts += [f[i*3] * 0.5, f[i*3+1] * 0.5, f[i*3+2] * 0.5] } }
        cubeBuf = dev.makeBuffer(bytes: verts, length: verts.count * 4, options: .storageModeShared)!

        // Cubos extra en posiciones pseudoaleatorias fijas
        var seed: UInt32 = 12345
        func rnd() -> Float { seed = seed &* 1664525 &+ 1013904223; return Float(seed >> 8) / Float(1 << 24) }
        var ex: [(SIMD3<Float>, Float, SIMD4<Float>)] = []
        for _ in 0..<EXTRA_CUBES {
            let a = rnd() * 2 * .pi, r = 3.5 + rnd() * 6, y = 0.3 + rnd() * 5
            ex.append((SIMD3<Float>(cos(a) * r, y, sin(a) * r), 0.1 + rnd() * 0.3,
                       SIMD4<Float>(0.3 + rnd() * 0.7, 0.3 + rnd() * 0.7, 0.3 + rnd() * 0.7, 1)))
        }
        extra = ex

        let maxInst = 160 + EXTRA_CUBES
        for _ in 0..<3 {
            instBuffers.append(dev.makeBuffer(length: maxInst * MemoryLayout<Inst>.stride, options: .storageModeShared)!)
        }

        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: W,
            kCVPixelBufferHeightKey as String: H,
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any]()]
        var p: CVPixelBufferPool?
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &p)
        var tc: CVMetalTextureCache?
        CVMetalTextureCacheCreate(nil, nil, dev, nil, &tc)
        guard let pp = p, let tcc = tc else { print("ERROR: no se pudo crear el pool de imágenes"); return nil }
        pool = pp; texCache = tcc
        print("GPU: \(dev.name) · \(W)x\(H) (\(EYE) por ojo) · \(FPS) fps · \(BITRATE / 1_000_000) Mbps · \(EXTRA_CUBES) cubos extra")
    }

    func scene(_ t: Float) -> [Inst] {
        var out: [Inst] = []
        let grid = SIMD4<Float>(0.35, 0.3, 0.5, 1)
        for i in -5...5 {
            out.append(Inst(model: trs(Float(i), 0, 0, 0.02, 0.01, 10), color: grid))
            out.append(Inst(model: trs(0, 0, Float(i), 10, 0.01, 0.02), color: grid))
        }
        // Anillo de cubos de colores alrededor tuyo
        for i in 0..<12 {
            let a = Float(i) / 12 * 2 * .pi + t * 0.2
            let x = cos(a) * 2.2, z = sin(a) * 2.2, y = 1.3 + 0.2 * sin(t * 1.5 + Float(i))
            let hue = Float(i) / 12
            let col = SIMD4<Float>(0.5 + 0.5 * cos(6.28 * hue), 0.5 + 0.5 * cos(6.28 * (hue - 0.33)),
                                   0.5 + 0.5 * cos(6.28 * (hue - 0.66)), 1)
            out.append(Inst(model: trs(x, y, z, 1, 1, 1) * rotY(t + Float(i)) * rotX(t * 0.7) * trs(0, 0, 0, 0.35, 0.35, 0.35), color: col))
        }
        for (x, z) in [(-3, -3), (3, -3), (-3, 3), (3, 3)] as [(Float, Float)] {
            out.append(Inst(model: trs(x, 1.5, z, 0.25, 3, 0.25), color: SIMD4<Float>(0.9, 0.6, 0.3, 1)))
        }
        for (p, s, c) in extra {
            out.append(Inst(model: trs(p.x, p.y, p.z, 1, 1, 1) * rotY(t * 0.5 + p.x) * trs(0, 0, 0, s, s, s), color: c))
        }
        return out
    }

    func render(pose: Pose, t: Float) {
        guard shared.beginFrame() else { return }
        let start = DispatchTime.now().uptimeNanoseconds
        var pbOut: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pbOut)
        var cvTexOut: CVMetalTexture?
        if let pb = pbOut {
            CVMetalTextureCacheCreateTextureFromImage(nil, texCache, pb, nil, .bgra8Unorm, W, H, 0, &cvTexOut)
        }
        guard let pb = pbOut, let cvTex = cvTexOut, let tex = CVMetalTextureGetTexture(cvTex),
              let cb = queue.makeCommandBuffer() else { shared.endFrame(); return }

        lobby?.update(pose: pose)
        var insts = lobby?.cubes() ?? scene(t)
        if insts.count > 160 + EXTRA_CUBES { insts.removeLast(insts.count - 160 - EXTRA_CUBES) }
        let quads = lobby?.quads() ?? []
        let ib = instBuffers[bufIndex]
        bufIndex = (bufIndex + 1) % instBuffers.count
        insts.withUnsafeBytes { _ = memcpy(ib.contents(), $0.baseAddress!, $0.count) }

        let rp = MTLRenderPassDescriptor()
        // (con foveated encoding se dibuja la imagen completa y después se achica el borde)
        let fv = Foveator.shared, full = fv?.source(SW, SH, .bgra8Unorm)
        rp.colorAttachments[0].texture = full ?? tex
        rp.colorAttachments[0].loadAction = .clear
        rp.colorAttachments[0].clearColor = MTLClearColor(red: 0.10, green: 0.06, blue: 0.14, alpha: 1)
        rp.colorAttachments[0].storeAction = .store
        rp.depthAttachment.texture = depthTex
        rp.depthAttachment.loadAction = .clear
        rp.depthAttachment.clearDepth = 1
        rp.depthAttachment.storeAction = .dontCare
        guard let enc = cb.makeRenderCommandEncoder(descriptor: rp) else { shared.endFrame(); return }
        enc.setRenderPipelineState(pipeline)
        enc.setDepthStencilState(depthState)
        enc.setVertexBuffer(cubeBuf, offset: 0, index: 0)
        enc.setVertexBuffer(ib, offset: 0, index: 1)
        for (i, eye) in pose.eyes.prefix(2).enumerated() {
            enc.setViewport(MTLViewport(originX: Double(i * EYE), originY: 0, width: Double(EYE), height: Double(SH), znear: 0, zfar: 1))
            var vp = zFix * eye.proj * eye.view
            enc.setVertexBytes(&vp, length: MemoryLayout<simd_float4x4>.stride, index: 2)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 36, instanceCount: insts.count)
            if !quads.isEmpty {
                enc.setRenderPipelineState(quadPipeline)
                for q in quads {
                    var c = q.corners
                    enc.setVertexBytes(&c, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    enc.setVertexBytes(&vp, length: MemoryLayout<simd_float4x4>.stride, index: 1)
                    enc.setFragmentTexture(q.texture, index: 0)
                    enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6)
                }
                enc.setRenderPipelineState(pipeline)
                enc.setVertexBuffer(cubeBuf, offset: 0, index: 0)
                enc.setVertexBuffer(ib, offset: 0, index: 1)
            }
        }
        enc.endEncoding()
        if let fv, let full { fv.warp(cb, full, into: tex) }

        let n = shared.nextFrameNo()
        let key = shared.takeKey()
        let encoder = self.encoder
        let poseId = pose.id
        cb.addCompletedHandler { _ in
            withExtendedLifetime(cvTex) {
                encoder.encode(pb, frameNo: n, poseId: poseId, forceKey: key, start: start)
            }
        }
        cb.commit()
    }
}


// MARK: - Ajustes avanzados por juego
/// Escala de render (el juego dibuja menos píxeles y MetalFX lo agranda), escala automática (se ajusta sola entre
/// sesiones según los cuadros por segundo), campo de visión recortado y emular controles de Index. Hay unos para todos
/// los juegos ("") y cada juego guarda solo lo que se cambió para él. Se guardan en perfiles_juegos.json.
final class GameProfiles {
    static let shared = GameProfiles()
    struct P: Codable { var scale: Double?; var auto: Bool?; var fovk: Double?; var index: Bool?; var vive: Bool?
                        var eye: Int?; var fps: Int?; var mbps: Int?; var fov: String? }   // (calidad propia del juego)
    private let lock = NSLock()
    private var data: [String: P] = [:]
    private let file = URL(fileURLWithPath: "perfiles_juegos.json")
    private(set) var current = ""          // el juego abierto ("" = ninguno)
    private(set) var activeK = 1.0         // campo de visión del juego abierto (lo que se le pidió)
    private var autoN = 0, autoFps = 0.0, autoGame = 0.0, autoHz = 0.0, autoSince = 0.0
    private var mtime: Date?
    init() { reload() }
    /// Lee el archivo si cambió (VRmin Host también lo edita, desde sus Ajustes)
    private func reload() {
        let t = (try? FileManager.default.attributesOfItem(atPath: file.path))?[.modificationDate] as? Date
        guard t != mtime else { return }
        mtime = t
        if let d = try? Data(contentsOf: file), let o = try? JSONDecoder().decode([String: P].self, from: d) { data = o }
    }
    private func save() {
        if let d = try? JSONEncoder().encode(data) { try? d.write(to: file) }
        mtime = (try? FileManager.default.attributesOfItem(atPath: file.path))?[.modificationDate] as? Date
    }
    /// Lo que vale para un juego: lo de todos los juegos con lo suyo encima
    func effective(_ key: String) -> (scale: Double, auto: Bool, fovk: Double, index: Bool, vive: Bool) {
        lock.lock(); defer { lock.unlock() }
        reload()
        let a = data[""] ?? P(), g = key.isEmpty ? P() : (data[key] ?? P())
        return (min(1, max(0.5, g.scale ?? a.scale ?? 1)), g.auto ?? a.auto ?? false, min(1, max(0.75, g.fovk ?? a.fovk ?? 1)), g.index ?? a.index ?? false, g.vive ?? a.vive ?? false)
    }
    func hasOwn(_ key: String) -> Bool { lock.lock(); defer { lock.unlock() }; return !key.isEmpty && data[key] != nil }
    /// El juego que se abrió: por la carpeta de Steam (si se encuentra) o por el nombre que da el juego
    func begin(app: String) {
        let key = GameProfiles.steamFolder() ?? app
        lock.lock(); current = key; autoN = 0; autoFps = 0; autoGame = 0; autoSince = Date().timeIntervalSince1970; lock.unlock()
        let e = effective(key)
        lock.lock(); activeK = e.fovk; lock.unlock()
        print("[juego] Ajustes de «\(key)»: escala \(Int(e.scale * 100)) %\(e.auto ? " (automática)" : ""), campo de visión \(Int(e.fovk * 100)) %\(e.index ? ", controles de Index" : "")\(e.vive ? ", controles de Vive" : "")")
    }
    /// La línea "config" para el runtime (con «pv»: son los ajustes de este juego)
    func configLine(fps: Int, hello: Bool) -> String {
        let e = effective(current)
        let eye = max(256, Int((Double(EYE) * e.scale * e.fovk / 8).rounded()) * 8)
        // «vg»: Modo Centinela (el runtime adelanta las poses al momento en que se ve el cuadro: no hay reproyección)
        return "{\"type\":\"config\",\"eye\":\(eye),\"fps\":\(fps),\"fovk\":\(String(format: "%.3f", e.fovk)),\"index\":\(e.index ? 1 : 0),\"vive\":\(e.vive ? 1 : 0),\"vg\":\(Vigia.shared.on ? 1 : 0)\(hello ? ",\"pv\":1" : "")}"
    }
    /// Desde el Quest: cambiar lo de un juego ("game": el abierto; "key:<carpeta>": cualquiera) o lo de todos ("all"),
    /// o volver a lo de todos
    func apply(scope: String, scale: Double?, auto: Bool?, fovk: Double?, index: Bool?, vive: Bool? = nil, reset: Bool,
               eye: Int? = nil, fps: Int? = nil, mbps: Int? = nil, fov: String? = nil) {
        lock.lock()
        reload()
        let key = scope == "game" ? current : scope.hasPrefix("key:") ? String(scope.dropFirst(4)) : ""
        if scope != "all" && key.isEmpty { lock.unlock(); return }
        if reset { data[key] = nil }
        else {
            var p = data[key] ?? P()
            if let v = scale { p.scale = min(1, max(0.5, v)) }
            if let v = auto { p.auto = v }
            if let v = fovk { p.fovk = min(1, max(0.75, v)) }
            if let v = index { p.index = v }
            if let v = vive { p.vive = v }
            if !key.isEmpty {   // (la calidad de «todos» es la general de VRmin Host, no va acá)
                if let v = eye, (512...2560).contains(v) { p.eye = v }
                if let v = fps, [72, 80, 90, 120].contains(v) { p.fps = v }
                if let v = mbps, (10...300).contains(v) { p.mbps = v }
                if let v = fov, v.count < 80 { p.fov = v }
            }
            data[key] = p
        }
        save()
        lock.unlock()
        print("[juego] Ajustes avanzados de \(key.isEmpty ? "todos los juegos" : "«\(key)»") guardados (se aplican la próxima vez que se abra el juego)")
    }
    /// Lo que ve el Quest en Ajustes › Avanzado
    func state() -> [String: Any] {
        let a = effective("")
        var o: [String: Any] = ["all": ["scale": a.scale, "auto": a.auto, "fovk": a.fovk, "index": a.index, "vive": a.vive]]
        // Los juegos con ajustes propios (lo que eligió el usuario para cada uno; lo demás sale de «all»)
        lock.lock()
        var own: [String: Any] = [:]
        for (k, p) in data where !k.isEmpty {
            var g: [String: Any] = [:]
            if let v = p.scale { g["scale"] = v }; if let v = p.auto { g["auto"] = v }
            if let v = p.fovk { g["fovk"] = v }; if let v = p.index { g["index"] = v }; if let v = p.vive { g["vive"] = v }
            if let v = p.eye { g["eye"] = v }; if let v = p.fps { g["fps"] = v }; if let v = p.mbps { g["mbps"] = v }; if let v = p.fov { g["fov"] = v }
            own[k] = g
        }
        lock.unlock()
        o["own"] = own
        if !current.isEmpty {
            let g = effective(current)
            o["game"] = ["name": current, "scale": g.scale, "auto": g.auto, "fovk": g.fovk, "index": g.index, "vive": g.vive, "own": hasOwn(current)]
        }
        return o
    }
    func end() { lock.lock(); current = ""; activeK = 1; lock.unlock() }
    /// La calidad que quiere un juego (la suya o la general) y si es distinta de la que corre ahora
    func quality(for key: String) -> (eye: Int, fps: Int, mbps: Int, fov: String, differs: Bool) {
        lock.lock(); reload(); let p = key.isEmpty ? P() : (data[key] ?? P()); lock.unlock()
        let eye = p.eye ?? GEN_EYE, fps = p.fps ?? GEN_FPS, mbps = p.mbps ?? GEN_MBPS, fov = p.fov ?? GEN_FOV
        let fv = Fovea(fov)   // (solo nivel y tamaño: el centro se ajusta solo)
        let differs = eye != EYE || fps != FPS || mbps * 1_000_000 != BITRATE || fv.level != FOVEA.level || (fv.on && fv.size != FOVEA.size)
        return (eye, fps, mbps, fov, differs)
    }
    /// Escala automática: cada 20 s mira si el juego llega a los cuadros por segundo del visor. Si no llega, baja la escala
    /// un 5 %; si sobra (el juego tarda menos del 70 % del tiempo de un cuadro), la sube. Queda para la próxima vez.
    func autoTick(_ d: Data) {
        guard let o = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
              let fps = o["fps"] as? Double, let hz = (o["hz"] as? Double) ?? (o["hz"] as? Int).map(Double.init), hz > 0 else { return }
        let game = o["juego"] as? Double ?? 0
        let key = current
        guard !key.isEmpty, effective(key).auto else { return }
        lock.lock()
        autoN += 1; autoFps += fps; autoGame += game; autoHz = hz
        let ready = autoN >= 20
        var p = data[key] ?? P()
        var changed = false
        if ready {
            let f = autoFps / Double(autoN), g = autoGame / Double(autoN)
            let cur = p.scale ?? data[""]?.scale ?? 1
            if f < autoHz * 0.93 && cur > 0.5 { p.scale = max(0.5, cur - 0.05); changed = true }
            else if f >= autoHz * 0.98 && g < 700 / autoHz && cur < 1 { p.scale = min(1, cur + 0.05); changed = true }
            autoN = 0; autoFps = 0; autoGame = 0
            if changed { data[key] = p; save() }
        }
        lock.unlock()
        if changed { print("[juego] Escala automática de «\(key)»: \(Int((p.scale ?? 1) * 100)) % (se usa la próxima vez que lo abras)") }
    }
    /// La carpeta de Steam del juego que está corriendo (steamapps\common\<carpeta>)
    static func steamFolder() -> String? {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/bin/ps"); p.arguments = ["-axo", "command="]
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return nil }
        let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        p.waitUntilExit()
        for l in out.split(separator: "\n") {
            guard let r = l.range(of: "steamapps\\common\\") else { continue }
            let rest = l[r.upperBound...]
            if let e = rest.firstIndex(of: "\\") { return String(rest[..<e]) }
        }
        return nil
    }
}

// MARK: - Conexión con el juego (runtime OpenXR dentro de GameHub)
final class GameLink {
    /// Generación de cuadros en el Quest: el juego dibuja a la mitad de cuadros (el Quest inventa los intermedios)
    var halfRate = false {
        didSet { if halfRate != oldValue { sendLine(Data(configLine.utf8)); print("[juego] Generación de cuadros \(halfRate ? "activada: el juego dibuja a \(FPS / 2) cuadros/s" : "apagada: el juego dibuja a \(FPS) cuadros/s")") } }
    }
    var configLine: String { GameProfiles.shared.configLine(fps: halfRate ? FPS / 2 : FPS, hello: false) }
    /// Todas las conexiones de juegos abiertas (puede haber copias viejas en segundo plano).
    private var clients: [Int32: Int] = [:]      // fd -> envíos fallidos seguidos
    private let lock = NSLock()
    private let frameLock = NSLock()
    private let encodeQueue = DispatchQueue(label: "game-encode", qos: .userInteractive)
    /// Para diagnóstico: en qué paso está el lector del juego y desde cuándo
    private var stage = "esperando"
    private var stageSince = DispatchTime.now().uptimeNanoseconds
    private var framesThisSecond = 0
    private func setStage(_ st: String) { stage = st; stageSince = DispatchTime.now().uptimeNanoseconds }
    private let encoder: Encoder
    private let shared: Shared
    private var pool: CVPixelBufferPool!
    /// El último cuadro mandado al visor: mientras el juego carga (no manda cuadros pero sigue conectado) se vuelve a
    /// mandar, así el visor muestra el juego quieto y no se va a la sala
    private let holdLock = NSLock()
    private var holdPB: CVPixelBuffer?, holdPose: UInt32 = 0, holdAt: UInt64 = 0
    private var loadingPB: CVPixelBuffer?, loadingFor: UInt64 = 0
    /// El cuadro dado, más oscuro y con «Cargando…» en el centro de cada ojo
    private func loadingFrame(from src: CVPixelBuffer) -> CVPixelBuffer? {
        var po: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &po)
        guard let dst = po else { return nil }
        CVPixelBufferLockBaseAddress(src, .readOnly); CVPixelBufferLockBaseAddress(dst, [])
        defer { CVPixelBufferUnlockBaseAddress(dst, []); CVPixelBufferUnlockBaseAddress(src, .readOnly) }
        let w = CVPixelBufferGetWidth(dst), h = CVPixelBufferGetHeight(dst)
        guard CVPixelBufferGetWidth(src) == w, CVPixelBufferGetHeight(src) == h,
              let sb = CVPixelBufferGetBaseAddress(src), let db = CVPixelBufferGetBaseAddress(dst) else { return nil }
        let sbpr = CVPixelBufferGetBytesPerRow(src), dbpr = CVPixelBufferGetBytesPerRow(dst)
        for y in 0..<h { memcpy(db + y * dbpr, sb + y * sbpr, min(sbpr, dbpr)) }
        guard let ctx = CGContext(data: db, width: w, height: h, bitsPerComponent: 8, bytesPerRow: dbpr, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        ctx.setFillColor(CGColor(gray: 0, alpha: 0.55)); ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        // (con Core Text: este hilo no es el principal)
        guard let font = CTFontCreateUIFontForLanguage(.emphasizedSystem, CGFloat(h) * 0.045, nil) else { return dst }
        let attrs: [NSAttributedString.Key: Any] = [NSAttributedString.Key(kCTFontAttributeName as String): font,
                                                    NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(gray: 1, alpha: 1)]
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: tr("Cargando…", "Loading…"), attributes: attrs))
        let bounds = CTLineGetBoundsWithOptions(line, [])
        for e in 0..<2 {
            let cx = CGFloat(w) * (CGFloat(e) * 0.5 + 0.25)
            ctx.textPosition = CGPoint(x: cx - bounds.width / 2, y: CGFloat(h) / 2 - bounds.height / 2)
            CTLineDraw(line, ctx)
        }
        return dst
    }
    private func hold(_ pb: CVPixelBuffer, _ pose: UInt32) {
        holdLock.lock(); holdPB = pb; holdPose = pose; holdAt = DispatchTime.now().uptimeNanoseconds; holdLock.unlock()
    }

    init(encoder: Encoder, shared: Shared) {
        self.encoder = encoder
        self.shared = shared
        Vigia.shared.onChange = { [weak self] in if let self { self.sendLine(Data(self.configLine.utf8)) } }
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: W,
            kCVPixelBufferHeightKey as String: H,
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any]()]
        var p: CVPixelBufferPool?
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &p)
        pool = p
    }

    func start(port: UInt16) {
        // Juego cargando (conectado pero sin cuadros desde hace más de 0,3 s, hasta 2 min): repetir el último cuadro
        // unas 10 veces por segundo, con su pose (el visor lo deja quieto en su lugar)
        Thread.detachNewThread { [self] in
            while true {
                usleep(100_000)
                lock.lock(); let open = clients.count; lock.unlock()
                holdLock.lock(); let pb = holdPB, pose = holdPose, at = holdAt; if open == 0 { holdPB = nil }; holdLock.unlock()
                guard open > 0, let pb, at > 0 else { continue }
                let now = DispatchTime.now().uptimeNanoseconds, age = now - at
                guard age > 300_000_000, age < 120_000_000_000, shared.beginFrame() else { continue }
                shared.markGameFrame()
                // (la primera vez: una copia oscurecida con «Cargando…» en cada ojo, para que no parezca colgado)
                if loadingFor != at { loadingPB = loadingFrame(from: pb); loadingFor = at }
                let show = loadingPB ?? pb
                let fn = shared.nextFrameNo(), enc = encoder
                encodeQueue.async { enc.encode(show, frameNo: fn, poseId: pose, forceKey: false, start: now) }
            }
        }
        // Vigilante: cada 3 s avisa si el lector del juego quedó trabado en algún paso
        Thread.detachNewThread { [self] in
            var lastFrames = 0
            while true {
                sleep(3)
                lock.lock(); let open = clients.count; lock.unlock()
                if open == 0 { continue }
                let secs = Double(DispatchTime.now().uptimeNanoseconds - stageSince) / 1e9
                let fps = (frames - lastFrames) / 3
                lastFrames = frames
                if fps < 20 || (stage != "leyendo cabecera" && secs > 1) {
                    print("[mac] Cuadros del juego recibidos: \(fps)/s · paso actual: \(stage) (hace \(String(format: "%.1f", secs)) s) · en codificación: \(shared.inFlightCount)")
                }
            }
        }
        Thread.detachNewThread { [self] in
            let ls = socket(AF_INET, SOCK_STREAM, 0)
            var one: Int32 = 1
            setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
            var addr = sockaddr_in()
            addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            addr.sin_family = sa_family_t(AF_INET)
            addr.sin_port = port.bigEndian
            addr.sin_addr.s_addr = inet_addr("127.0.0.1")
            let r = withUnsafePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.bind(ls, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
            if r != 0 || listen(ls, 8) != 0 { print("ERROR: no se pudo abrir el puerto \(port) para el juego"); return }
            while true {
                let c = accept(ls, nil, nil)
                if c < 0 { continue }
                setsockopt(c, Int32(IPPROTO_TCP), TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
                var big: Int32 = 16 << 20
                setsockopt(c, SOL_SOCKET, SO_RCVBUF, &big, socklen_t(MemoryLayout<Int32>.size))
                // Nunca cerramos a nadie: cada conexión vive en su propio hilo hasta que el juego la corta.
                lock.lock(); clients[c] = 0; let total = clients.count; lock.unlock()
                print("[juego] Un juego se conectó a QuestVRBridge (conexiones abiertas: \(total))")
                sendTo(c, Data(configLine.utf8))
                shared.requestKey()
                Thread.detachNewThread { [self] in
                    readLoop(c)
                    lock.lock(); clients[c] = nil; pending[c] = nil; let left = clients.count; lock.unlock()
                    Darwin.close(c)
                    print("[juego] Conexión con un juego cerrada (quedan: \(left))")
                    if left == 0 { GameProfiles.shared.end() }
                }
            }
        }
    }

    /// Bytes que quedaron sin entregar a cada conexión (nunca esperamos a nadie).
    private var pending: [Int32: Data] = [:]

    /// Envía sin bloquear jamás. Si una conexión no lee (por ejemplo, una copia del juego
    /// congelada), sus datos se acumulan aparte y las demás conexiones no se enteran.
    private func sendTo(_ fd: Int32, _ line: Data) {
        lock.lock(); defer { lock.unlock() }
        guard clients[fd] != nil else { return }
        var buf = pending[fd] ?? Data()
        if buf.count > 64 * 1024 {
            // Lleva rato sin leer: descartamos lo viejo; la próxima pose completa reemplaza a todo
            if (clients[fd] ?? 0) > 600 { Darwin.shutdown(fd, SHUT_RDWR) }
            clients[fd] = (clients[fd] ?? 0) + 1
            return
        }
        buf.append(line)
        buf.append(0x0A)
        let n = buf.withUnsafeBytes { raw -> Int in
            Darwin.send(fd, raw.baseAddress!, raw.count, Int32(MSG_DONTWAIT))
        }
        if n >= buf.count {
            pending[fd] = nil
            clients[fd] = 0
        } else {
            if n > 0 { buf.removeFirst(n) }
            pending[fd] = buf
            clients[fd] = (clients[fd] ?? 0) + 1
        }
    }

    func sendLine(_ line: Data) {
        lock.lock(); let fds = Array(clients.keys); lock.unlock()
        for fd in fds { sendTo(fd, line) }
    }

    private func readExact(_ fd: Int32, _ p: UnsafeMutableRawPointer, _ n: Int) -> Bool {
        var off = 0
        while off < n {
            let r = Darwin.recv(fd, p + off, n - off, 0)
            if r <= 0 { return false }
            off += r
        }
        return true
    }

    private func readLoop(_ fd: Int32) {
        var buf = [UInt8](repeating: 0, count: 16 << 20)
        var hdr = [UInt8](repeating: 0, count: 4)
        // Memoria compartida con el runtime (archivo mapeado): los cuadros no pasan por la red
        var shm: UnsafeMutableRawPointer? = nil
        var shmLen = 0, shmSlot = 0
        var shmBuf: MTLBuffer? = nil   // (la misma memoria compartida, vista por la GPU)
        var gpuTex: [MTLTexture] = []   // copia directa: las texturas compartidas del juego (una por lugar)
        defer { if let m = shm { munmap(m, shmLen) } }
        while true {
            setStage("leyendo cabecera")
            guard hdr.withUnsafeMutableBytes({ readExact(fd, $0.baseAddress!, 4) }) else { return }
            setStage("leyendo imagen")
            let n = Int(hdr[0]) << 24 | Int(hdr[1]) << 16 | Int(hdr[2]) << 8 | Int(hdr[3])
            if n <= 0 || n > 128 << 20 { return }
            if n > buf.count { buf = [UInt8](repeating: 0, count: n) }
            guard buf.withUnsafeMutableBytes({ readExact(fd, $0.baseAddress!, n) }) else { return }
            switch buf[0] {
            case 3:
                print("[juego] " + (String(bytes: buf[1..<n], encoding: .utf8) ?? "?"))
            case 7:
                Perf.shared.setRuntime(Data(buf[1..<n]))
                GameProfiles.shared.autoTick(Data(buf[1..<n]))
            case 9:
                // El runtime dice qué juego es: se le mandan sus ajustes (resolución, campo de visión, controles de Index)
                let app = String(decoding: buf[1..<n], as: UTF8.self)
                GameProfiles.shared.begin(app: app)
                sendTo(fd, Data(GameProfiles.shared.configLine(fps: halfRate ? FPS / 2 : FPS, hello: true).utf8))
            case 4:
                // Esta conexión es solo de imágenes: no le mandamos poses
                lock.lock(); clients[fd] = nil; pending[fd] = nil; let left = clients.count; lock.unlock()
                print("[juego] Conexión de imágenes identificada (conexiones de control: \(left))")
            case 5:
                // Oferta de memoria compartida: nonce, tamaño de cada lugar, ruta del archivo en la Mac
                guard n > 9 else { break }
                let nonce = UInt32(buf[1]) << 24 | UInt32(buf[2]) << 16 | UInt32(buf[3]) << 8 | UInt32(buf[4])
                let slot = Int(buf[5]) << 24 | Int(buf[6]) << 16 | Int(buf[7]) << 8 | Int(buf[8])
                let path = String(decoding: buf[9..<n], as: UTF8.self)
                shmBuf = nil
                if let m = shm { munmap(m, shmLen); shm = nil }
                let f = open(path, O_RDWR)
                if f < 0 { print("[juego] Memoria compartida: no pude abrir \(path)"); break }
                let total = 64 + slot * 3
                let m = mmap(nil, total, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0)
                close(f)
                guard let mm = m, mm != MAP_FAILED else { print("[juego] Memoria compartida: mmap falló"); break }
                let h = mm.assumingMemoryBound(to: UInt32.self)
                if h[0] == 0x31535256 && h[1] == nonce {
                    shm = mm; shmLen = total; shmSlot = slot
                    let page = Int(getpagesize()), rounded = (total + page - 1) / page * page
                    shmBuf = Foveator.shared?.dev.makeBuffer(bytesNoCopy: mm, length: rounded, options: .storageModeShared, deallocator: nil)
                    print("[juego] Memoria compartida \(shmBuf != nil ? "también vista por la GPU (sin copias en la Mac)" : "solo por el procesador")")
                    OSMemoryBarrier()
                    h[7] = 0x32564F50   // reserved[2] «POV2»: sé dibujar los paneles flotantes del juego (tipo 12)
                    OSMemoryBarrier()
                    h[2] = nonce   // confirmación: el runtime pasa a usarla
                    print("[juego] Memoria compartida activa (los cuadros ya no pasan por la red)")
                } else {
                    munmap(mm, total)
                    print("[juego] Memoria compartida: el archivo no coincide; sigue por la red")
                }
            case 6, 12:
                // Cuadro listo en la memoria compartida (12: con paneles flotantes después de la imagen)
                guard n >= (buf[0] == 12 ? 12 : 10), let m = shm else { break }
                let slot = Int(buf[1])
                guard slot < 3 else { break }
                let state = (m + 32 + slot * 4).assumingMemoryBound(to: Int32.self)
                OSMemoryBarrier()
                let ew = Int(buf[7]) << 8 | Int(buf[8]), eh = Int(buf[9]) << 8 | Int(buf[10])
                let poseId = UInt32(buf[2]) << 24 | UInt32(buf[3]) << 16 | UInt32(buf[4]) << 8 | UInt32(buf[5])
                let nq = buf[0] == 12 ? Int(buf[11]) : 0
                if ew * 2 * eh * 4 <= shmSlot {
                    setStage("esperando turno")
                    frameLock.lock()
                    setStage("procesando imagen")
                    let base = UnsafeRawPointer(m + 64 + slot * shmSlot)
                    // cada panel: w, h, banderas, (relleno), 2 ojos × 4 esquinas × (x, y, z, w), y después sus píxeles
                    var quads: [Foveator.GameQuad] = [], off = ew * 2 * eh * 4
                    for _ in 0..<nq {
                        guard off + 144 <= shmSlot else { break }
                        let hp = (base + off).assumingMemoryBound(to: UInt32.self)
                        let qw = Int(hp[0]), qh = Int(hp[1]), fl = hp[2]
                        guard qw > 0, qh > 0, off + 144 + qw * qh * 4 <= shmSlot else { break }
                        let fp = (base + off + 16).assumingMemoryBound(to: Float.self)
                        let clip = (0..<2).map { e in (0..<4).map { k in SIMD4<Float>(fp[e * 16 + k * 4], fp[e * 16 + k * 4 + 1], fp[e * 16 + k * 4 + 2], fp[e * 16 + k * 4 + 3]) } }
                        quads.append(Foveator.GameQuad(w: qw, h: qh, flags: fl, clip: clip, pixels: base + off + 144, offset: 64 + slot * shmSlot + off + 144))
                        off += 144 + qw * qh * 4
                    }
                    handlePixels(base, poseId: poseId, isRGBA: buf[6] == 1, ew: ew, eh: eh, quads: quads,
                                 gpu: shmBuf.map { ($0, 64 + slot * shmSlot) })
                    frameLock.unlock()
                }
                OSMemoryBarrier()
                state.pointee = 0   // lugar libre para el próximo cuadro
            case 10:
                // Copia directa: el juego comparte sus texturas (generación, ancho y alto de un ojo, formato, 3 nombres)
                guard n >= 10 + 3 * 56, let m = shm else { break }   // [10][gen 4][ancho 2][alto 2][formato][3 nombres de 56]
                let h = m.assumingMemoryBound(to: UInt32.self)
                let gen = UInt32(buf[1]) << 24 | UInt32(buf[2]) << 16 | UInt32(buf[3]) << 8 | UInt32(buf[4])
                let ew = Int(buf[5]) << 8 | Int(buf[6]), eh = Int(buf[7]) << 8 | Int(buf[8])
                var list: [MTLTexture] = []
                if let dev = Foveator.shared?.dev {
                    for i in 0..<3 {
                        let bytes = buf[(10 + i * 56)..<(10 + i * 56 + 56)].prefix { $0 != 0 }
                        if let t = GpuShare.open(String(decoding: bytes, as: UTF8.self), device: dev), t.width >= ew * 2, t.height >= eh { list.append(t) }
                    }
                }
                gpuTex = list.count == 3 ? list : []
                h[6] = gen   // reserved[1]
                OSMemoryBarrier()
                h[5] = gpuTex.isEmpty ? 0x4c494146 : 0x31555047   // reserved[0]: «FAIL» o «GPU1»
                print(gpuTex.isEmpty ? "[juego] Copia directa: no pude abrir las texturas del juego; sigue la memoria compartida"
                                     : "[juego] Copia directa activa: el cuadro del juego no sale de la GPU (\(ew)x\(eh) por ojo, \(list[0].pixelFormat.rawValue))")
            case 11:
                // Cuadro listo en una textura compartida
                guard n >= 10, let m = shm else { break }
                let slot = Int(buf[1])
                guard slot < 3 else { break }
                let state = (m + 32 + slot * 4).assumingMemoryBound(to: Int32.self)
                OSMemoryBarrier()
                let ew = Int(buf[7]) << 8 | Int(buf[8]), eh = Int(buf[9]) << 8 | Int(buf[10])
                let poseId = UInt32(buf[2]) << 24 | UInt32(buf[3]) << 16 | UInt32(buf[4]) << 8 | UInt32(buf[5])
                if slot < gpuTex.count {
                    setStage("esperando turno")
                    frameLock.lock()
                    setStage("procesando imagen")
                    handleTexture(gpuTex[slot], poseId: poseId, ew: ew, eh: eh)
                    frameLock.unlock()
                }
                OSMemoryBarrier()
                state.pointee = 0   // (la GPU ya terminó de leerla: warpTexture espera)
            case 2:
                if n > 10 {
                    setStage("esperando turno")
                    frameLock.lock()
                    setStage("procesando imagen")
                    buf.withUnsafeBytes { handleFrame($0.baseAddress!, n) }
                    frameLock.unlock()
                }
            default:
                break
            }
        }
    }

    private var frames = 0
    private var lastSnapshot: UInt64 = 0

    /// Guarda una captura (a la mitad de tamaño) del cuadro del juego en capturas/juego.png
    private func saveSnapshot(_ base: UnsafeMutableRawPointer, _ bpr: Int) {
        let cs = CGColorSpaceCreateDeviceRGB()
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        guard let ctx = CGContext(data: base, width: W, height: H, bitsPerComponent: 8, bytesPerRow: bpr, space: cs, bitmapInfo: info.rawValue),
              let img = ctx.makeImage() else { return }
        let small = CGContext(data: nil, width: W / 2, height: H / 2, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                              bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
        small?.interpolationQuality = .medium
        small?.draw(img, in: CGRect(x: 0, y: 0, width: W / 2, height: H / 2))
        guard let out = small?.makeImage() else { return }
        try? FileManager.default.createDirectory(atPath: "capturas", withIntermediateDirectories: true)
        let url = URL(fileURLWithPath: "capturas/juego.png") as CFURL
        if let dest = CGImageDestinationCreateWithURL(url, UTType.png.identifier as CFString, 1, nil) {
            CGImageDestinationAddImage(dest, out, nil)
            CGImageDestinationFinalize(dest)
        }
    }

    private func handleFrame(_ p: UnsafeRawPointer, _ n: Int) {
        let b = p.assumingMemoryBound(to: UInt8.self)
        let poseId = UInt32(b[1]) << 24 | UInt32(b[2]) << 16 | UInt32(b[3]) << 8 | UInt32(b[4])
        let isRGBA = b[5] == 1
        let ew = Int(b[6]) << 8 | Int(b[7])
        let eh = Int(b[8]) << 8 | Int(b[9])
        guard ew > 0, eh > 0, n >= 10 + ew * 2 * eh * 4 else { return }
        handlePixels(p + 10, poseId: poseId, isRGBA: isRGBA, ew: ew, eh: eh)
    }

    private func handlePixels(_ srcPtr: UnsafeRawPointer, poseId: UInt32, isRGBA: Bool, ew: Int, eh: Int, quads: [Foveator.GameQuad] = [], gpu: (MTLBuffer, Int)? = nil) {
        guard ew > 0, eh > 0 else { return }
        frames += 1
        if frames == 1 { print("[juego] Primer cuadro recibido: \(ew)x\(eh) por ojo, \(isRGBA ? "RGBA" : "BGRA") · GPU \(Foveator.shared != nil ? "lista" : "NO disponible") · se manda \(W)x\(H)") }
        shared.markGameFrame()
        guard shared.beginFrame() else { Perf.shared.addDrop(); return }   // el codificador está atrasado: descartar
        let start = DispatchTime.now().uptimeNanoseconds
        // Modo Centinela: el juego se ve en la ventana de la Mac tal como lo dibujó (sin achicar ni agrandar: lo escala
        // la ventana en la GPU), y al visor apoyado va solo 1 cuadro de cada 20. Así la Mac libera cada cuadro enseguida
        // y el juego no tiene que descartar cuadros (eso eran los tirones).
        if Vigia.shared.on {
            if let f = vigiaFrame(srcPtr, isRGBA: isRGBA, ew: ew, eh: eh) { Vigia.shared.show(f) }
            vigiaSkip += 1
            if vigiaSkip % 20 != 0 { shared.endFrame(); return }
        }
        if let fv = Foveator.shared, FOVEA.on || ew != EYE || eh != SH || GameProfiles.shared.activeK < 0.999 || !quads.isEmpty || gpu != nil {
            var po: CVPixelBuffer?
            CVPixelBufferPoolCreatePixelBuffer(nil, pool, &po)
            guard let pb = po, fv.warpPixels(srcPtr, ew: ew, eh: eh, isRGBA: isRGBA, rects: eyeRects(), into: pb, quads: quads, gpu: gpu) else { shared.endFrame(); return }
            Perf.shared.addMac(Double(DispatchTime.now().uptimeNanoseconds - start) / 1e6)
            let fn = shared.nextFrameNo(), key = shared.takeKey(), enc = encoder
            hold(pb, poseId); encodeQueue.async { enc.encode(pb, frameNo: fn, poseId: poseId, forceKey: key, start: start) }
            return
        }
        var pbOut: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pbOut)
        guard let pb = pbOut else { shared.endFrame(); return }
        CVPixelBufferLockBaseAddress(pb, [])
        let dstBase = CVPixelBufferGetBaseAddress(pb)!
        let dstBPR = CVPixelBufferGetBytesPerRow(pb)
        let srcBPR = ew * 2 * 4
        let src = UnsafeMutableRawPointer(mutating: srcPtr)
        if ew == EYE && eh == H {
            for y in 0..<H { _ = memcpy(dstBase + y * dstBPR, src + y * srcBPR, W * 4) }
        } else {
            for e in 0..<2 {
                var s = vImage_Buffer(data: src + e * ew * 4, height: vImagePixelCount(eh), width: vImagePixelCount(ew), rowBytes: srcBPR)
                var d = vImage_Buffer(data: dstBase + e * EYE * 4, height: vImagePixelCount(H), width: vImagePixelCount(EYE), rowBytes: dstBPR)
                _ = vImageScale_ARGB8888(&s, &d, nil, vImage_Flags(kvImageNoFlags))
            }
        }
        if isRGBA {
            var d1 = vImage_Buffer(data: dstBase, height: vImagePixelCount(H), width: vImagePixelCount(W), rowBytes: dstBPR)
            var d2 = d1
            let map: [UInt8] = [2, 1, 0, 3]
            _ = vImagePermuteChannels_ARGB8888(&d1, &d2, map, vImage_Flags(kvImageNoFlags))
        }
        let nowNs = DispatchTime.now().uptimeNanoseconds
        if nowNs - lastSnapshot > 5_000_000_000 {
            lastSnapshot = nowNs
            saveSnapshot(dstBase, dstBPR)
        }
        CVPixelBufferUnlockBaseAddress(pb, [])
        Perf.shared.addMac(Double(DispatchTime.now().uptimeNanoseconds - start) / 1e6)
        let fn = shared.nextFrameNo(), key = shared.takeKey(), enc = encoder
        hold(pb, poseId); encodeQueue.async { enc.encode(pb, frameNo: fn, poseId: poseId, forceKey: key, start: start) }
    }
    /// Cuadro del juego que ya está en la GPU (copia directa)
    private func handleTexture(_ tex: MTLTexture, poseId: UInt32, ew: Int, eh: Int) {
        guard ew > 0, eh > 0, let fv = Foveator.shared else { return }
        frames += 1
        if frames == 1 { print("[juego] Primer cuadro recibido: \(ew)x\(eh) por ojo, copia directa en la GPU · se manda \(W)x\(H)") }
        shared.markGameFrame()
        guard shared.beginFrame() else { Perf.shared.addDrop(); return }   // el codificador está atrasado: descartar
        let start = DispatchTime.now().uptimeNanoseconds
        var po: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &po)
        guard let pb = po, fv.warpTexture(tex, ew: ew, eh: eh, rects: eyeRects(), into: pb) else { shared.endFrame(); return }
        Perf.shared.addMac(Double(DispatchTime.now().uptimeNanoseconds - start) / 1e6)
        let fn = shared.nextFrameNo(), key = shared.takeKey(), enc = encoder
        hold(pb, poseId); encodeQueue.async { enc.encode(pb, frameNo: fn, poseId: poseId, forceKey: key, start: start) }
    }
    private var vigiaSkip = 0
    private var vigiaPool: CVPixelBufferPool?, vigiaPoolW = 0, vigiaPoolH = 0
    /// La imagen del juego a su tamaño (los dos ojos lado a lado), para la ventana del Modo Centinela: solo copiar filas
    private func vigiaFrame(_ srcPtr: UnsafeRawPointer, isRGBA: Bool, ew: Int, eh: Int) -> CVPixelBuffer? {
        if vigiaPool == nil || vigiaPoolW != ew * 2 || vigiaPoolH != eh {
            let a: [String: Any] = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
                                    kCVPixelBufferWidthKey as String: ew * 2, kCVPixelBufferHeightKey as String: eh,
                                    kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any]()]
            var p: CVPixelBufferPool?
            CVPixelBufferPoolCreate(nil, nil, a as CFDictionary, &p)
            vigiaPool = p; vigiaPoolW = ew * 2; vigiaPoolH = eh
        }
        guard let vp = vigiaPool else { return nil }
        var po: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, vp, &po)
        guard let pb = po else { return nil }
        CVPixelBufferLockBaseAddress(pb, [])
        let dst = CVPixelBufferGetBaseAddress(pb)!, dbpr = CVPixelBufferGetBytesPerRow(pb), sbpr = ew * 2 * 4
        if dbpr == sbpr { memcpy(dst, srcPtr, sbpr * eh) }
        else { for y in 0..<eh { memcpy(dst + y * dbpr, srcPtr + y * sbpr, sbpr) } }
        if isRGBA {
            var d1 = vImage_Buffer(data: dst, height: vImagePixelCount(eh), width: vImagePixelCount(ew * 2), rowBytes: dbpr)
            var d2 = d1
            let map: [UInt8] = [2, 1, 0, 3]
            _ = vImagePermuteChannels_ARGB8888(&d1, &d2, map, vImage_Flags(kvImageNoFlags))
        }
        CVPixelBufferUnlockBaseAddress(pb, [])
        return pb
    }

    /// Dónde va la imagen de cada ojo dentro de la imagen completa: todo, o (con el campo de visión recortado) la parte del
    /// centro óptico de cada lente que el juego dibujó. Sale de la vista de cada ojo que manda el Quest (tangentes).
    private func eyeRects() -> [SIMD4<Float>] {
        let k = Float(GameProfiles.shared.activeK)
        let xr = shared.xr
        return (0..<2).map { e -> SIMD4<Float> in
            let b = 1 + e * 11
            guard k < 0.999, xr.count > b + 10 else { return SIMD4<Float>(0, 0, 1, 1) }
            let tl = xr[b + 7], tr = xr[b + 8], td = xr[b + 9], tu = xr[b + 10]
            guard tr - tl > 0.1, tu - td > 0.1 else { return SIMD4<Float>(0, 0, 1, 1) }
            let cx = -tl / (tr - tl), cy = tu / (tu - td)
            return SIMD4<Float>(cx * (1 - k), cy * (1 - k), k, k)
        }
    }

}

// MARK: - Cámara de la Mac: seguimiento de la cabeza (Modo Centinela)
// Con la cara que ve la cámara (Vision) se calcula cuánto te moviste desde que se centró: de costado, arriba/abajo,
// hacia la pantalla, y hacia dónde girás la cabeza. Se manda al Quest ~30 veces por segundo.
/// Filtro «One Euro»: quieto cuando no te movés (saca el temblor), rápido cuando te movés de verdad
struct OneEuro {
    var minCutoff: Double, beta: Double
    private var x: Double?, dx = 0.0, t = 0.0
    init(minCutoff: Double, beta: Double) { self.minCutoff = minCutoff; self.beta = beta }
    private static func alpha(_ cutoff: Double, _ dt: Double) -> Double { let tau = 1 / (2 * .pi * cutoff); return 1 / (1 + tau / dt) }
    mutating func reset() { x = nil }
    mutating func f(_ v: Double, _ now: Double) -> Double {
        guard let px = x else { x = v; t = now; return v }
        let dt = max(1e-3, now - t); t = now
        let a = OneEuro.alpha(1.0, dt)
        dx += ((v - px) / dt - dx) * a
        let cutoff = minCutoff + beta * abs(dx)
        let nx = px + (v - px) * OneEuro.alpha(cutoff, dt)
        x = nx
        return nx
    }
}

final class HeadCam: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    static let shared = HeadCam()
    private var session: AVCaptureSession?
    private(set) var preview: AVCaptureVideoPreviewLayer?
    private let q = DispatchQueue(label: "headcam", qos: .userInteractive)
    var send: (String) -> Void = { _ in }
    private var base: (cx: Double, cy: Double, w: Double, yaw: Double, pitch: Double)?
    private var wantRecenter = true
    // filtros: la posición de la cara (centro y tamaño) y el giro
    private var fx = OneEuro(minCutoff: 0.6, beta: 4), fy = OneEuro(minCutoff: 0.6, beta: 4), fw = OneEuro(minCutoff: 0.25, beta: 1)
    private var fyaw = OneEuro(minCutoff: 0.4, beta: 0.8), fpitch = OneEuro(minCutoff: 0.4, beta: 0.8)
    var onPreview: (AVCaptureVideoPreviewLayer) -> Void = { _ in }
    private var lastFace = Date.distantPast
    private var running = false

    func start() {
        guard !running else { return }
        running = true
        wantRecenter = true
        let go = { [self] in
            q.async { [self] in
                guard session == nil else { session?.startRunning(); return }
                guard let dev = AVCaptureDevice.default(for: .video), let input = try? AVCaptureDeviceInput(device: dev) else {
                    print("[vigía] Sin cámara: la cabeza sigue a los controles"); return
                }
                let s = AVCaptureSession()
                s.sessionPreset = .vga640x480
                guard s.canAddInput(input) else { return }
                s.addInput(input)
                let out = AVCaptureVideoDataOutput()
                out.alwaysDiscardsLateVideoFrames = true
                out.setSampleBufferDelegate(self, queue: q)
                if s.canAddOutput(out) { s.addOutput(out) }
                session = s
                DispatchQueue.main.sync {
                    let pl = AVCaptureVideoPreviewLayer(session: s)
                    pl.videoGravity = .resizeAspectFill
                    if let c = pl.connection, c.isVideoMirroringSupported { c.automaticallyAdjustsVideoMirroring = false; c.isVideoMirrored = true }
                    preview = pl
                    onPreview(pl)
                }
                s.startRunning()
                print("[vigía] Cámara de la Mac: siguiendo tu cabeza")
            }
        }
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized: go()
        case .notDetermined: AVCaptureDevice.requestAccess(for: .video) { ok in if ok { go() } else { print("[vigía] Sin permiso de cámara") } }
        default: print("[vigía] Sin permiso de cámara (Ajustes del Sistema › Privacidad › Cámara › VRmin Host)")
        }
    }
    func stop() {
        running = false
        q.async { [self] in session?.stopRunning() }
        send("{\"type\":\"vigiahead\",\"ok\":false}")
    }
    func recenter() { q.async { [self] in wantRecenter = true; fx.reset(); fy.reset(); fw.reset(); fyaw.reset(); fpitch.reset() } }

    /// Muñecas vistas por la cámara (posición en la imagen, de 0 a 1, y cuánta confianza): el Quest aprende cómo
    /// se corresponden con la posición de los controles mientras los ve, y las usa cuando deja de verlos
    private var lw = (OneEuro(minCutoff: 1.0, beta: 6), OneEuro(minCutoff: 1.0, beta: 6)), rw = (OneEuro(minCutoff: 1.0, beta: 6), OneEuro(minCutoff: 1.0, beta: 6))
    private func sendWrists(_ o: VNHumanBodyPoseObservation?) {
        let now = CACurrentMediaTime()
        func pt(_ j: VNHumanBodyPoseObservation.JointName, _ f: inout (OneEuro, OneEuro)) -> (Double, Double, Double) {
            guard let o, let p = try? o.recognizedPoint(j), p.confidence > 0.2 else { return (0, 0, 0) }
            return (f.0.f(Double(p.location.x), now), f.1.f(Double(p.location.y), now), Double(p.confidence))
        }
        let l = pt(.leftWrist, &lw), r = pt(.rightWrist, &rw)
        send(String(format: "{\"type\":\"vigiahands\",\"lx\":%.4f,\"ly\":%.4f,\"lc\":%.2f,\"rx\":%.4f,\"ry\":%.4f,\"rc\":%.2f}",
                    l.0, l.1, l.2, r.0, r.1, r.2))
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sb: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard running, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        let req = VNDetectFaceRectanglesRequest()
        if #available(macOS 12.0, *) { req.revision = VNDetectFaceRectanglesRequestRevision3 }
        // También las muñecas (el cuerpo): ayudan al visor cuando no ve un control
        let body = VNDetectHumanBodyPoseRequest()
        try? VNImageRequestHandler(cvPixelBuffer: pb, options: [:]).perform([req, body])
        sendWrists(body.results?.first)
        // la cara más grande (la más cerca)
        guard let f = (req.results ?? []).max(by: { $0.boundingBox.width < $1.boundingBox.width }) else {
            if Date().timeIntervalSince(lastFace) > 0.5 { send("{\"type\":\"vigiahead\",\"ok\":false}") }
            return
        }
        lastFace = Date()
        let bb = f.boundingBox
        let now = CACurrentMediaTime()
        let cx = fx.f(Double(bb.midX), now), cy = fy.f(Double(bb.midY), now), w = fw.f(Double(bb.width), now)
        var yawRaw = 0.0, pitchRaw = 0.0
        if #available(macOS 12.0, *) { yawRaw = fyaw.f(f.yaw?.doubleValue ?? 0, now); pitchRaw = fpitch.f(f.pitch?.doubleValue ?? 0, now) }
        // Al centrar se toma cómo te ve la cámara mirando la pantalla del juego: si la cámara está a un costado
        // (en diagonal), ese giro pasa a ser «adelante»
        if wantRecenter || base == nil { base = (cx, cy, w, yawRaw, pitchRaw); wantRecenter = false }
        guard let b = base else { return }
        // Cámara de ≈ 70° de ancho: una cara de 15 cm ocupa 0,107/distancia del ancho de la imagen
        let dist = 0.107 / max(0.03, w), dist0 = 0.107 / max(0.03, b.w)
        let cdx = -(cx - b.cx) * 1.4 * dist         // a la derecha, visto desde la cámara (la imagen está al revés que vos)
        let dy = (cy - b.cy) * 0.79 * dist          // arriba
        // hacia la cámara (sale del tamaño de la cara, lo más ruidoso: zona muerta de 5 cm y a la mitad)
        let rz = dist0 - dist
        let cdz = (abs(rz) < 0.05 ? 0 : rz - (rz > 0 ? 0.05 : -0.05)) * 0.5
        // de la cámara a tu cuerpo: girado lo que estabas girado respecto de la cámara al centrar
        let th = max(-1.1, min(1.1, b.yaw)), c = cos(th), sn = sin(th)
        let dx = cdx * c + cdz * sn, dz = -cdx * sn + cdz * c
        // zona muerta: los movimientos muy chicos (temblor) no cuentan
        func dead(_ v: Double, _ z: Double) -> Double { abs(v) < z ? 0 : v - (v > 0 ? z : -z) }
        // (la distancia sale del tamaño de la cara, que es lo más ruidoso: se usa a la mitad)
        // Más fuerza que lo medido: sentado o parado frente a la Mac te movés menos que en el juego (agacharte, esquivar)
        let gainX = 2.0, gainY = 2.5, gainZ = 1.5
        // Solo la posición (agacharte, inclinarte, esquivar): hacia dónde girás la cabeza NO mueve la vista del juego,
        // porque mirás la pantalla de la Mac (si la vista te siguiera, se iría para el costado)
        send(String(format: "{\"type\":\"vigiahead\",\"ok\":true,\"dx\":%.3f,\"dy\":%.3f,\"dz\":%.3f,\"yaw\":0,\"pitch\":0}",
                    max(-1.5, min(1.5, dead(dx, 0.015) * gainX)), max(-1.2, min(0.6, dead(dy, 0.015) * gainY)), max(-1.0, min(1.0, dz * gainZ))))
    }
}

// MARK: - Modo Centinela
// El visor queda apoyado en el escritorio mirándote y sigue los controles; el juego se ve acá, a pantalla completa
// (el ojo izquierdo de lo que dibuja el juego), con indicaciones encima. Esc o «Salir» lo terminan.
final class VigiaWindow: NSWindow {
    var onEsc: () -> Void = {}
    override var canBecomeKey: Bool { true }
    override func keyDown(with e: NSEvent) { if e.keyCode == 53 { onEsc() } else { super.keyDown(with: e) } }
}

final class Vigia: NSObject, NSWindowDelegate {
    static let shared = Vigia()
    /// Cerrar la ventana (botón rojo) = salir del Modo Centinela
    func windowShouldClose(_ sender: NSWindow) -> Bool { exitPressed(); return false }
    var lastPoseAt = Date.distantPast   // (el visor está mandando su posición: los juegos lo van a encontrar)
    private(set) var on = false { didSet { if on != oldValue { onChange?() } } }
    var onChange: (() -> Void)?   // (el enlace con el juego le vuelve a mandar los ajustes al runtime)
    private var win: VigiaWindow?
    private var image = CALayer()
    private var text = CATextLayer()
    private var held: CVPixelBuffer?
    private let lock = NSLock()
    private var pending: CVPixelBuffer?
    private var scheduled = false
    var onExit: () -> Void = {}

    func set(_ v: Bool) {
        DispatchQueue.main.async { [self] in
            guard v != on else { return }
            on = v
            if v { open() } else { close() }
            print(v ? "[vigía] Modo Centinela: el juego se ve en la Mac" : "[vigía] Modo Centinela terminado")
        }
        if !v { lock.lock(); pending = nil; lock.unlock() }
    }

    func say(_ code0: String) {
        let t: String
        // sin juego todavía: el aviso es abrir un juego
        let code = (code0 == "sincalibrar" && !hadFrames) ? "esperando" : code0
        if code == "listo" || code == "recentrado" { HeadCam.shared.recenter() }
        switch code {
        case "calibrar": t = tr("Centrando en 3 segundos: mirá la pantalla donde ves el juego, con los dos controles delante del pecho", "Centering in 3 seconds: look at the screen where you see the game, with both controllers in front of your chest")
        case "listo": t = tr("¡Listo! Centrado: adelante en el juego es hacia el visor", "Done! Centered: forward in the game is toward the headset")
        case "sincontroles": t = tr("El visor no ve los dos controles: ponete delante del visor, a 1–2 m, con los controles a la vista (si alguno no responde, apretá un botón)", "The headset can't see both controllers: stand in front of it, 1–2 m away, with the controllers in view (if one doesn't respond, press a button)")
        case "sincalibrar": t = tr("Si el juego mira para otro lado o te ves desde afuera: mirá al visor con los controles delante del pecho y mantené el botón de menú izquierdo (centra y calibra)", "If the game faces the wrong way or you see yourself from outside: look at the headset with the controllers in front of your chest and hold the left menu button (centers and calibrates)")
        case "sintracking": t = tr("El visor perdió el seguimiento (no sabe dónde está): asegurate de que vea la habitación con luz; si sigue, levantalo, mové un poco la cabeza con él y volvé a apoyarlo", "The headset lost tracking: make sure it sees a lit room; if it continues, pick it up, move it around a bit and set it down again")
        case "dormidoizq": t = tr("El control izquierdo parece dormido: apretá cualquier botón para despertarlo", "The left controller seems asleep: press any button to wake it")
        case "dormidoder": t = tr("El control derecho parece dormido: apretá cualquier botón para despertarlo", "The right controller seems asleep: press any button to wake it")
        case "esperando": t = tr("Modo Centinela listo. Abrí un juego desde VRmin Host (Jugar): se va a ver acá", "Sentinel Mode ready. Open a game from VRmin Host (Play): it will show here")
        case "recentrado": t = tr("Centrado: adelante en el juego es hacia el visor", "Centered: forward in the game is toward the headset")
        default: t = code
        }
        DispatchQueue.main.async { [self] in
            text.string = t
            text.opacity = 1
            let id = UUID(); lastSayId = id
            DispatchQueue.main.asyncAfter(deadline: .now() + (code == "listo" ? 4 : 8)) { [self] in if lastSayId == id && code != "calibrar" && code != "esperando" { text.opacity = 0.0 } }
        }
    }
    private var lastSayId = UUID()

    private func open() {
        guard let scr = NSScreen.main else { return }
        hadFrames = false; launching = ""; gamesKey = ""
        // Ventana normal (se mueve a otro monitor, se agranda o se pone a pantalla completa con el botón verde)
        let size = NSSize(width: min(1440, scr.visibleFrame.width * 0.85), height: min(1440, scr.visibleFrame.width * 0.85) / 1.54)
        let w = VigiaWindow(contentRect: NSRect(origin: .zero, size: size), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        w.title = tr("VRmin · Modo Centinela", "VRmin · Sentinel Mode")
        w.backgroundColor = .black
        w.isReleasedWhenClosed = false
        w.collectionBehavior = [.fullScreenPrimary]
        w.delegate = self
        w.center()
        w.setFrameAutosaveName("VRminVigia")   // (recuerda dónde la dejaste)
        w.onEsc = { [weak self] in self?.exitPressed() }
        let v = NSView(frame: NSRect(origin: .zero, size: size))
        v.wantsLayer = true
        v.layer?.backgroundColor = NSColor.black.cgColor
        image = CALayer()
        image.frame = v.bounds
        // El juego dibuja cada ojo cuadrado pero con un campo de visión de pantalla (≈ 1,54:1): se estira a toda la
        // pantalla para que quede con sus proporciones reales (antes se veía estirado hacia arriba)
        image.contentsGravity = .resize
        image.minificationFilter = .trilinear; image.magnificationFilter = .linear
        image.contentsRect = CGRect(x: 0, y: 0, width: 0.5, height: 1)   // ojo izquierdo
        image.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
        v.layer?.addSublayer(image)
        text = CATextLayer()
        text.contentsScale = scr.backingScaleFactor
        text.fontSize = 22
        text.foregroundColor = NSColor.white.cgColor
        text.backgroundColor = NSColor(white: 0, alpha: 0.6).cgColor
        text.cornerRadius = 10
        text.isWrapped = true
        text.alignmentMode = .center
        text.frame = CGRect(x: size.width * 0.1, y: 24, width: size.width * 0.8, height: 64)
        text.autoresizingMask = [.layerWidthSizable]
        v.layer?.addSublayer(text)
        let b = NSButton(title: tr("Salir del Modo Centinela (Esc)", "Exit Sentinel Mode (Esc)"), target: self, action: #selector(exitButton))
        b.bezelStyle = .rounded
        b.sizeToFit()
        b.frame.origin = CGPoint(x: size.width - b.frame.width - 16, y: size.height - b.frame.height - 12)
        b.autoresizingMask = [.minXMargin, .minYMargin]
        v.addSubview(b)
        // Lista de juegos (mientras todavía no hay juego): se elige acá mismo, con el mouse
        let list = NSStackView()
        list.orientation = .vertical
        list.alignment = .centerX
        list.spacing = 10
        list.translatesAutoresizingMaskIntoConstraints = false
        v.addSubview(list)
        NSLayoutConstraint.activate([list.centerXAnchor.constraint(equalTo: v.centerXAnchor), list.centerYAnchor.constraint(equalTo: v.centerYAnchor)])
        gamesView = list
        refreshGames()
        // Cámara de la Mac: sigue tu cabeza (de verdad) mientras el visor apoyado sigue los controles
        let cam = HeadCam.shared
        let attach: (AVCaptureVideoPreviewLayer) -> Void = { [weak v] pl in
            guard let v else { return }
            pl.removeFromSuperlayer()
            pl.frame = CGRect(x: 16, y: 100, width: 240, height: 180)
            pl.autoresizingMask = [.layerMaxXMargin, .layerMaxYMargin]
            pl.cornerRadius = 10; pl.masksToBounds = true
            pl.zPosition = 10
            pl.isHidden = UserDefaults.standard.bool(forKey: "vigiaCamHidden")
            v.layer?.addSublayer(pl)
        }
        cam.onPreview = attach
        if let pl = cam.preview { attach(pl) }
        cam.start()
        let cb = NSButton(title: UserDefaults.standard.bool(forKey: "vigiaCamHidden") ? tr("Mostrar cámara", "Show camera") : tr("Ocultar cámara", "Hide camera"), target: self, action: #selector(toggleCam(_:)))
        cb.bezelStyle = .rounded
        cb.sizeToFit()
        cb.frame.origin = CGPoint(x: 16, y: 64)
        cb.autoresizingMask = [.maxXMargin, .maxYMargin]
        v.addSubview(cb)
        w.contentView = v
        w.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        win = w
        say("esperando")
        proximity(true)
        lastFrameAt = Date(); hadFrames = false
        watch?.invalidate()
        watch = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            guard let self, self.on else { return }
            // Se cerró el juego (sin cuadros hace 6 s): se termina el Modo Centinela y el sensor vuelve a la normalidad
            // (solo si ya hubo un juego: con «Solo Modo Centinela» se puede empezar antes de abrir el juego)
            // Se cerró el juego: el Modo Centinela sigue (se sale a mano); vuelve la lista de juegos
            if self.hadFrames && Date().timeIntervalSince(self.lastFrameAt) > 5 {
                self.hadFrames = false; self.launching = ""; self.gamesKey = ""
                self.image.contents = nil; self.held = nil
                self.gamesView?.isHidden = false
            }
            // Mientras el juego sigue abierto: se vuelve a avisar cada minuto (por si el visor se reinició)
            if Date().timeIntervalSince(self.proxAt) > 30 { self.proximity(true) }
            self.refreshGames()
        }
    }
    private var watch: Timer?
    private var gamesView: NSStackView?
    var listGames: () -> [Lobby.GameEntry] = { [] }   // (se conectan abajo, cuando existe la sala)
    var launchGame: (InMsg) -> Void = { _ in }
    private var gamesKey = ""
    private var launching = ""

    /// Lista de juegos de la biblioteca: un botón por juego (se esconde cuando ya se ve el juego)
    private func refreshGames() {
        guard let list = gamesView else { return }
        if hadFrames { list.isHidden = true; return }
        renderQueue.async {
            let gs = self.listGames()
            DispatchQueue.main.async { [self] in
                let ready = Date().timeIntervalSince(lastPoseAt) < 2
                let key = gs.map { $0.id }.joined(separator: ",") + launching + "\(ready)"
                guard key != gamesKey, let list = gamesView else { return }
                gamesKey = key
                list.arrangedSubviews.forEach { $0.removeFromSuperview() }
                let t = NSTextField(labelWithString: !launching.isEmpty ? tr("Abriendo \(launching)…", "Opening \(launching)…")
                    : !ready ? tr("Esperando al visor…", "Waiting for the headset…") : gs.isEmpty ? tr("Todavía no hay juegos: instalalos desde Steam", "No games yet: install them from Steam") : tr("Elegí un juego", "Pick a game"))
                t.font = .systemFont(ofSize: 30, weight: .bold); t.textColor = .white
                list.addArrangedSubview(t)
                if !ready && launching.isEmpty {
                    // sin el visor mandando su posición, el juego abriría como si no hubiera visor (negro o sin VR)
                    let h = NSTextField(wrappingLabelWithString: tr("Los juegos se pueden abrir cuando el visor esté listo. Si tarda, tocá «Empezar» en VRmin Host otra vez o apretá una vez el botón de encendido del visor.", "Games can be opened once the headset is ready. If it takes long, click “Start” in VRmin Host again or press the headset's power button once."))
                    h.textColor = .lightGray; h.alignment = .center; h.preferredMaxLayoutWidth = 520
                    list.addArrangedSubview(h)
                }
                guard launching.isEmpty, ready else { return }
                // En orden alfabético, con la imagen de cada juego (de Steam), de a 3 por fila
                let sorted = gs.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
                var row: NSStackView?
                for (i, g) in sorted.enumerated() {
                    if i % 3 == 0 {
                        let r = NSStackView(); r.orientation = .horizontal; r.spacing = 14; r.alignment = .top
                        list.addArrangedSubview(r); row = r
                    }
                    let b = NSButton(title: g.name, target: self, action: #selector(pickGame(_:)))
                    b.isBordered = false
                    b.imagePosition = .imageAbove
                    b.font = .systemFont(ofSize: 14, weight: .semibold)
                    b.contentTintColor = .white
                    b.attributedTitle = NSAttributedString(string: g.name, attributes: [.foregroundColor: NSColor.white, .font: NSFont.systemFont(ofSize: 14, weight: .semibold)])
                    b.identifier = NSUserInterfaceItemIdentifier(g.id)
                    b.image = gameImage(g.id) { [weak b] img in b?.image = img }
                    b.widthAnchor.constraint(equalToConstant: 240).isActive = true
                    row?.addArrangedSubview(b)
                }
            }
        }
    }
    /// Imagen del juego (la cabecera de Steam, 460×215): de la memoria, o se baja una vez
    private var imgCache: [String: NSImage] = [:]
    private func gameImage(_ id: String, _ done: @escaping (NSImage) -> Void) -> NSImage {
        let size = NSSize(width: 240, height: 112)
        if let c = imgCache[id] { return c }
        let ph = NSImage(size: size, flipped: false) { r in NSColor(white: 1, alpha: 0.08).setFill(); NSBezierPath(roundedRect: r, xRadius: 8, yRadius: 8).fill(); return true }
        if let u = URL(string: "https://cdn.cloudflare.steamstatic.com/steam/apps/\(id)/header.jpg") {
            URLSession.shared.dataTask(with: u) { [weak self] d, _, _ in
                guard let d, let src = NSImage(data: d) else { return }
                let img = NSImage(size: size, flipped: false) { r in
                    NSBezierPath(roundedRect: r, xRadius: 8, yRadius: 8).addClip(); src.draw(in: r); return true }
                DispatchQueue.main.async { self?.imgCache[id] = img; done(img) }
            }.resume()
        }
        return ph
    }
    @objc private func pickGame(_ b: NSButton) {
        guard let id = b.identifier?.rawValue,
              let m = try? JSONDecoder().decode(InMsg.self, from: Data("{\"type\":\"launch\",\"game\":\"\(id)\"}".utf8)) else { return }
        launching = b.title
        refreshGames()
        renderQueue.async { self.launchGame(m) }
        // si en un rato no aparece el juego, vuelve la lista

    }
    private var lastFrameAt = Date()
    private var hadFrames = false
    private var proxAt = Date.distantPast

    @objc private func toggleCam(_ b: NSButton) {
        guard let pl = HeadCam.shared.preview else { return }
        pl.isHidden.toggle()
        UserDefaults.standard.set(pl.isHidden, forKey: "vigiaCamHidden")
        b.title = pl.isHidden ? tr("Mostrar cámara", "Show camera") : tr("Ocultar cámara", "Hide camera")
        b.sizeToFit()
    }
    private func close() {
        HeadCam.shared.stop()
        HeadCam.shared.preview?.removeFromSuperlayer()
        watch?.invalidate(); watch = nil
        win?.orderOut(nil); win = nil
        gamesView = nil; launching = ""; gamesKey = ""
        held = nil
        proximity(false)
    }

    @objc private func exitButton() { exitPressed() }
    private func exitPressed() { set(false); onExit() }

    /// Cuadro nuevo del juego (desde el hilo del juego): se muestra en el próximo paso del hilo principal
    func show(_ pb: CVPixelBuffer) {
        lock.lock(); pending = pb; let go = !scheduled; scheduled = true; lock.unlock()
        guard go else { return }
        DispatchQueue.main.async { [self] in
            lock.lock(); let p = pending; pending = nil; scheduled = false; lock.unlock()
            lastFrameAt = Date()
            if !hadFrames { hadFrames = true; gamesView?.isHidden = true }
            guard on, let p = p, let surf = CVPixelBufferGetIOSurface(p)?.takeUnretainedValue() else { return }
            CATransaction.begin(); CATransaction.setDisableActions(true)
            image.contents = surf
            CATransaction.commit()
            held = p   // (se guarda hasta el próximo, para que el codificador no lo reuse mientras se ve)
        }
    }

    /// Si el Quest está conectado por cable con el modo desarrollador y adb autorizado: que no se apague la pantalla
    /// al sacarte el visor (sin modo desarrollador no hay forma: hay que tapar el sensor)
    private func proximity(_ keepOn: Bool) {
        // (primero el que instala VRmin Host con un botón: <datos>/herramientas/platform-tools/adb)
        let data = ProcessInfo.processInfo.environment["QVB_DATA"] ?? (NSHomeDirectory() + "/Library/Application Support/QuestVRBridge")
        let cands = [data + "/herramientas/platform-tools/adb", "/opt/homebrew/bin/adb", "/usr/local/bin/adb", NSHomeDirectory() + "/Library/Android/sdk/platform-tools/adb"]
        guard let adb = cands.first(where: { FileManager.default.isExecutableFile(atPath: $0) }) else {
            if keepOn && proxAt == .distantPast { print("[vigía] Sin adb: para que el visor no apague la pantalla, instalá las herramientas en VRmin Host (o tapá el sensor)") }
            proxAt = Date(); return
        }
        if keepOn { proxAt = Date() } else { proxAt = .distantPast }
        DispatchQueue.global(qos: .utility).async {
            let args = keepOn ? ["shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close"]
                              : ["shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.automation_disable"]
            let p = Process(); p.executableURL = URL(fileURLWithPath: adb); p.arguments = args
            p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
            do { try p.run(); p.waitUntilExit(); print("[vigía] adb: sensor de proximidad " + (keepOn ? "ignorado" : "normal") + " (código \(p.terminationStatus))") }
            catch { }
            if keepOn {   // (y que no se duerma: si el visor apagó la pantalla, los controles dejan de responder)
                let w = Process(); w.executableURL = URL(fileURLWithPath: adb); w.arguments = ["shell", "input", "keyevent", "KEYCODE_WAKEUP"]
                w.standardOutput = FileHandle.nullDevice; w.standardError = FileHandle.nullDevice
                try? w.run(); w.waitUntilExit()
            }
        }
    }
}

// MARK: - Sonido de los juegos (Wine) -> Quest
// Usa los "process taps" de Core Audio (macOS 14.2+): escucha solo los procesos de Wine,
// sin drivers virtuales. El sonido se manda como PCM de 16 bits en bloques de 20 ms.
func caRead<T>(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector, _ value: T,
               scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> T? {
    var addr = AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    var size = UInt32(MemoryLayout<T>.size)
    var v = value
    let err = withUnsafeMutablePointer(to: &v) { AudioObjectGetPropertyData(obj, &addr, 0, nil, &size, $0) }
    return err == noErr ? v : nil
}

@available(macOS 14.2, *)
final class AudioTap {
    private let link: Link
    private let gameMute: Bool
    private var muteMac: Bool
    private var global = false
    var deskActive: () -> Bool = { false }   // hay una ventana del Quest conectada mirando la Mac
    var gameActive: () -> Bool = { false }
    private let ioQueue = DispatchQueue(label: "audio-io", qos: .userInteractive)
    private let sendQueue = DispatchQueue(label: "audio-send", qos: .userInteractive)
    private var tapID = AudioObjectID(kAudioObjectUnknown)
    private var aggID = AudioObjectID(kAudioObjectUnknown)
    private var procID: AudioDeviceIOProcID?
    private var current: [AudioObjectID] = []
    private var rate: Double = 48000
    private var channels = 2
    private var interleaved = true
    // Acumulador (solo se usa en ioQueue)
    private var acc: [Int16] = []
    private var peak: Float = 0
    private let peakLock = NSLock()
    private var silentSince: UInt64 = 0
    private var warnedSilent = false
    private var sentChunks = 0

    var gameEnabled = true   // VR_AUDIO=0: el sonido de los juegos queda en la Mac
    init(link: Link, muteMac: Bool) { self.link = link; self.gameMute = muteMac; self.muteMac = muteMac }

    /// Procesos de Wine que usan audio (cualquier Wine: el del prototipo o el de GameHub)
    private func wineProcesses() -> [AudioObjectID] {
        let sys = AudioObjectID(kAudioObjectSystemObject)
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyProcessObjectList,
                                              mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(sys, &addr, 0, nil, &size) == noErr, size > 0 else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(sys, &addr, 0, nil, &size, &ids) == noErr else { return [] }
        var out: [AudioObjectID] = []
        for id in ids {
            guard let pid = caRead(id, kAudioProcessPropertyPID, pid_t(0)), pid > 0 else { continue }
            var buf = [CChar](repeating: 0, count: 4096)
            if proc_pidpath(pid, &buf, UInt32(buf.count)) > 0 {
                let path = String(cString: buf).lowercased()
                if path.contains("wine") { out.append(id) }
            }
        }
        return out.sorted()
    }

    func start() {
        Thread.detachNewThread { [self] in
            var lastKey = ""
            while true {
                // Con un juego: solo el sonido de Wine (según el ajuste de juegos).
                // Sin juego, si se eligió escuchar el escritorio en el visor: todo el sonido de la Mac.
                let wantGlobal = !gameActive() && DESK_AUDIO != 0 && deskActive()
                let procs = wantGlobal ? [] : (gameEnabled && !Vigia.shared.on ? wineProcesses() : [])   // (Modo Centinela: el sonido queda en la Mac)
                let mute = wantGlobal ? DESK_AUDIO == 1 : gameMute
                let key = "\(wantGlobal)|\(mute)|\(procs)"
                if key != lastKey {
                    lastKey = key
                    stopTap()
                    current = procs; global = wantGlobal; muteMac = mute
                    if wantGlobal || !procs.isEmpty { startTap(procs) }
                }
                checkSilence()
                sleep(1)
            }
        }
    }

    private func stopTap() {
        if aggID != kAudioObjectUnknown {
            AudioDeviceStop(aggID, procID)
            if let p = procID { AudioDeviceDestroyIOProcID(aggID, p) }
            AudioHardwareDestroyAggregateDevice(aggID)
        }
        if tapID != kAudioObjectUnknown { AudioHardwareDestroyProcessTap(tapID) }
        aggID = AudioObjectID(kAudioObjectUnknown); tapID = AudioObjectID(kAudioObjectUnknown); procID = nil
    }

    private func startTap(_ procs: [AudioObjectID]) {
        let desc = global ? CATapDescription(stereoGlobalTapButExcludeProcesses: []) : CATapDescription(stereoMixdownOfProcesses: procs)
        desc.uuid = UUID()
        desc.name = "QuestVRBridge"
        desc.isPrivate = true
        desc.muteBehavior = muteMac ? .mutedWhenTapped : .unmuted
        var tap = AudioObjectID(kAudioObjectUnknown)
        var err = AudioHardwareCreateProcessTap(desc, &tap)
        guard err == noErr else { print("[audio] No se pudo escuchar a Wine (error \(err))"); return }
        tapID = tap
        guard let fmt = caRead(tap, kAudioTapPropertyFormat, AudioStreamBasicDescription()) else {
            print("[audio] No se pudo leer el formato del sonido"); stopTap(); return
        }
        guard fmt.mFormatID == kAudioFormatLinearPCM, fmt.mFormatFlags & kAudioFormatFlagIsFloat != 0, fmt.mBitsPerChannel == 32 else {
            print("[audio] Formato de sonido inesperado (\(fmt.mFormatID) \(fmt.mBitsPerChannel) bits)"); stopTap(); return
        }
        rate = fmt.mSampleRate
        channels = max(1, Int(fmt.mChannelsPerFrame))
        interleaved = fmt.mFormatFlags & kAudioFormatFlagIsNonInterleaved == 0

        let sys = AudioObjectID(kAudioObjectSystemObject)
        guard let outDev = caRead(sys, kAudioHardwarePropertyDefaultSystemOutputDevice, AudioObjectID(kAudioObjectUnknown)),
              let uidCF = caRead(outDev, kAudioDevicePropertyDeviceUID, "" as CFString) else {
            print("[audio] No encontré la salida de sonido de la Mac"); stopTap(); return
        }
        let outUID = uidCF as String
        let agg: [String: Any] = [
            kAudioAggregateDeviceNameKey: "QuestVRBridge-Audio",
            kAudioAggregateDeviceUIDKey: UUID().uuidString,
            kAudioAggregateDeviceMainSubDeviceKey: outUID,
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceIsStackedKey: false,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceSubDeviceListKey: [[kAudioSubDeviceUIDKey: outUID]],
            kAudioAggregateDeviceTapListKey: [[kAudioSubTapDriftCompensationKey: true,
                                               kAudioSubTapUIDKey: desc.uuid.uuidString]],
        ]
        var aggOut = AudioObjectID(kAudioObjectUnknown)
        err = AudioHardwareCreateAggregateDevice(agg as CFDictionary, &aggOut)
        guard err == noErr else { print("[audio] No se pudo crear el dispositivo de captura (error \(err))"); stopTap(); return }
        aggID = aggOut
        // El sonido llega al ritmo de la salida de la Mac (el dispositivo principal), no al del formato del tap:
        // si la Mac está en 44,1 kHz y se avisaba 48 kHz, el Quest se quedaba sin sonido cada ≈ 2 s (un corte)
        if let nominal = caRead(aggOut, kAudioDevicePropertyNominalSampleRate, Float64(0)), nominal >= 8000, abs(nominal - rate) > 1 {
            print("[audio] La salida de la Mac va a \(Int(nominal)) Hz (el tap decía \(Int(rate)) Hz): se usa \(Int(nominal)) Hz")
            rate = nominal
        }
        ioQueue.sync { acc.removeAll(keepingCapacity: true) }
        err = AudioDeviceCreateIOProcIDWithBlock(&procID, aggID, ioQueue) { [weak self] _, inData, _, _, _ in
            self?.process(inData)
        }
        guard err == noErr else { print("[audio] Error preparando la captura (\(err))"); stopTap(); return }
        err = AudioDeviceStart(aggID, procID)
        guard err == noErr else { print("[audio] Error iniciando la captura (\(err))"); stopTap(); return }
        silentSince = DispatchTime.now().uptimeNanoseconds
        print("[audio] Enviando el sonido de Wine al Quest (\(procs.count) proceso(s), \(Int(rate)) Hz)"
              + (muteMac ? " · la Mac queda en silencio mientras tanto" : ""))
    }

    /// Corre en ioQueue: convierte a 16 bits estéreo intercalado y manda bloques de 20 ms
    private func process(_ inData: UnsafePointer<AudioBufferList>) {
        let abl = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: inData))
        guard abl.count > 0, let d0 = abl[0].mData else { return }
        var localPeak: Float = 0
        func put(_ l: Float, _ r: Float) {
            localPeak = max(localPeak, abs(l), abs(r))
            acc.append(Int16(max(-1, min(1, l)) * 32767))
            acc.append(Int16(max(-1, min(1, r)) * 32767))
        }
        if interleaved {
            let ch = channels
            let p = d0.assumingMemoryBound(to: Float.self)
            let frames = Int(abl[0].mDataByteSize) / (4 * ch)
            for i in 0..<frames { put(p[i * ch], ch > 1 ? p[i * ch + 1] : p[i * ch]) }
        } else {
            let pl = d0.assumingMemoryBound(to: Float.self)
            let pr = (abl.count > 1 ? abl[1].mData ?? d0 : d0).assumingMemoryBound(to: Float.self)
            let frames = Int(abl[0].mDataByteSize) / 4
            for i in 0..<frames { put(pl[i], pr[i]) }
        }
        peakLock.lock(); peak = max(peak, localPeak); peakLock.unlock()
        let chunk = Int(rate / 50) * 2           // 20 ms, 2 canales
        while acc.count >= chunk {
            var payload = Data([2, 0, 0, 0])
            var rBE = UInt32(rate).bigEndian
            payload.append(Data(bytes: &rBE, count: 4))
            acc.withUnsafeBufferPointer { payload.append(UnsafeBufferPointer(start: $0.baseAddress, count: chunk)) }
            acc.removeFirst(chunk)
            let link = self.link
            sentChunks += 1
            sendQueue.async { link.send(payload) }
        }
    }

    /// Si hay juego pero solo llega silencio, casi seguro falta el permiso de grabación de audio
    private func checkSilence() {
        guard aggID != kAudioObjectUnknown, !global else { return }
        peakLock.lock(); let p = peak; peak = 0; peakLock.unlock()
        let now = DispatchTime.now().uptimeNanoseconds
        if p > 0.0001 { silentSince = now; warnedSilent = false; return }
        if !warnedSilent && now - silentSince > 20_000_000_000 {
            warnedSilent = true
            print("[audio] Llevo 20 s recibiendo silencio de Wine. Si el juego debería sonar: Ajustes del Sistema >"
                  + " Privacidad y seguridad > Grabación de pantalla y audio del sistema > activá \"Terminal\" y reiniciá start.sh")
        }
    }
}

// MARK: - Sala de inicio (lo que se ve en el visor cuando no hay un juego abierto)
// Lista de juegos instalados (se apunta con el control y se aprieta el gatillo) y,
// opcionalmente, una pantalla con el escritorio de la Mac que se puede usar con el control.

let LOBBY_NOT_GAMES: Set<String> = ["228980", "250820", "1070560", "1391110", "1628350"]

struct LobbyQuad { var corners: [SIMD4<Float>]; let texture: MTLTexture }

@available(macOS 13.0, *)
final class DesktopCapture: NSObject, SCStreamOutput {
    private var stream: SCStream?
    private let lock = NSLock()
    private var latest: CVPixelBuffer?
    private(set) var bounds = CGRect(x: 0, y: 0, width: 1, height: 1)
    private let queue = DispatchQueue(label: "desktop-capture", qos: .userInteractive)
    var failed: String?
    var onFrame: ((CVPixelBuffer) -> Void)?
    var displayID: CGDirectDisplayID = CGMainDisplayID()
    private var tries = 0

    func start() {
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { [self] content, err in
            // Un monitor virtual recién creado tarda un momento en aparecer
            if let content, displayID != CGMainDisplayID(), !content.displays.contains(where: { $0.displayID == displayID }), tries < 8 {
                tries += 1
                DispatchQueue.global().asyncAfter(deadline: .now() + 0.5) { self.start() }
                return
            }
            guard let content,
                  let d = content.displays.first(where: { $0.displayID == displayID }) ?? content.displays.first else {
                failed = "Falta el permiso de grabación de pantalla"
                print("[escritorio] No pude capturar la pantalla: \(err?.localizedDescription ?? "sin pantallas"). " +
                      "Activá QuestVRBridge en Ajustes del Sistema > Privacidad y seguridad > Grabación de pantalla y audio del sistema.")
                return
            }
            let cfg = SCStreamConfiguration()
            // Resolución real de la pantalla (píxeles, no puntos), hasta 2560 de ancho para que el texto se lea bien
            let pixW = Double(CGDisplayCopyDisplayMode(d.displayID)?.pixelWidth ?? d.width * 2)
            let pixH = pixW * Double(d.height) / Double(max(d.width, 1))
            let scale = min(1.0, Double(DESK_MAX_W) / max(pixW, 1))
            cfg.width = max(2, Int(pixW * scale) & ~1)
            cfg.height = max(2, Int(pixH * scale) & ~1)
            cfg.pixelFormat = kCVPixelFormatType_32BGRA
            cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(DESK_FPS))
            cfg.showsCursor = true
            cfg.queueDepth = 4
            let s = SCStream(filter: SCContentFilter(display: d, excludingWindows: []), configuration: cfg, delegate: nil)
            do {
                try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
            } catch { print("[escritorio] Error: \(error.localizedDescription)"); return }
            bounds = CGDisplayBounds(d.displayID)
            stream = s
            s.startCapture { e in
                if let e { print("[escritorio] No arrancó la captura: \(e.localizedDescription)") }
                else { print("[escritorio] Mostrando el escritorio de la Mac en el visor") }
            }
        }
    }

    func stop() { stream?.stopCapture(completionHandler: nil); stream = nil; lock.lock(); latest = nil; lock.unlock() }

    var frame: CVPixelBuffer? { lock.lock(); defer { lock.unlock() }; return latest }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        lock.lock(); latest = pb; lock.unlock()
        onFrame?(pb)
    }
}

/// Codificador aparte para el escritorio (la app del Quest lo muestra como una pantalla nítida)
final class DeskEncoder {
    private var session: VTCompressionSession?
    private var w = 0, h = 0
    private var frameNo: Int64 = 0
    private let lock = NSLock()
    private var forceKey = true
    private let send: (Data) -> Void
    private let streamId: UInt32?
    private var bitrate: Int
    /// Cambia la calidad en vivo (sin cortar el video)
    func setBitrate(_ b: Int) {
        bitrate = b
        if let s = session { VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: b)) }
    }
    /// streamId nil: escritorio completo (paquete 7). Con número: una ventana de la Mac (paquete 8).
    init(send: @escaping (Data) -> Void, streamId: UInt32? = nil, bitrate: Int = 20_000_000) {
        self.send = send; self.streamId = streamId; self.bitrate = bitrate
    }
    func requestKey() { lock.lock(); forceKey = true; lock.unlock() }
    /// Cada imagen de una ventana (o pantalla) pasa también por acá, para el 3D de esa ventana
    static var depthHook: ((CVPixelBuffer, UInt32) -> Void)?

    private func create(_ width: Int, _ height: Int) {
        if let s = session { VTCompressionSessionInvalidate(s); session = nil }
        var s: VTCompressionSession?
        let spec = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true] as CFDictionary
        guard VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_H264,
                                         encoderSpecification: spec, imageBufferAttributes: nil, compressedDataAllocator: nil,
                                         outputCallback: nil, refcon: nil, compressionSessionOut: &s) == noErr, let s else { return }
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_High_AutoLevel)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        // 60 cuadros por segundo; un escritorio cambia poco entre cuadros, así que 20 Mbps alcanzan
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: bitrate))
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: NSNumber(value: DESK_FPS))
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: NSNumber(value: 120))
        VTCompressionSessionPrepareToEncodeFrames(s)
        session = s; w = width; h = height
        requestKey()
        if streamId == nil { print("[escritorio] Enviando el escritorio a la app del Quest: \(width)x\(height)") }
    }

    func encode(_ pb: CVPixelBuffer) {
        if let sid = streamId { Self.depthHook?(pb, sid) }
        let pw = CVPixelBufferGetWidth(pb), ph = CVPixelBufferGetHeight(pb)
        if session == nil || pw != w || ph != h { create(pw, ph) }
        guard let s = session else { return }
        frameNo += 1
        lock.lock(); let key = forceKey; forceKey = false; lock.unlock()
        let props: CFDictionary? = key ? [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary : nil
        let ww = UInt16(pw), hh = UInt16(ph), send = self.send, sid = streamId
        VTCompressionSessionEncodeFrame(s, imageBuffer: pb, presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()),
                                        duration: .invalid, frameProperties: props, infoFlagsOut: nil) { st, _, sb in
            guard st == noErr, let sb, let r = Encoder.toAnnexB(sb) else { return }
            var p: Data
            if let sid {
                p = Data([8]); var i = sid.bigEndian; p.append(Data(bytes: &i, count: 4)); p.append(r.1 ? 1 : 0)
            } else { p = Data([7, r.1 ? 1 : 0]) }
            var a = ww.bigEndian, b = hh.bigEndian
            p.append(Data(bytes: &a, count: 2)); p.append(Data(bytes: &b, count: 2))
            p.append(r.0)
            send(p)
        }
    }
}

/// Segundo monitor virtual: la Mac cree que tiene otra pantalla conectada (misma técnica que DeskPad o BetterDisplay,
/// con una función interna de macOS). Mientras exista, podés arrastrar ventanas a ella como a un monitor de verdad.
final class VirtualMonitor {
    static let streamId: UInt32 = 0x7FFF_0002
    private var display: NSObject?
    private(set) var displayID: CGDirectDisplayID = 0
    private var modeHeight: UInt32 = 1600

    private static func set(_ o: NSObject, _ key: String, _ v: Any) -> Bool {
        let setter = "set" + key.prefix(1).uppercased() + key.dropFirst() + ":"
        guard o.responds(to: NSSelectorFromString(setter)) else { return false }
        o.setValue(v, forKey: key)
        return true
    }

    /// Devuelve un mensaje de error, o nil si quedó creado
    func create(aspect: Double) -> String? {
        if display != nil { return nil }
        guard let descCls = NSClassFromString("CGVirtualDisplayDescriptor") as? NSObject.Type,
              let dispCls = NSClassFromString("CGVirtualDisplay") as? NSObject.Type else {
            return "Esta versión de macOS no permite monitores virtuales"
        }
        let desc = descCls.init()
        guard Self.set(desc, "queue", DispatchQueue.main) || Self.set(desc, "dispatchQueue", DispatchQueue.main) else { return "macOS cambió los monitores virtuales" }
        _ = Self.set(desc, "name", "QuestVRBridge")
        _ = Self.set(desc, "maxPixelsWide", NSNumber(value: UInt32(3840)))
        _ = Self.set(desc, "maxPixelsHigh", NSNumber(value: UInt32(2400)))
        _ = Self.set(desc, "sizeInMillimeters", NSValue(size: NSSize(width: 600, height: 375)))
        _ = Self.set(desc, "vendorID", NSNumber(value: UInt32(0x5156)))
        _ = Self.set(desc, "productID", NSNumber(value: UInt32(0x0002)))
        _ = Self.set(desc, "serialNum", NSNumber(value: UInt32(0x0001)))
        let initSel = NSSelectorFromString("initWithDescriptor:")
        guard let raw = (dispCls as AnyObject).perform(NSSelectorFromString("alloc"))?.takeUnretainedValue(),
              (raw as AnyObject).responds(to: initSel),
              let disp = (raw as AnyObject).perform(initSel, with: desc)?.takeRetainedValue() as? NSObject else {
            return "No se pudo crear el monitor virtual"
        }
        display = disp
        displayID = (disp.value(forKey: "displayID") as? NSNumber)?.uint32Value ?? 0
        if displayID == 0 || !apply(aspect: aspect) { display = nil; displayID = 0; return "No se pudo configurar el monitor virtual" }
        print("[monitor] Segundo monitor creado (pantalla \(displayID))")
        return nil
    }

    /// Cambia la resolución para que tenga la forma de la ventana del Quest (sin bordes negros)
    @discardableResult
    func apply(aspect: Double) -> Bool {
        guard let disp = display,
              let setCls = NSClassFromString("CGVirtualDisplaySettings") as? NSObject.Type,
              let modeCls = NSClassFromString("CGVirtualDisplayMode") as? NSObject.Type else { return false }
        let a = min(max(aspect, 0.5), 2.4)
        var h = modeHeight, w = UInt32((Double(h) * a).rounded()) & ~1
        if w > 3840 { w = 3840; h = UInt32((Double(w) / a).rounded()) & ~1 }
        typealias ModeInit = @convention(c) (AnyObject, Selector, UInt32, UInt32, Double) -> Unmanaged<AnyObject>
        typealias Apply = @convention(c) (AnyObject, Selector, AnyObject) -> Bool
        let mSel = NSSelectorFromString("initWithWidth:height:refreshRate:")
        let aSel = NSSelectorFromString("applySettings:")
        guard let mImp = class_getMethodImplementation(modeCls, mSel), let aImp = class_getMethodImplementation(type(of: disp), aSel),
              disp.responds(to: aSel), modeCls.instancesRespond(to: mSel),
              let mAlloc = (modeCls as AnyObject).perform(NSSelectorFromString("alloc"))?.takeUnretainedValue() else { return false }
        let mode = unsafeBitCast(mImp, to: ModeInit.self)(mAlloc, mSel, w, h, 60).takeRetainedValue()
        let st = setCls.init()
        _ = Self.set(st, "hiDPI", NSNumber(value: UInt32(1)))   // Retina: el texto se ve al doble de nitidez
        _ = Self.set(st, "modes", [mode])
        let ok = unsafeBitCast(aImp, to: Apply.self)(disp, aSel, st)
        if ok { print("[monitor] Resolución del segundo monitor: \(w)x\(h)") }
        return ok
    }

    func destroy() {
        if display != nil { print("[monitor] Segundo monitor quitado") }
        display = nil; displayID = 0
    }
}

/// Una ventana de una app de la Mac, capturada sola (aunque esté tapada) y enviada a su propia ventana del Quest.
@available(macOS 13.0, *)
final class WindowStream: NSObject, SCStreamOutput {
    let id: UInt32
    let encoder: DeskEncoder
    private var stream: SCStream?
    private var size = CGSize.zero
    private let queue = DispatchQueue(label: "ventana-captura", qos: .userInteractive)

    init(id: UInt32, send: @escaping (Data) -> Void) {
        self.id = id
        encoder = DeskEncoder(send: send, streamId: id, bitrate: 12_000_000)
    }

    private static func config(_ w: SCWindow) -> SCStreamConfiguration {
        let cfg = SCStreamConfiguration()
        var scale = 2.0
        if let m = CGDisplayCopyDisplayMode(CGMainDisplayID()), m.width > 0 { scale = Double(m.pixelWidth) / Double(m.width) }
        var pw = Double(w.frame.width) * scale, ph = Double(w.frame.height) * scale
        let k = min(1.0, Double(DESK_MAX_W) / max(pw, ph, 1))   // a resolución real (Retina), hasta el máximo elegido
        pw *= k; ph *= k
        cfg.width = max(64, Int(pw) & ~1)
        cfg.height = max(64, Int(ph) & ~1)
        cfg.pixelFormat = kCVPixelFormatType_32BGRA
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(DESK_FPS))
        cfg.showsCursor = true
        cfg.queueDepth = 4
        if #available(macOS 14.0, *) { cfg.ignoreShadowsSingleWindow = true }
        return cfg
    }

    func start(_ w: SCWindow) {
        size = w.frame.size
        let s = SCStream(filter: SCContentFilter(desktopIndependentWindow: w), configuration: Self.config(w), delegate: nil)
        do { try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue) }
        catch { print("[ventanas] Error: \(error.localizedDescription)"); return }
        stream = s
        s.startCapture { e in if let e { print("[ventanas] No arrancó la captura de \(w.owningApplication?.applicationName ?? "?"): \(e.localizedDescription)") } }
    }

    /// Si la ventana cambió de tamaño en la Mac, la captura se adapta
    func update(_ w: SCWindow) {
        guard let s = stream, abs(w.frame.width - size.width) > 1 || abs(w.frame.height - size.height) > 1 else { return }
        size = w.frame.size
        s.updateConfiguration(Self.config(w)) { _ in }
    }

    func stop() { stream?.stopCapture(completionHandler: nil); stream = nil }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        encoder.encode(pb)
    }
}

/// 3D automático: una IA (Depth Anything V2, de Apple para Core ML) calcula la profundidad de cada imagen del escritorio
/// en el Neural Engine de la Mac; el Quest la usa para mostrar la TV con relieve (también sirve para juegos planos).
final class Depth3D {
    var enabled = false {                     // escritorio (TV)
        didSet { if enabled && !oldValue { kick(0) } }
    }
    private var windows = Set<UInt32>()       // ventanas con su propio 3D
    private var lastRunBy: [UInt32: UInt64] = [:]
    private var range: [UInt32: (Float, Float)] = [:]
    // Última imagen (ya achicada para la IA) de cada pantalla y ventana: al activar el 3D se usa enseguida,
    // sin esperar a que la ventana cambie (una ventana quieta no manda imágenes nuevas)
    private var cache: [UInt32: CVPixelBuffer] = [:]
    private var cacheAt: [UInt32: UInt64] = [:]

    /// Prende o apaga el 3D de una ventana
    func setWindow(_ sid: UInt32, _ on: Bool) {
        lock.lock()
        let was = windows.contains(sid)
        if on { windows.insert(sid) } else { windows.remove(sid); lastRunBy[sid] = nil; range[sid] = nil }
        lock.unlock()
        if on && !was { kick(sid) }
    }

    /// Calcula ya la profundidad de la última imagen guardada (si la IA está lista y libre)
    private func kick(_ sid: UInt32) {
        lock.lock()
        guard let m = model, let pb = cache[sid], !busy else { lock.unlock(); return }
        busy = true; lastRunBy[sid] = DispatchTime.now().uptimeNanoseconds
        let r = range[sid] ?? (0, 1)
        lock.unlock()
        q.async { [self] in
            defer { lock.lock(); busy = false; lock.unlock() }
            run(m, pb, sid: sid, range: r)
        }
    }
    func clearWindows() { lock.lock(); windows.removeAll(); lastRunBy.removeAll(); range = range.filter { $0.key == 0 }; lock.unlock() }
    private(set) var statusText = ""
    private let dataDir: String
    private let send: (Data) -> Void
    private let q = DispatchQueue(label: "profundidad-3d", qos: .userInitiated)
    private let lock = NSLock()
    private var model: MLModel?
    private var loading = false
    private var busy = false
    private var inName = "", outName = ""
    private var inW = 518, inH = 392
    private let ci = CIContext(options: [.cacheIntermediates: false])
    private var inPB: CVPixelBuffer?
    private static let files = ["Manifest.json", "Data/com.apple.CoreML/model.mlmodel", "Data/com.apple.CoreML/weights/weight.bin"]
    private static let base = "https://huggingface.co/apple/coreml-depth-anything-v2-small/resolve/main/DepthAnythingV2SmallF16.mlpackage/"

    init(dataDir: String, send: @escaping (Data) -> Void) {
        self.dataDir = dataDir; self.send = send
        // Si la IA ya se bajó alguna vez, cargarla desde el principio (así el 3D arranca al instante)
        let compiled = URL(fileURLWithPath: dataDir.isEmpty ? NSTemporaryDirectory() : dataDir)
            .appendingPathComponent("modelos/DepthAnythingV2SmallF16.mlmodelc")
        if FileManager.default.fileExists(atPath: compiled.path) { prepare() }
    }

    private func setStatus(_ s: String) { lock.lock(); statusText = s; lock.unlock() }

    /// Baja el modelo la primera vez (≈50 MB), lo prepara para esta Mac y lo carga
    func prepare() {
        lock.lock()
        if model != nil || loading { lock.unlock(); return }
        loading = true
        lock.unlock()
        q.async { [self] in
            defer { lock.lock(); loading = false; lock.unlock() }
            let dir = URL(fileURLWithPath: dataDir.isEmpty ? NSTemporaryDirectory() : dataDir).appendingPathComponent("modelos")
            let compiled = dir.appendingPathComponent("DepthAnythingV2SmallF16.mlmodelc")
            let pkg = dir.appendingPathComponent("DepthAnythingV2SmallF16.mlpackage")
            let fm = FileManager.default
            do {
                if !fm.fileExists(atPath: compiled.path) {
                    for f in Self.files {
                        let dst = pkg.appendingPathComponent(f)
                        if fm.fileExists(atPath: dst.path) { continue }
                        setStatus(tr("Bajando la IA de 3D (≈50 MB, solo la primera vez)…", "Downloading the 3D AI (≈50 MB, first time only)…"))
                        try fm.createDirectory(at: dst.deletingLastPathComponent(), withIntermediateDirectories: true)
                        let sem = DispatchSemaphore(value: 0)
                        var err: Error?
                        URLSession.shared.downloadTask(with: URL(string: Self.base + f)!) { tmp, resp, e in
                            if let tmp, (resp as? HTTPURLResponse)?.statusCode == 200 {
                                do { try fm.moveItem(at: tmp, to: dst) } catch { err = error }
                            } else { err = e ?? NSError(domain: "descarga", code: (resp as? HTTPURLResponse)?.statusCode ?? -1) }
                            sem.signal()
                        }.resume()
                        sem.wait()
                        if let err { throw err }
                    }
                    setStatus(tr("Preparando la IA de 3D para esta Mac…", "Preparing the 3D AI for this Mac…"))
                    let tmp = try MLModel.compileModel(at: pkg)
                    try? fm.removeItem(at: compiled)
                    try fm.moveItem(at: tmp, to: compiled)
                }
                let cfg = MLModelConfiguration()
                if #available(macOS 13.0, *) { cfg.computeUnits = .cpuAndNeuralEngine } else { cfg.computeUnits = .all }
                let m = try MLModel(contentsOf: compiled, configuration: cfg)
                inName = m.modelDescription.inputDescriptionsByName.first(where: { $0.value.type == .image })?.key
                    ?? m.modelDescription.inputDescriptionsByName.keys.first ?? "image"
                outName = m.modelDescription.outputDescriptionsByName.keys.first ?? "depth"
                if let c = m.modelDescription.inputDescriptionsByName[inName]?.imageConstraint { inW = c.pixelsWide; inH = c.pixelsHigh }
                // Una pasada de prueba: la primera predicción prepara el motor neuronal (si no, tarda en la primera de verdad)
                var warm: CVPixelBuffer?
                CVPixelBufferCreate(nil, inW, inH, kCVPixelFormatType_32BGRA, [kCVPixelBufferIOSurfacePropertiesKey as String: [:]] as CFDictionary, &warm)
                if let warm { _ = try? m.prediction(from: MLDictionaryFeatureProvider(dictionary: [inName: MLFeatureValue(pixelBuffer: warm)])) }
                lock.lock(); model = m; lock.unlock()
                setStatus("")
                print("[3D] IA de profundidad lista (\(inW)x\(inH))")
                // Lo que ya estaba pedido mientras cargaba
                lock.lock(); let pend = Array(windows) + (enabled ? [0] : []); lock.unlock()
                for sid in pend { kick(sid) }
            } catch {
                setStatus(tr("No se pudo preparar el 3D: ", "Couldn't prepare 3D: ") + error.localizedDescription)
                print("[3D] Error: \(error)")
            }
        }
    }

    /// Una imagen nueva del escritorio (sid 0) o de una ventana: calcular su profundidad
    /// (hasta 15 por segundo cada una; la IA hace una por vez) y mandarla al Quest
    func submit(_ pb: CVPixelBuffer, sid: UInt32 = 0) {
        let now = DispatchTime.now().uptimeNanoseconds
        lock.lock()
        let on = sid == 0 ? enabled : windows.contains(sid)
        if !on {
            // Sin 3D: guardar de vez en cuando una copia chiquita, para arrancar al instante cuando se active
            let due = model != nil && now - (cacheAt[sid] ?? 0) > 500_000_000
            if due { cacheAt[sid] = now }
            lock.unlock()
            if due { q.async { [self] in if let small = smallCopy(pb) { lock.lock(); cache[sid] = small; lock.unlock() } } }
            return
        }
        guard let m = model, !busy, now - (lastRunBy[sid] ?? 0) > 66_000_000 else { lock.unlock(); return }
        busy = true; lastRunBy[sid] = now
        let r = range[sid] ?? (0, 1)
        lock.unlock()
        q.async { [self] in
            defer { lock.lock(); busy = false; lock.unlock() }
            guard let input = scaled(pb) else { return }
            run(m, input, sid: sid, range: r)
        }
    }

    /// Copia achicada (propia) de una imagen, al tamaño que pide la IA
    private func smallCopy(_ pb: CVPixelBuffer) -> CVPixelBuffer? {
        var out: CVPixelBuffer?
        CVPixelBufferCreate(nil, inW, inH, kCVPixelFormatType_32BGRA, [kCVPixelBufferIOSurfacePropertiesKey as String: [:]] as CFDictionary, &out)
        guard let o = out else { return nil }
        let img = CIImage(cvPixelBuffer: pb)
        ci.render(img.transformed(by: CGAffineTransform(scaleX: CGFloat(inW) / img.extent.width, y: CGFloat(inH) / img.extent.height)), to: o)
        return o
    }

    /// Profundidad de una imagen ya achicada → al Quest
    private func run(_ m: MLModel, _ input: CVPixelBuffer, sid: UInt32, range r: (Float, Float)) {
        var (lo, hi) = r
        do {
            guard let out = try? m.prediction(from: MLDictionaryFeatureProvider(dictionary: [inName: MLFeatureValue(pixelBuffer: input)])),
                  let fv = out.featureValue(for: outName) else { return }
            var vals: [Float] = []
            var w = 0, h = 0
            if let a = fv.multiArrayValue {
                let sh = a.shape.map { $0.intValue }
                guard sh.count >= 2 else { return }
                h = sh[sh.count - 2]; w = sh[sh.count - 1]
                vals = (0..<(w * h)).map { a[$0].floatValue }
            } else if let ob = fv.imageBufferValue {
                (vals, w, h) = Self.read(ob)
            }
            guard w > 0, h > 0, vals.count == w * h else { return }
            // Normalizar (1 = cerca) con límites que cambian despacio para que no "respire"
            var mn = Float.greatestFiniteMagnitude, mx = -Float.greatestFiniteMagnitude
            for v in vals { mn = min(mn, v); mx = max(mx, v) }
            lo = lo + (mn - lo) * 0.3; hi = hi + (mx - hi) * 0.3
            lock.lock(); range[sid] = (lo, hi); lock.unlock()
            let span = max(hi - lo, 1e-4)
            // A la mitad de tamaño alcanza (el relieve es suave)
            let ow = w / 2, oh = h / 2
            var px = [UInt8](repeating: 0, count: ow * oh)
            for y in 0..<oh { for x in 0..<ow {
                let v = (vals[(y * 2) * w + x * 2] - lo) / span
                px[y * ow + x] = UInt8(max(0, min(255, v * 255)))
            } }
            let n = px.count
            let dst = UnsafeMutablePointer<UInt8>.allocate(capacity: n + 64)
            defer { dst.deallocate() }
            let sz = px.withUnsafeBufferPointer { compression_encode_buffer(dst, n + 64, $0.baseAddress!, n, nil, COMPRESSION_ZLIB) }
            guard sz > 0 else { return }
            var p = Data([9, sid == 0 ? 0 : 1])
            if sid != 0 { var b = sid.bigEndian; p.append(Data(bytes: &b, count: 4)) }
            var bw = UInt16(ow).bigEndian, bh = UInt16(oh).bigEndian, raw = UInt32(n).bigEndian
            p.append(Data(bytes: &bw, count: 2)); p.append(Data(bytes: &bh, count: 2)); p.append(Data(bytes: &raw, count: 4))
            p.append(dst, count: sz)
            send(p)
        }
    }

    /// Achica la imagen del escritorio al tamaño que pide la IA
    private func scaled(_ pb: CVPixelBuffer) -> CVPixelBuffer? {
        if inPB == nil {
            CVPixelBufferCreate(nil, inW, inH, kCVPixelFormatType_32BGRA,
                                [kCVPixelBufferIOSurfacePropertiesKey as String: [:]] as CFDictionary, &inPB)
        }
        guard let out = inPB else { return nil }
        let img = CIImage(cvPixelBuffer: pb)
        let sx = CGFloat(inW) / img.extent.width, sy = CGFloat(inH) / img.extent.height
        ci.render(img.transformed(by: CGAffineTransform(scaleX: sx, y: sy)), to: out)
        return out
    }

    private static func half(_ b: UInt16) -> Float {
        let s: UInt32 = UInt32(b & 0x8000) << 16
        let e = Int((b >> 10) & 0x1f), m = UInt32(b & 0x3ff)
        if e == 0 { return (s != 0 ? -1 : 1) * Float(m) / 1024 / 16384 }
        if e == 31 { return 0 }
        return Float(bitPattern: s | UInt32(e - 15 + 127) << 23 | m << 13)
    }

    private static func read(_ pb: CVPixelBuffer) -> ([Float], Int, Int) {
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), row = CVPixelBufferGetBytesPerRow(pb)
        guard let base = CVPixelBufferGetBaseAddress(pb) else { return ([], 0, 0) }
        var out = [Float](repeating: 0, count: w * h)
        switch CVPixelBufferGetPixelFormatType(pb) {
        case kCVPixelFormatType_OneComponent16Half:
            for y in 0..<h { let r = (base + y * row).assumingMemoryBound(to: UInt16.self); for x in 0..<w { out[y * w + x] = half(r[x]) } }
        case kCVPixelFormatType_OneComponent32Float:
            for y in 0..<h { let r = (base + y * row).assumingMemoryBound(to: Float.self); for x in 0..<w { out[y * w + x] = r[x] } }
        case kCVPixelFormatType_OneComponent8:
            for y in 0..<h { let r = (base + y * row).assumingMemoryBound(to: UInt8.self); for x in 0..<w { out[y * w + x] = Float(r[x]) } }
        default:
            return ([], 0, 0)
        }
        return (out, w, h)
    }
}

struct MacWindow { let id: UInt32; let app: String; let title: String; let pid: pid_t; let frame: CGRect }

final class Lobby {
    struct GameEntry { let id: String; let name: String; var vr = true; var dir = "" }   // vr: false = juego plano (se abre en una ventana)
    private let device: MTLDevice
    private let shared: Shared
    private let lock = NSLock()
    private var games: [GameEntry] = []
    /// Para la ventana del Modo Centinela (lista de juegos para abrir desde la Mac)
    func gameList() -> [GameEntry] { games }
    private var lastScan: UInt64 = 0
    private var status = ""
    private var launchingQ: (id: String, name: String, t: Int)?   // juego que se está abriendo desde el visor (para la pantalla de carga)
    private var lastGameActive = Date.distantPast
    private var busy = false
    private var hover = -1
    private var drawnKey = ""
    private let TW = 1024, TH = 1280
    private let ctx: CGContext
    private var listTex: [MTLTexture] = []
    private var listIdx = 0
    private var anchorSet = false
    private var aPos = SIMD3<Float>(0, 0, 0), aFwd = SIMD3<Float>(0, 0, -1), aRight = SIMD3<Float>(1, 0, 0)
    private var prevTrig: [Float] = [0, 0]
    private var prevB: [Bool] = [false, false]
    private var lasers: [(SIMD3<Float>, SIMD3<Float>, Float)] = []
    private var hands: [simd_float4x4] = []
    private var cursor: SIMD3<Float>?
    private var desktopOn = false
    /// Qué pantalla de la Mac se ve en la pantalla principal del Quest (0 = la principal de la Mac)
    private var deskDisplay: CGDirectDisplayID = 0
    /// Ventanas de pantalla que muestran otra pantalla (botón «Cambiar de monitor»): ventana → pantalla
    private var dispRemap: [UInt32: CGDirectDisplayID] = [:]
    private var capture: AnyObject?
    private var texCache: CVMetalTextureCache?
    private var deskTex: MTLTexture?
    private var deskCV: [CVMetalTexture] = []
    private var deskAspect: Float = 16.0 / 9.0
    private var mouseDown = false
    private var lastMouse = CGPoint(x: -1, y: -1)
    private var axWarned = false
    private let scriptDir = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent()
    private let dataDir = env["QVB_DATA"] ?? ""
    private let listW: Float = 0.8, listH: Float = 1.0
    private var listC = SIMD3<Float>(0, 1.4, -1.6)
    private var deskC = SIMD3<Float>(0.6, 1.4, -1.9)
    private let deskW: Float = 1.6

    // App nativa del Quest: los paneles viajan aparte (nítidos) en vez de dibujarse en el video
    var isNative: () -> Bool = { false }
    var sendNative: (Data) -> Void = { _ in }
    private var resend = true
    private var lastPanels = ""
    private var lastPanelsAt: UInt64 = 0
    private var hiddenSent = false
    private var nativeNow = false
    private var deskEncoder: DeskEncoder!
    private var depth3D: Depth3D!
    func requestResend() { lock.lock(); resend = true; lock.unlock() }

    // MARK: Ventana 2D del Quest (lista y escritorio en una ventana normal, sin modo inmersivo)
    var mode2D = false
    var uiInHeadset = false   // sala VR con el menú y las ventanas dibujados por el Quest
    private var last2D = ""
    private var last2DAt: UInt64 = 0
    private var tick2DAt: UInt64 = 0
    private var inGame = false   // hay un juego VR abierto: el escritorio no se captura (ahorra Mac y Wi-Fi)
    // Para el indicador de rendimiento del Quest: qué juego está abierto y qué Mac es
    private var gameName = "", gameScanAt: UInt64 = 0
    private lazy var macModel: String = {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/sbin/system_profiler"); p.arguments = ["SPHardwareDataType"]
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return "Mac" }
        let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        p.waitUntilExit()
        for l in out.split(separator: "\n") where l.contains("Model Name:") { return l.components(separatedBy: ":").last?.trimmingCharacters(in: .whitespaces) ?? "Mac" }
        return "Mac"
    }()
    private func findGameName() -> String {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/bin/ps"); p.arguments = ["-axo", "command="]
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return "" }
        let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        p.waitUntilExit()
        var name = ""
        for l in out.split(separator: "\n") {
            guard let r = l.range(of: "steamapps\\common\\") else { continue }
            let rest = l[r.upperBound...]
            if let e = rest.firstIndex(of: "\\") { name = String(rest[..<e]); break }
        }
        return name
    }
    func tick2D(gameActive: Bool) {
        inGame = gameActive
        let now = DispatchTime.now().uptimeNanoseconds
        if now - tick2DAt < 200_000_000 { return }
        tick2DAt = now
        nativeNow = isNative() && !uiInHeadset
        // Con un juego abierto, lo que se revisa seguido (juegos instalados, ventanas, pantallas) se revisa mucho menos:
        // pedirle a macOS la lista de ventanas cada 2 s trababa un instante la imagen y el sonido del juego
        let slow: UInt64 = gameActive ? 6 : 1
        if now - lastScan > 5_000_000_000 * slow || games.isEmpty && now - lastScan > 1_000_000_000 { lastScan = now; scanGames() }
        if now - winScanAt > 2_000_000_000 * (gameActive ? 8 : 1) {
            winScanAt = now; scanWindows()
            // Los nombres de las pantallas se leen en el hilo principal (así lo pide macOS)
            DispatchQueue.main.async { let l = self.displayList(); self.queue.async { self.displays = l } }
        }
        lock.lock(); let st = status; let b = busy; lock.unlock()
        var err = ""
        if #available(macOS 13.0, *), desktopOn, let c = capture as? DesktopCapture, let f = c.failed { err = f }
        if gameActive && now - gameScanAt > (gameName.isEmpty ? 3_000_000_000 : 30_000_000_000) {   // (ps, aparte: no frena este hilo)
            gameScanAt = now
            DispatchQueue.global(qos: .utility).async { [weak self] in
                guard let self else { return }
                let n = self.findGameName()
                self.queue.async { self.gameName = n }
            }
        }
        if !gameActive { gameName = "" } else { lastGameActive = Date() }
        let obj: [String: Any] = ["type": "lobby2d", "launching": launchingInfo(gameActive) ?? NSNull(), "game": gameActive, "gameName": gameName, "macModel": macModel, "busy": b, "status": st, "desk": desktopOn, "deskErr": err,
                                  "depth3d": depth3D.enabled, "depthStatus": depth3D.statusText,
                                  "cfg": ["eye": GEN_EYE, "fps": GEN_FPS, "mbps": GEN_MBPS, "audioMac": env["VR_AUDIO_MAC"] == "1", "fov": GEN_FOV,   // (los generales)
                                          "codec": env["VR_CODEC"] ?? "hevc", "run": "\(EYE) · \(FPS) Hz · \(BITRATE / 1_000_000) Mbps",
                                          "audio": env["VR_AUDIO"] == "0" ? "mac" : env["VR_AUDIO_MAC"] == "1" ? "both" : "quest",
                                          "deskMaxW": DESK_MAX_W, "deskFps": DESK_FPS, "deskMbps": DESK_MBPS,
                                          "deskAudio": DESK_AUDIO == 1 ? "quest" : DESK_AUDIO == 2 ? "both" : "mac"] as [String: Any],
                                  "games": games.map { ["id": $0.id, "name": $0.name, "vr": $0.vr, "dir": $0.dir] as [String: Any] },
                                  "monitor": monitor.displayID != 0, "monitorErr": monErr, "displays": displays, "mainDisplay": mainDisplayInfo(), "deskDisplay": Self.displayBit | (deskDisplay == 0 ? CGMainDisplayID() : deskDisplay), "monitorId": monitor.displayID != 0 ? Self.displayBit | monitor.displayID : 0,
                                  "typing": Typing.shared.active, "vigia": Vigia.shared.on, "ll": linkLocalAddrs(), "perf": gameActive ? Perf.shared.snapshot() : [String: Any](), "adv": GameProfiles.shared.state(),
                                  "windows": macWins.map { ["id": $0.id, "app": $0.app, "title": $0.title,
                                                           "w": Int($0.frame.width), "h": Int($0.frame.height)] as [String: Any] }]
        guard let d = try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys]),
              let s = String(data: d, encoding: .utf8) else { return }
        if s != last2D || now - last2DAt > 2_000_000_000 { last2D = s; last2DAt = now; sendText(s) }
    }
    // Ventanas de la Mac (una ventana del Quest por cada una)
    var queue: DispatchQueue = .main
    private var macWins: [MacWindow] = []
    private var scWins: [UInt32: AnyObject] = [:]
    private var winStreams: [UInt32: AnyObject] = [:]
    private var winScanAt: UInt64 = 0
    private var focusedWin: UInt32 = 0
    private var pendingWins: Set<UInt32> = []   // pedidas antes de tener la lista de ventanas
    private let monitor = VirtualMonitor()
    private var monCapture: AnyObject?
    private var monEncoder: DeskEncoder!
    private var monErr = ""
    private var monAspect = 1.6
    private var lastResize: [UInt32: (Int, Int)] = [:]
    // Otras pantallas de la Mac (monitor físico conectado): cada una en su ventana del Quest
    private static let displayBit: UInt32 = 0x4000_0000
    private var dispStreams: [UInt32: (AnyObject, DeskEncoder)] = [:]
    private static func isDisplay(_ sid: UInt32) -> Bool { sid & displayBit != 0 && sid != VirtualMonitor.streamId }
    private static func displayID(_ sid: UInt32) -> CGDirectDisplayID { sid & ~displayBit }

    private var displays: [[String: Any]] = []
    /// La pantalla principal de la Mac como una pantalla más (para abrirla durante un juego, cuando el escritorio está apagado)
    private func mainDisplayInfo() -> [String: Any] {
        let d = CGMainDisplayID(), b = CGDisplayBounds(d)
        let name = NSScreen.main?.localizedName ?? "Mac"
        return ["id": Self.displayBit | d, "name": name, "w": Int(b.width), "h": Int(b.height)]
    }

    private func displayList() -> [[String: Any]] {
        var cnt: UInt32 = 0
        var ids = [CGDirectDisplayID](repeating: 0, count: 16)
        guard CGGetActiveDisplayList(16, &ids, &cnt) == .success else { return [] }
        var out: [[String: Any]] = []
        for d in ids.prefix(Int(cnt)) where d != CGMainDisplayID() && d != monitor.displayID {
            let b = CGDisplayBounds(d)
            let name = NSScreen.screens.first(where: { ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value == d })?.localizedName ?? "Monitor"
            out.append(["id": Self.displayBit | d, "name": name, "w": Int(b.width), "h": Int(b.height)])
        }
        return out
    }

    private func startDisplay(_ sid: UInt32) {
        guard #available(macOS 13.0, *) else { return }
        if let e = dispStreams[sid] { e.1.requestKey(); return }
        let enc = DeskEncoder(send: { [weak self] d in self?.sendNative(d) }, streamId: sid, bitrate: 20_000_000)
        let c = DesktopCapture()
        c.displayID = dispRemap[sid] ?? Self.displayID(sid)
        c.onFrame = { pb in enc.encode(pb) }
        c.start()
        dispStreams[sid] = (c, enc)
        print("[monitor] Mostrando en el Quest la pantalla \(Self.displayID(sid))")
    }

    private func stopDisplay(_ sid: UInt32) {
        guard #available(macOS 13.0, *), let e = dispStreams[sid] else { return }
        (e.0 as? DesktopCapture)?.stop()
        dispStreams[sid] = nil
    }

    private func startMonitor() {
        guard #available(macOS 13.0, *) else { monErr = "Necesita macOS 13 o más nuevo"; return }
        if monitor.displayID != 0 { monEncoder.requestKey(); return }
        if let e = monitor.create(aspect: monAspect) { monErr = e; print("[monitor] \(e)"); return }
        monErr = ""
        if monEncoder == nil { monEncoder = DeskEncoder(send: { [weak self] d in self?.sendNative(d) }, streamId: VirtualMonitor.streamId, bitrate: 20_000_000) }
        startMonCapture()
    }

    private func startMonCapture() {
        guard #available(macOS 13.0, *) else { return }
        (monCapture as? DesktopCapture)?.stop()
        let c = DesktopCapture()
        c.displayID = monitor.displayID
        let enc = monEncoder!
        c.onFrame = { pb in enc.encode(pb) }
        c.start()
        monCapture = c
    }

    func stopMonitor() {
        guard #available(macOS 13.0, *) else { return }
        (monCapture as? DesktopCapture)?.stop(); monCapture = nil
        monitor.destroy()
    }

    /// La ventana del Quest cambió de tamaño: adaptar la ventana de la Mac (o el monitor) a esa forma
    private func resizeFor(_ sid: UInt32, qw: Int, qh: Int) {
        guard qw > 100, qh > 100, !Self.isDisplay(sid) else { return }
        if let l = lastResize[sid], l == (qw, qh) { return }
        lastResize[sid] = (qw, qh)
        let aspect = Double(qw) / Double(qh)
        if sid == VirtualMonitor.streamId {
            monAspect = aspect
            if monitor.displayID != 0, monitor.apply(aspect: aspect) {
                // La captura arranca de nuevo con la resolución nueva (después de que macOS la aplique)
                queue.asyncAfter(deadline: .now() + 0.8) { [weak self] in
                    guard let self, self.monitor.displayID != 0 else { return }
                    self.startMonCapture()
                }
            }
            return
        }
        guard let mw = macWins.first(where: { $0.id == sid }), let fr = windowFrame(sid), let el = axWindow(mw, fr) else { return }
        // Tamaño en puntos de la Mac ≈ tamaño de la ventana del Quest, sin pasarse de la pantalla
        let vis = visibleFrameCG()
        var w = CGFloat(qw), h = CGFloat(qh)
        let k = min(1, vis.width / w, vis.height / h)
        w *= k; h *= k
        var size = CGSize(width: w.rounded(), height: h.rounded())
        var pos = CGPoint(x: min(max(fr.minX, vis.minX), vis.maxX - size.width), y: min(max(fr.minY, vis.minY), vis.maxY - size.height))
        if let pv = AXValueCreate(.cgPoint, &pos) { AXUIElementSetAttributeValue(el, kAXPositionAttribute as CFString, pv) }
        if let sv = AXValueCreate(.cgSize, &size) { AXUIElementSetAttributeValue(el, kAXSizeAttribute as CFString, sv) }
        winScanAt = 0   // volver a leer las ventanas pronto
    }

    /// Zona útil de la pantalla principal (sin la barra de menú ni el Dock), en coordenadas de arriba hacia abajo
    private func visibleFrameCG() -> CGRect {
        DispatchQueue.main.sync {
            guard let scr = NSScreen.screens.first else { return CGDisplayBounds(CGMainDisplayID()) }
            let vf = scr.visibleFrame, full = scr.frame
            return CGRect(x: vf.minX, y: full.maxY - vf.maxY, width: vf.width, height: vf.height)
        }
    }
    private static let hiddenApps: Set<String> = ["local.questvrbridge.app", "com.apple.dock", "com.apple.WindowManager",
        "com.apple.controlcenter", "com.apple.notificationcenterui", "com.apple.Spotlight", "com.apple.systemuiserver"]

    private func scanWindows() {
        guard #available(macOS 13.0, *) else { return }
        SCShareableContent.getExcludingDesktopWindows(true, onScreenWindowsOnly: true) { [weak self] content, _ in
            guard let self, let content else { return }
            var list: [MacWindow] = []
            var map: [UInt32: AnyObject] = [:]
            for w in content.windows {
                guard w.windowLayer == 0, w.isOnScreen, w.frame.width >= 200, w.frame.height >= 120,
                      let app = w.owningApplication, !Self.hiddenApps.contains(app.bundleIdentifier),
                      !app.applicationName.isEmpty else { continue }
                list.append(MacWindow(id: w.windowID, app: app.applicationName, title: w.title ?? "", pid: app.processID, frame: w.frame))
                map[w.windowID] = w
            }
            list.sort { ($0.app, $0.id) < ($1.app, $1.id) }
            self.queue.async {
                self.macWins = list
                self.scWins = map
                for (id, st) in self.winStreams { if let w = map[id] as? SCWindow { (st as? WindowStream)?.update(w) } }
                let pend = self.pendingWins
                self.pendingWins = []
                for id in pend { self.startWindow(id) }
            }
        }
    }

    func requestWinKey(_ id: UInt32) {
        guard #available(macOS 13.0, *) else { return }
        if id == VirtualMonitor.streamId { monEncoder?.requestKey(); return }
        if Self.isDisplay(id) { dispStreams[id]?.1.requestKey(); return }
        (winStreams[id] as? WindowStream)?.encoder.requestKey()
    }

    func stopWindows() {
        guard #available(macOS 13.0, *) else { return }
        for sid in Array(dispStreams.keys) { stopDisplay(sid) }
        for (_, st) in winStreams { (st as? WindowStream)?.stop() }
        winStreams = [:]
        depth3D?.clearWindows()
    }

    /// Posición actual de la ventana en la pantalla de la Mac (puede haberse movido)
    private func windowFrame(_ id: UInt32) -> CGRect? {
        if let info = CGWindowListCopyWindowInfo([.optionIncludingWindow], CGWindowID(id)) as? [[String: Any]],
           let b = info.first?[kCGWindowBounds as String] as? [String: Any],
           let r = CGRect(dictionaryRepresentation: b as CFDictionary) { return r }
        return macWins.first(where: { $0.id == id })?.frame
    }

    /// Trae la ventana al frente en la Mac (para que el clic y el teclado vayan a ella)
    private func focusWindow(_ w: MacWindow, _ frame: CGRect) {
        let appEl = AXUIElementCreateApplication(w.pid)
        AXUIElementSetAttributeValue(appEl, kAXFrontmostAttribute as CFString, kCFBooleanTrue)
        NSRunningApplication(processIdentifier: w.pid)?.activate(options: [])
        if let el = axWindow(w, frame) {
            AXUIElementPerformAction(el, kAXRaiseAction as CFString)
            AXUIElementSetAttributeValue(el, kAXMainAttribute as CFString, kCFBooleanTrue)
        }
    }

    /// La misma ventana vista por Accesibilidad (para traerla al frente o cambiarle el tamaño)
    private func axWindow(_ w: MacWindow, _ frame: CGRect) -> AXUIElement? {
        let appEl = AXUIElementCreateApplication(w.pid)
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(appEl, kAXWindowsAttribute as CFString, &value) == .success,
              let wins = value as? [AXUIElement] else { return nil }
        var best: AXUIElement?
        for el in wins {
            var pv: CFTypeRef?, sv: CFTypeRef?
            var pos = CGPoint.zero, size = CGSize.zero
            if AXUIElementCopyAttributeValue(el, kAXPositionAttribute as CFString, &pv) == .success, let pv { AXValueGetValue(pv as! AXValue, .cgPoint, &pos) }
            if AXUIElementCopyAttributeValue(el, kAXSizeAttribute as CFString, &sv) == .success, let sv { AXValueGetValue(sv as! AXValue, .cgSize, &size) }
            if abs(pos.x - frame.minX) < 4 && abs(pos.y - frame.minY) < 4 && abs(size.width - frame.width) < 4 { best = el; break }
            var tv: CFTypeRef?
            if best == nil, AXUIElementCopyAttributeValue(el, kAXTitleAttribute as CFString, &tv) == .success, (tv as? String) == w.title, !w.title.isEmpty { best = el }
        }
        return best
    }

    private func startWindow(_ sid: UInt32) {
        guard #available(macOS 13.0, *), winStreams[sid] == nil, let w = scWins[sid] as? SCWindow else { return }
        let st = WindowStream(id: sid, send: { [weak self] d in self?.sendNative(d) })
        winStreams[sid] = st
        st.start(w)
        print("[ventanas] Mostrando en el Quest: \(w.owningApplication?.applicationName ?? "?") — \(w.title ?? "")")
    }

    func handle2D(_ m: InMsg) {
        switch m.type {
        case "deskdisp":
            // Botón «Cambiar de monitor» de la pantalla principal: otra pantalla de la Mac (o el monitor virtual)
            let raw = (m.id ?? 0) & ~Self.displayBit
            deskDisplay = (raw == 0 || raw == CGMainDisplayID()) ? 0 : raw
            if desktopOn { toggleDesktop(); toggleDesktop() }
            print("[escritorio] Pantalla principal del Quest: pantalla \(deskDisplay == 0 ? CGMainDisplayID() : deskDisplay)")
            last2D = ""
        case "dispswap":
            // Lo mismo en una ventana de pantalla (por ejemplo, la pantalla principal dentro de un juego)
            if let sid = m.id, Self.isDisplay(sid) {
                let raw = (m.to ?? sid) & ~Self.displayBit
                if raw == 0 || raw == Self.displayID(sid) { dispRemap[sid] = nil } else { dispRemap[sid] = raw }
                if dispStreams[sid] != nil { stopDisplay(sid); startDisplay(sid) }
                print("[monitor] Ventana \(sid): ahora muestra la pantalla \(dispRemap[sid] ?? Self.displayID(sid))")
            }
        case "deskcfg":
            // Escritorio y ventanas: resolución máxima, cuadros por segundo y calidad
            let mw = min(max(m.maxw ?? DESK_MAX_W, 960), 3840), f = min(max(m.fps ?? DESK_FPS, 15), 60), mb = min(max(m.mbps ?? DESK_MBPS, 4), 80)
            let restart = mw != DESK_MAX_W || f != DESK_FPS
            DESK_MAX_W = mw; DESK_FPS = f; DESK_MBPS = mb
            deskEncoder.setBitrate(mb * 1_000_000)
            for (_, e) in dispStreams { e.1.setBitrate(mb * 1_000_000) }
            monEncoder?.setBitrate(mb * 1_000_000)
            if restart && desktopOn { toggleDesktop(); toggleDesktop() }   // la captura arranca de nuevo con lo elegido
            print("[escritorio] Ajustes: hasta \(mw) px · \(f) cuadros/s · \(mb) Mbps")
            if let a = m.audio { DESK_AUDIO = a == "quest" ? 1 : a == "both" ? 2 : 0 }
            last2D = ""
        case "settings":
            // Juegos VR: se aplican reiniciando el servidor, eso lo hace la app de la Mac
            var o: [String: Any] = ["qvb": "settings"]
            if let v = m.eye { o["eye"] = v }
            if let v = m.fps { o["fps"] = v }
            if let v = m.mbps { o["mbps"] = v }
            if let v = m.audioMac { o["audioMac"] = v }
            if let v = m.audio { o["audio"] = v }
            if let v = m.fov { o["fov"] = v }
            if let v = m.codec { o["codec"] = v }
            if let d = try? JSONSerialization.data(withJSONObject: o) {
                let s = socket(AF_INET, SOCK_DGRAM, 0)
                var a = sockaddr_in()
                a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET)
                a.sin_port = UInt16(9102).bigEndian; a.sin_addr.s_addr = inet_addr("127.0.0.1")
                _ = d.withUnsafeBytes { b in withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(s, b.baseAddress, d.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } } }
                close(s)
                print("[ajustes] Juegos VR: \(o) (la app de la Mac reinicia el servidor)")
            }
        case "depth3d":
            if m.on == true { depth3D.prepare() }
            if let sid = m.id, sid != 0 {
                // 3D de una sola ventana (o pantalla)
                depth3D.setWindow(sid, m.on == true)
                print("[3D] Ventana \(sid) en 3D: \(m.on == true ? "activado" : "desactivado")")
            } else {
                depth3D.enabled = m.on == true
                print(m.on == true ? "[3D] Escritorio en 3D: activado" : "[3D] Escritorio en 3D: desactivado")
            }
            last2D = ""
        case "monitor":
            if m.on == true { startMonitor() } else { stopMonitor() }
            last2D = ""
        case "wsize":
            if let sid = m.id, let w = m.w, let h = m.h { resizeFor(sid, qw: w, qh: h) }
        case "win":
            guard #available(macOS 13.0, *), let sid = m.id else { return }
            if sid == VirtualMonitor.streamId { if m.on == true { startMonitor() }; return }
            if Self.isDisplay(sid) { if m.on == true { startDisplay(sid) } else { stopDisplay(sid) }; return }
            if m.on == true {
                if let st = winStreams[sid] as? WindowStream { st.encoder.requestKey(); return }
                if scWins[sid] == nil { pendingWins.insert(sid); winScanAt = DispatchTime.now().uptimeNanoseconds; scanWindows(); return }
                startWindow(sid)
            } else {
                pendingWins.remove(sid)
                (winStreams[sid] as? WindowStream)?.stop()
                winStreams[sid] = nil
                depth3D.setWindow(sid, false)
            }
        case "wmouse":
            if m.id == VirtualMonitor.streamId, let u = m.u, let v = m.v, monitor.displayID != 0 {
                focusedWin = 0
                let b = CGDisplayBounds(monitor.displayID)
                postMouse(CGPoint(x: b.minX + CGFloat(min(max(u, 0), 1)) * b.width, y: b.minY + CGFloat(min(max(v, 0), 1)) * b.height),
                          down: m.down, scroll: m.scroll ?? 0)
                return
            }
            if let sid = m.id, Self.isDisplay(sid), let u = m.u, let v = m.v {
                focusedWin = 0
                let b = CGDisplayBounds(dispRemap[sid] ?? Self.displayID(sid))
                postMouse(CGPoint(x: b.minX + CGFloat(min(max(u, 0), 1)) * b.width, y: b.minY + CGFloat(min(max(v, 0), 1)) * b.height),
                          down: m.down, scroll: m.scroll ?? 0)
                return
            }
            guard let sid = m.id, let u = m.u, let v = m.v, let mw = macWins.first(where: { $0.id == sid }),
                  let fr = windowFrame(sid) else { return }
            if focusedWin != sid || m.down == true { focusedWin = sid; focusWindow(mw, fr) }
            let p = CGPoint(x: fr.minX + CGFloat(min(max(u, 0), 1)) * fr.width, y: fr.minY + CGFloat(min(max(v, 0), 1)) * fr.height)
            postMouse(p, down: m.down, scroll: m.scroll ?? 0)
        case "launch":
            if let id = m.game, let g = games.first(where: { $0.id == id }) { launch(g) }
        case "report":     // «Algo anda mal» en el Quest: VRmin Host junta los registros
            print("[reporte] pedido desde el visor")
        case "quitgame":   // desde el menú del juego en el Quest: cerrar solo el juego
            print("[sala] Cerrando el juego desde el visor")
            DispatchQueue.global(qos: .userInitiated).async { [self] in _ = runScript("cerrar_juego.sh", []) }
        case "desk":
            if let on = m.on, on != desktopOn { toggleDesktop() }
            if m.on == true { deskEncoder.requestKey() }
            last2D = ""
        case "mouse":
            focusedWin = 0
            if let u = m.u, let v = m.v { mouse(min(max(u, 0), 1), min(max(v, 0), 1), down: m.down, scroll: m.scroll ?? 0) }
        default: break
        }
    }
    func requestDeskKey() { deskEncoder.requestKey() }
    /// Cuando hay un juego abierto, la app del Quest esconde los paneles
    func setHidden() {
        if hiddenSent { return }
        hiddenSent = true
        if isNative() { sendText("{\"type\":\"panels\",\"lobby\":false}") }
    }
    private func sendText(_ t: String) { var d = Data([3]); d.append(Data(t.utf8)); sendNative(d) }

    init(device: MTLDevice, shared: Shared) {
        self.device = device
        self.shared = shared
        ctx = CGContext(data: nil, width: TW, height: TH, bitsPerComponent: 8, bytesPerRow: TW * 4,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: TW, height: TH, mipmapped: false)
        td.usage = .shaderRead
        for _ in 0..<2 { listTex.append(device.makeTexture(descriptor: td)!) }
        CVMetalTextureCacheCreate(nil, nil, device, nil, &texCache)
        deskEncoder = DeskEncoder(send: { [weak self] d in self?.sendNative(d) }, bitrate: DESK_MBPS * 1_000_000)
        depth3D = Depth3D(dataDir: dataDir, send: { [weak self] d in self?.sendNative(d) })
        DeskEncoder.depthHook = { [weak self] pb, sid in self?.depth3D.submit(pb, sid: sid) }
    }

    // MARK: Juegos
    private func scanGames() {
        let lib = URL(fileURLWithPath: dataDir).appendingPathComponent("prefijo/drive_c/Program Files (x86)/Steam/steamapps")
        guard !dataDir.isEmpty, let files = try? FileManager.default.contentsOfDirectory(atPath: lib.path),
              let re = try? NSRegularExpression(pattern: "^\\s*\"(\\w+)\"\\s+\"([^\"]*)\"", options: [.anchorsMatchLines]) else { return }
        var out: [GameEntry] = []
        for f in files where f.hasPrefix("appmanifest_") && f.hasSuffix(".acf") {
            guard let txt = try? String(contentsOf: lib.appendingPathComponent(f), encoding: .utf8) else { continue }
            var d: [String: String] = [:]
            for m in re.matches(in: txt, range: NSRange(txt.startIndex..., in: txt)) {
                guard let k = Range(m.range(at: 1), in: txt), let v = Range(m.range(at: 2), in: txt) else { continue }
                if d[String(txt[k])] == nil { d[String(txt[k])] = String(txt[v]) }
            }
            guard let id = d["appid"], let dir = d["installdir"], !LOBBY_NOT_GAMES.contains(id),
                  FileManager.default.fileExists(atPath: lib.appendingPathComponent("common/\(dir)").path) else { continue }
            // VR o plano: lo decide VRmin Host (vrjuegos.py, con la tienda de Steam) y queda en perfiles/detectado
            var vr = true
            if let dd = try? Data(contentsOf: URL(fileURLWithPath: dataDir + "/perfiles/detectado/\(id).json")),
               let o = try? JSONSerialization.jsonObject(with: dd) as? [String: Any] {
                vr = (o["vr"] as? Bool) ?? !((o["apis"] as? [Any]) ?? [1]).isEmpty
            }
            out.append(GameEntry(id: id, name: d["name"] ?? dir, vr: vr, dir: dir))
        }
        out.sort { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        games = out
    }

    private func setStatus(_ s: String) { lock.lock(); status = s; lock.unlock() }

    private func steamRunning() -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
        p.arguments = ["-if", "steam\\.exe"]
        p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    private func runScript(_ name: String, _ args: [String]) -> Bool {
        let path = scriptDir.appendingPathComponent(name).path
        guard FileManager.default.fileExists(atPath: path) else { return false }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/bash")
        p.arguments = [path] + args
        guard (try? p.run()) != nil else { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    /// Qué juego se está abriendo (pantalla de carga del visor): el abierto desde el visor, o el que abrió jugar.sh
    /// (desde VRmin Host) hace menos de 90 s y todavía no arrancó
    private func launchingInfo(_ gameActive: Bool) -> [String: Any]? {
        lock.lock()
        if gameActive || (launchingQ.map { Int(Date().timeIntervalSince1970) - $0.t > 240 } ?? false) { launchingQ = nil }
        let q = launchingQ
        lock.unlock()
        if gameActive { return nil }
        if let q { return ["id": q.id, "name": q.name, "t": q.t] }
        let path = dataDir + "/lanzando.txt"
        guard !dataDir.isEmpty, let m = (try? FileManager.default.attributesOfItem(atPath: path))?[.modificationDate] as? Date,
              Date().timeIntervalSince(m) < 90, m > lastGameActive,
              let txt = try? String(contentsOfFile: path, encoding: .utf8) else { return nil }
        let f = txt.trimmingCharacters(in: .whitespacesAndNewlines).components(separatedBy: "\t")
        guard f.count >= 3 else { return nil }
        return ["id": f[0], "name": f[1], "t": Int(f[2]) ?? 0]
    }

    private func launch(_ g: GameEntry) {
        lock.lock()
        if busy { lock.unlock(); return }
        busy = true; status = "Preparando \(g.name)…"
        launchingQ = (g.id, g.name, Int(Date().timeIntervalSince1970))
        lock.unlock()
        print("[sala] Abriendo \(g.name) desde el visor")
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            if !steamRunning() {
                setStatus(tr("Abriendo Steam (≈1 minuto)…", "Opening Steam (≈1 minute)…"))
                if !runScript("abrir_steam.sh", []) && !steamRunning() {
                    setStatus(tr("No pude abrir Steam (usá la app de la Mac)", "Couldn't open Steam (use the Mac app)"))
                }
                var waited = 0
                while !steamRunning() && waited < 150 { sleep(2); waited += 2 }
                if steamRunning() { setStatus(tr("Steam abierto, esperando que termine de cargar…", "Steam is open, waiting for it to finish loading…")); sleep(25) }
            }
            setStatus(g.vr ? tr("Abriendo \(g.name)… (si Steam pregunta, elegí VR en la Mac)", "Opening \(g.name)… (if Steam asks, choose VR on the Mac)")
                           : tr("Abriendo \(g.name)… aparece como una ventana de la Mac", "Opening \(g.name)… it shows up as a Mac window"))
            // Si el juego tiene otra calidad que la que corre, la app de la Mac reinicia el servidor con esa y lo abre ella
            let q = GameProfiles.shared.quality(for: g.dir)
            if g.vr && q.differs {
                setStatus(tr("Cambiando la calidad para \(g.name)… (se corta unos segundos)", "Switching quality for \(g.name)… (a few seconds' cut)"))
                toHost(["qvb": "playgame", "id": g.id, "name": g.name, "eye": q.eye, "fps": q.fps, "mbps": q.mbps, "fov": q.fov])
                lock.lock(); busy = false; lock.unlock()   // (launchingQ sigue: la app de la Mac lo abre)
                print("[sala] \(g.name) usa otra calidad (\(q.eye) · \(q.fps) Hz · \(q.mbps) Mbps): la app de la Mac reinicia y lo abre")
                return
            }
            if !runScript("jugar.sh", [g.id]) {   // el runtime y los argumentos de cada juego los decide jugar.sh
                setStatus(tr("No se pudo abrir \(g.name). Mirá el registro en la Mac.", "Couldn't open \(g.name). Check the log on the Mac."))
            }
            sleep(40)
            lock.lock(); busy = false; launchingQ = nil; if status.hasPrefix("Abriendo") || status.hasPrefix("Preparando") { status = "" }; lock.unlock()
        }
    }

    private func toggleDesktop() {
        desktopOn.toggle()
        if #available(macOS 13.0, *) {
            if desktopOn {
                let c = DesktopCapture()
                if deskDisplay != 0 { c.displayID = deskDisplay }
                c.onFrame = { [weak self] pb in
                    guard let self, self.isNative(), self.mode2D || ((self.uiInHeadset || (self.nativeNow && !self.hiddenSent)) && !self.inGame) else { return }
                    self.deskEncoder.encode(pb)
                    self.depth3D.submit(pb)
                }
                c.start(); capture = c
            } else {
                (capture as? DesktopCapture)?.stop(); capture = nil; deskTex = nil; deskCV = []
            }
        }
    }

    // MARK: Por cuadro
    private func vec(_ a: [Float], _ i: Int) -> SIMD3<Float> { SIMD3<Float>(a[i], a[i + 1], a[i + 2]) }
    private func quat(_ a: [Float], _ i: Int) -> simd_quatf { simd_quatf(ix: a[i], iy: a[i + 1], iz: a[i + 2], r: a[i + 3]) }

    private func hit(_ o: SIMD3<Float>, _ d: SIMD3<Float>, _ c: SIMD3<Float>, _ w: Float, _ h: Float) -> (Float, Float, Float)? {
        let n = -aFwd
        let den = simd_dot(d, n)
        if abs(den) < 1e-4 { return nil }
        let t = simd_dot(c - o, n) / den
        if t <= 0 { return nil }
        let p = o + d * t
        let lx = simd_dot(p - c, aRight), ly = p.y - c.y
        if abs(lx) > w / 2 || abs(ly) > h / 2 { return nil }
        return (t, lx / w + 0.5, 0.5 - ly / h)
    }

    private func rowAt(_ v: Float) -> Int {
        let py = Int(v * Float(TH))
        let i = (py - 170) / 92
        if py < 170 || i < 0 || i > min(games.count, 9) + (nativeNow ? 1 : 0) { return -1 }
        return (py - 170) % 92 < 84 ? i : -1
    }

    func update(pose: Pose) {
        // Sala VR nueva: el menú y las ventanas los dibuja la app del Quest; acá solo queda la habitación
        if uiInHeadset { nativeNow = false; lasers = []; hands = []; cursor = nil; hover = -1; return }
        nativeNow = isNative()
        if hiddenSent { lastPanels = ""; deskEncoder.requestKey() }   // volvió la sala: mostrar los paneles ya
        hiddenSent = false
        let now = DispatchTime.now().uptimeNanoseconds
        if now - lastScan > 5_000_000_000 { lastScan = now; scanGames() }
        guard pose.eyes.count >= 2 else { return }
        let headM = pose.eyes[0].view.inverse
        let head = SIMD3<Float>(headM.columns.3.x, headM.columns.3.y, headM.columns.3.z)
        var f = -SIMD3<Float>(headM.columns.2.x, 0, headM.columns.2.z)
        if simd_length(f) < 1e-3 { f = SIMD3<Float>(0, 0, -1) }
        f = simd_normalize(f)
        let xr = shared.xr
        let hasHands = xr.count >= 23 + 54
        // Re-centrar con B o Y
        var recenter = !anchorSet
        if hasHands {
            for h in 0..<2 {
                let b = xr[23 + 27 * h + 21] > 0.5
                if b && !prevB[h] { recenter = true }
                prevB[h] = b
            }
        }
        if recenter {
            anchorSet = true
            aPos = SIMD3<Float>(head.x, 0, head.z); aFwd = f; aRight = SIMD3<Float>(-f.z, 0, f.x)
        }
        let up = SIMD3<Float>(0, 1, 0)
        let eyeH = max(1.0, head.y) - 0.15
        if desktopOn {
            listC = aPos + aFwd * 1.5 - aRight * 0.95 + up * eyeH
            deskC = aPos + aFwd * 1.9 + aRight * 0.45 + up * eyeH
        } else {
            listC = aPos + aFwd * 1.5 + up * eyeH
        }
        let deskH = deskW / deskAspect

        // Controles: rayo, selección y clics
        lasers = []; hands = []; cursor = nil
        var newHover = -1
        if hasHands {
            for h in 0..<2 {
                let base = 23 + 27 * h
                guard xr[base] > 0.5 else { continue }
                let g = simd_float4x4(quat(xr, base + 4))
                var gm = g; gm.columns.3 = SIMD4<Float>(vec(xr, base + 1), 1)
                hands.append(gm)
                let o = vec(xr, base + 8)
                let d = simd_normalize(quat(xr, base + 11).act(SIMD3<Float>(0, 0, -1)))
                let trig = xr[base + 15]
                let pressed = trig > 0.7 && prevTrig[h] <= 0.7
                let released = trig < 0.3 && prevTrig[h] >= 0.3
                prevTrig[h] = trig
                var best: (Float, Int, Float, Float)? = nil  // t, panel (0 lista, 1 escritorio), u, v
                if let r = hit(o, d, listC, listW, listH) { best = (r.0, 0, r.1, r.2) }
                if desktopOn, let r = hit(o, d, deskC, deskW, deskH), best == nil || r.0 < best!.0 { best = (r.0, 1, r.1, r.2) }
                let len = best?.0 ?? 2.5
                lasers.append((o, d, len))
                guard let b = best else { continue }
                cursor = o + d * b.0
                if b.1 == 0 {
                    let row = rowAt(b.3)
                    if row >= 0 { newHover = row }
                    if pressed && row >= 0 {
                        if row == min(games.count, 9) { toggleDesktop() }
                        else if row < games.count { launch(games[row]) }
                        else if nativeNow && row == min(games.count, 9) + 1 { sendText("{\"type\":\"exitvr\"}") }
                    }
                } else {
                    let dn: Bool? = pressed ? true : ((released && mouseDown) ? false : nil)
                    mouse(b.2, b.3, down: dn, scroll: xr[base + 18])
                }
            }
        }
        hover = newHover
        lock.lock(); let rs = resend; resend = false; lock.unlock()
        if rs { drawnKey = ""; deskEncoder.requestKey(); lastPanels = "" }
        redrawList()
        if nativeNow {
            let pj = panelJSON()
            if pj != lastPanels || now - lastPanelsAt > 1_000_000_000 { lastPanels = pj; lastPanelsAt = now; sendText(pj) }
        }

        // Escritorio: tomar el cuadro más nuevo
        if #available(macOS 13.0, *), desktopOn, let cap = capture as? DesktopCapture, let pb = cap.frame, let cache = texCache {
            var cv: CVMetalTexture?
            let w = CVPixelBufferGetWidth(pb), hh = CVPixelBufferGetHeight(pb)
            CVMetalTextureCacheCreateTextureFromImage(nil, cache, pb, nil, .bgra8Unorm, w, hh, 0, &cv)
            if let cv, let t = CVMetalTextureGetTexture(cv) {
                deskTex = t
                deskAspect = Float(w) / Float(max(hh, 1))
                deskCV.append(cv)
                if deskCV.count > 3 { deskCV.removeFirst() }
            }
        }
    }

    private func mouse(_ u: Float, _ v: Float, down: Bool?, scroll: Float) {
        guard #available(macOS 13.0, *), let cap = capture as? DesktopCapture else { return }
        if !axWarned {
            axWarned = true
            let opts = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
            if !AXIsProcessTrustedWithOptions(opts) {
                print("[escritorio] Para usar el mouse desde el visor: Ajustes del Sistema > Privacidad y seguridad > Accesibilidad > activá QuestVRBridge")
            }
        }
        let b = cap.bounds
        postMouse(CGPoint(x: b.minX + CGFloat(u) * b.width, y: b.minY + CGFloat(v) * b.height), down: down, scroll: scroll)
    }

    private func postMouse(_ p: CGPoint, down: Bool?, scroll: Float) {
        if !axWarned {
            axWarned = true
            let opts = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
            if !AXIsProcessTrustedWithOptions(opts) {
                print("[escritorio] Para usar el mouse desde el visor: Ajustes del Sistema > Privacidad y seguridad > Accesibilidad > activá QuestVRBridge")
            }
        }
        var type: CGEventType? = nil
        if let down { type = down ? .leftMouseDown : .leftMouseUp; mouseDown = down }
        else if abs(p.x - lastMouse.x) > 1 || abs(p.y - lastMouse.y) > 1 { type = mouseDown ? .leftMouseDragged : .mouseMoved }
        if let type {
            CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
            lastMouse = p
        }
        if abs(scroll) > 0.25 {
            CGEvent(scrollWheelEvent2Source: nil, units: .pixel, wheelCount: 1, wheel1: Int32(scroll * 18), wheel2: 0, wheel3: 0)?
                .post(tap: .cghidEventTap)
        }
    }

    // MARK: Dibujo
    private func text(_ s: String, _ x: CGFloat, _ y: CGFloat, _ size: CGFloat, _ color: CGColor, bold: Bool = false) {
        let font = CTFontCreateWithName((bold ? "Helvetica-Bold" : "Helvetica") as CFString, size, nil)
        let attrs: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTForegroundColorAttributeName as String): color]
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: attrs))
        ctx.textPosition = CGPoint(x: x, y: y)
        CTLineDraw(line, ctx)
    }

    private func panelJSON() -> String {
        let q = simd_quatf(simd_float3x3(columns: (aRight, SIMD3<Float>(0, 1, 0), -aFwd)))
        func pj(_ c: SIMD3<Float>, _ w: Float, _ h: Float) -> String {
            String(format: "{\"pos\":[%.4f,%.4f,%.4f],\"rot\":[%.5f,%.5f,%.5f,%.5f],\"w\":%.4f,\"h\":%.4f}",
                   c.x, c.y, c.z, q.imag.x, q.imag.y, q.imag.z, q.real, w, h)
        }
        var s = "{\"type\":\"panels\",\"lobby\":true,\"list\":" + pj(listC, listW, listH)
        if desktopOn && deskTex != nil { s += ",\"desk\":" + pj(deskC, deskW, deskW / deskAspect) }
        return s + "}"
    }

    /// Manda la imagen de la lista (RGBA comprimida con deflate) a la app del Quest
    private func sendListImage() {
        guard let data = ctx.data else { return }
        let n = TW * TH * 4
        let dst = UnsafeMutablePointer<UInt8>.allocate(capacity: n)
        defer { dst.deallocate() }
        let sz = compression_encode_buffer(dst, n, data.assumingMemoryBound(to: UInt8.self), n, nil, COMPRESSION_ZLIB)
        guard sz > 0 else { return }
        var p = Data([6, 0])
        var w = UInt16(TW).bigEndian, h = UInt16(TH).bigEndian, raw = UInt32(n).bigEndian
        p.append(Data(bytes: &w, count: 2)); p.append(Data(bytes: &h, count: 2)); p.append(Data(bytes: &raw, count: 4))
        p.append(dst, count: sz)
        sendNative(p)
    }

    private func redrawList() {
        lock.lock(); let st = status; let isBusy = busy; lock.unlock()
        let key = games.map { $0.name }.joined(separator: "|") + "#\(hover)#\(st)#\(desktopOn)#\(isBusy)#\(nativeNow)"
        if key == drawnKey { return }
        drawnKey = key
        let H = CGFloat(TH), W = CGFloat(TW)
        ctx.setFillColor(CGColor(red: 0.07, green: 0.08, blue: 0.11, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
        text("QuestVRBridge", 48, H - 95, 64, CGColor(red: 1, green: 1, blue: 1, alpha: 1), bold: true)
        text("Apuntá con el control y apretá el gatillo · B/Y: centrar", 50, H - 145, 28, CGColor(red: 0.6, green: 0.65, blue: 0.75, alpha: 1))
        let rows = min(games.count, 9)
        for i in 0...(rows + (nativeNow ? 1 : 0)) {
            let top = CGFloat(170 + i * 92)
            let r = CGRect(x: 36, y: H - top - 84, width: W - 72, height: 84)
            let isToggle = i == rows
            let isExit = i == rows + 1
            let hov = i == hover
            if isExit {
                ctx.setFillColor(CGColor(red: hov ? 0.45 : 0.30, green: hov ? 0.25 : 0.16, blue: hov ? 0.55 : 0.38, alpha: 1))
            } else if isToggle {
                ctx.setFillColor(CGColor(red: hov ? 0.25 : 0.14, green: hov ? 0.38 : 0.22, blue: hov ? 0.62 : 0.38, alpha: 1))
            } else {
                ctx.setFillColor(CGColor(red: hov ? 0.28 : 0.14, green: hov ? 0.30 : 0.15, blue: hov ? 0.40 : 0.21, alpha: 1))
            }
            ctx.addPath(CGPath(roundedRect: r, cornerWidth: 16, cornerHeight: 16, transform: nil))
            ctx.fillPath()
            var label = isExit ? "Salir del modo VR"
                : isToggle ? (desktopOn ? "Ocultar el escritorio de la Mac" : "Ver el escritorio de la Mac") : games[i].name
            if label.count > 34 { label = String(label.prefix(33)) + "…" }
            text(label, 64, r.minY + 28, 38, CGColor(red: 0.95, green: 0.95, blue: 1, alpha: 1), bold: hov)
        }
        if games.isEmpty {
            text("No hay juegos instalados en el Steam de QuestVRBridge", 48, H - 170 - 92 * CGFloat(nativeNow ? 2 : 1) - 50, 28, CGColor(red: 0.8, green: 0.8, blue: 0.85, alpha: 1))
        }
        if !st.isEmpty {
            text(st, 48, 48, 30, CGColor(red: 1, green: 0.85, blue: 0.3, alpha: 1))
        }
        listIdx = (listIdx + 1) % listTex.count
        if let data = ctx.data {
            listTex[listIdx].replace(region: MTLRegionMake2D(0, 0, TW, TH), mipmapLevel: 0, withBytes: data, bytesPerRow: TW * 4)
        }
        if nativeNow { sendListImage() }
    }

    private func corners(_ c: SIMD3<Float>, _ w: Float, _ h: Float) -> [SIMD4<Float>] {
        let r = aRight * (w / 2), u = SIMD3<Float>(0, h / 2, 0)
        return [SIMD4<Float>(c - r - u, 1), SIMD4<Float>(c + r - u, 1), SIMD4<Float>(c + r + u, 1), SIMD4<Float>(c - r + u, 1)]
    }

    func quads() -> [LobbyQuad] {
        if nativeNow || uiInHeadset { return [] }   // la app del Quest los muestra ella misma, más nítidos
        var q = [LobbyQuad(corners: corners(listC, listW, listH), texture: listTex[listIdx])]
        if desktopOn, let t = deskTex { q.append(LobbyQuad(corners: corners(deskC, deskW, deskW / deskAspect), texture: t)) }
        return q
    }

    func cubes() -> [Inst] {
        var out: [Inst] = []
        let grid = SIMD4<Float>(0.25, 0.25, 0.35, 1)
        for i in -5...5 {
            out.append(Inst(model: trs(Float(i), 0, 0, 0.015, 0.005, 10), color: grid))
            out.append(Inst(model: trs(0, 0, Float(i), 10, 0.005, 0.015), color: grid))
        }
        for m in hands { out.append(Inst(model: m * trs(0, 0, 0.03, 0.045, 0.045, 0.12), color: SIMD4<Float>(0.85, 0.85, 0.9, 1))) }
        for (o, d, len) in (nativeNow ? [] : lasers) {
            let a = simd_normalize(simd_cross(d, abs(d.y) < 0.9 ? SIMD3<Float>(0, 1, 0) : SIMD3<Float>(1, 0, 0)))
            let b = simd_cross(d, a)
            let mid = o + d * (len / 2)
            let m = simd_float4x4(columns: (SIMD4<Float>(a * 0.004, 0), SIMD4<Float>(b * 0.004, 0),
                                            SIMD4<Float>(d * len, 0), SIMD4<Float>(mid, 1)))
            out.append(Inst(model: m, color: SIMD4<Float>(0.4, 0.7, 1.0, 1)))
        }
        if !nativeNow, let c = cursor { out.append(Inst(model: trs(c.x, c.y, c.z, 0.02, 0.02, 0.02), color: SIMD4<Float>(1, 1, 1, 1))) }
        return out
    }
}

// MARK: - App nativa del Quest (sin navegador)
// La Mac avisa por UDP (puerto 9101) que está disponible; la app del Quest se conecta por TCP (9100).
// Mac -> Quest: [largo u32][1 video | 2 sonido | 3 texto JSON (config)]
// Quest -> Mac: [largo u32][JSON] (pose, key, log), mismo formato que la página web.
let NATIVE_PORT: UInt16 = 9100
let BEACON_PORT: UInt16 = 9101

// MARK: - Emparejar Quest y Mac
// La primera vez que un Quest se conecta, la Mac muestra un código de 6 números que hay que escribir en el Quest.
// Después, el Quest guarda una clave y se conecta solo. Los Quest emparejados quedan en emparejados.json
// (en la carpeta de datos); borrar ese archivo los olvida a todos.
final class Pairing {
    private let lock = NSLock()
    private let file = URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("emparejados.json")
    private var code: String?          // código vigente (uno a la vez)
    private var codeFor = ""           // para qué Quest
    private var codeUntil = Date.distantPast
    private var tries = 0

    private func load() -> [String: [String: String]] {
        guard let d = try? Data(contentsOf: file), let o = try? JSONSerialization.jsonObject(with: d) as? [String: [String: String]] else { return [:] }
        return o
    }
    private func save(_ o: [String: [String: String]]) {
        if let d = try? JSONSerialization.data(withJSONObject: o, options: [.prettyPrinted]) {
            try? d.write(to: file, options: .atomic)
            chmod(file.path, 0o600)
        }
    }
    private func random(_ n: Int) -> [UInt8] {
        var b = [UInt8](repeating: 0, count: n)
        if SecRandomCopyBytes(kSecRandomDefault, n, &b) != errSecSuccess { for i in 0..<n { b[i] = UInt8.random(in: 0...255) } }
        return b
    }
    private func eq(_ a: String, _ b: String) -> Bool {   // comparar sin delatar por el tiempo cuánto coincide
        let x = Array(a.utf8), y = Array(b.utf8)
        if x.count != y.count { return false }
        var d: UInt8 = 0
        for i in 0..<x.count { d |= x[i] ^ y[i] }
        return d == 0
    }

    enum Result { case ok(token: String?), needCode(wrong: Bool) }

    /// Revisa el primer mensaje del Quest
    func check(_ msg: [String: Any], ip: String) -> Result {
        lock.lock(); defer { lock.unlock() }
        guard let id = msg["id"] as? String, id.count >= 8, id.count <= 80 else { return .needCode(wrong: false) }
        let name = (msg["device"] as? String) ?? "Quest"
        var all = load()
        if let tok = msg["token"] as? String, let saved = all[id]?["token"], eq(tok, saved) { return .ok(token: nil) }
        if let c = msg["code"] as? String, let cur = code, Date() < codeUntil, codeFor == id {
            if eq(c, cur) {
                let tok = random(32).map { String(format: "%02x", $0) }.joined()
                let f = ISO8601DateFormatter()
                all[id] = ["token": tok, "name": name, "since": f.string(from: Date())]
                save(all)
                code = nil
                print("[emparejar] LISTO \(name) (\(ip))")
                return .ok(token: tok)
            }
            tries += 1
            if tries >= 5 { code = nil; print("[emparejar] VENCIDO") }   // demasiados intentos: código nuevo
            else { return .needCode(wrong: true) }
        }
        // Código nuevo (o el mismo, si sigue vigente para este Quest)
        if code == nil || Date() >= codeUntil || codeFor != id {
            let n = random(4).reduce(UInt32(0)) { $0 << 8 | UInt32($1) } % 1_000_000
            code = String(format: "%06u", n)
            codeFor = id; codeUntil = Date().addingTimeInterval(300); tries = 0
            print("[emparejar] CODIGO \(code!) \(name)")
        }
        return .needCode(wrong: msg["code"] != nil)
    }
}
let pairing = Pairing()

/// Direcciones IPv6 «link-local» (fe80::…) de la Mac en las redes cableadas (la del cable USB-C del Quest aparece
/// como una más). El Quest las usa para conectarse directo por el cable.
func linkLocalAddrs() -> [String] {
    var out: [String] = []
    var ifap: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&ifap) == 0 else { return out }
    var p = ifap
    while let cur = p {
        let i = cur.pointee
        p = i.ifa_next
        let name = String(cString: i.ifa_name)
        guard let a = i.ifa_addr, a.pointee.sa_family == sa_family_t(AF_INET6), (i.ifa_flags & UInt32(IFF_UP)) != 0,
              (i.ifa_flags & UInt32(IFF_LOOPBACK)) == 0,
              !["awdl", "llw", "utun", "anpi", "ap", "gif", "stf", "bridge", "lo"].contains(where: { name.hasPrefix($0) }) else { continue }
        var a6 = a.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { $0.pointee.sin6_addr }
        var buf = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))
        inet_ntop(AF_INET6, &a6, &buf, socklen_t(buf.count))
        var s = String(cString: buf)
        if let pc = s.firstIndex(of: "%") { s = String(s[..<pc]) }
        guard s.hasPrefix("fe80") else { continue }
        // (macOS guarda la interfaz dentro de la dirección: fe80:N::… → fe80::…)
        if s.hasPrefix("fe80:") && !s.hasPrefix("fe80::") {
            var b = a6
            withUnsafeMutableBytes(of: &b) { $0[2] = 0; $0[3] = 0 }
            inet_ntop(AF_INET6, &b, &buf, socklen_t(buf.count))
            s = String(cString: buf)
        }
        if !out.contains(s) { out.append(s) }
    }
    freeifaddrs(ifap)
    return out
}
let CABLE_ONLY = ProcessInfo.processInfo.environment["VR_CABLE_ONLY"] == "1"

final class NativeLink {
    private var fd: Int32 = -1
    private let lock = NSLock()
    private var pending = 0
    private var needKey = true
    private let sendQueue = DispatchQueue(label: "native-send", qos: .userInteractive)
    private let shared: Shared
    private var statSent = 0, statDropped = 0, statWaitKey = 0
    var onConnect: (() -> Void)?
    var onDeskDrop: ((Data) -> Void)?
    var onDisconnect: (() -> Void)?

    init(shared: Shared) {
        self.shared = shared
        Thread.detachNewThread { [self] in
            while true {
                sleep(5)
                lock.lock(); let c = fd >= 0; let a = statSent, d = statDropped, w = statWaitKey, pe = pending
                statSent = 0; statDropped = 0; statWaitKey = 0; lock.unlock()
                if c { print("[quest] últimos 5 s: \(a) cuadros enviados, \(d) descartados por red, \(w) esperando cuadro completo, \(pe / 1024) KB en cola") }
                // Con un juego abierto: en qué se va el tiempo de cada cuadro (lo mismo que el indicador del visor)
                let perf = Perf.shared.snapshot()
                if c && a > 0, JSONSerialization.isValidJSONObject(perf), let j = try? JSONSerialization.data(withJSONObject: perf, options: [.sortedKeys]),
                   let t = String(data: j, encoding: .utf8) { print("[rendimiento] \(t)") }
            }
        }
    }

    var isConnected: Bool { lock.lock(); defer { lock.unlock() }; return fd >= 0 }

    /// Nunca bloquea al codificador: si el Wi-Fi no da abasto, descarta video y pide un cuadro completo.
    func send(_ p: Data) {
        guard let t = p.first else { return }
        lock.lock()
        guard fd >= 0 else { lock.unlock(); return }
        if t == 1 {
            let key = p.count > 5 && p[p.startIndex + 5] == 1
            if needKey && !key { statWaitKey += 1; lock.unlock(); return }
            if pending > 4_000_000 { statDropped += 1; needKey = true; lock.unlock(); shared.requestKey(); return }
            needKey = false
            statSent += 1
        } else if t == 2 && pending > 1_000_000 {
            lock.unlock(); return
        } else if (t == 7 || t == 8) && pending > 3_000_000 {
            lock.unlock(); onDeskDrop?(p); return
        }
        let size = p.count + 4
        pending += size
        let f = fd
        lock.unlock()
        sendQueue.async { [self] in
            var len = UInt32(p.count).bigEndian
            var buf = Data(bytes: &len, count: 4)
            buf.append(p)
            let ok = buf.withUnsafeBytes { raw -> Bool in
                var off = 0
                while off < raw.count {
                    let n = Darwin.send(f, raw.baseAddress! + off, raw.count - off, 0)
                    if n <= 0 { return false }
                    off += n
                }
                return true
            }
            lock.lock(); pending -= size; lock.unlock()
            if !ok { Darwin.shutdown(f, SHUT_RDWR) }
        }
    }

    private func readExact(_ fd: Int32, _ n: Int) -> Data? {
        var d = Data(count: n)
        var off = 0
        while off < n {
            let r = d.withUnsafeMutableBytes { Darwin.recv(fd, $0.baseAddress! + off, n - off, 0) }
            if r <= 0 { return nil }
            off += r
        }
        return d
    }

    func start(handler: @escaping (Data) -> Void) {
        // Aviso en la red local para que la app del Quest encuentre la Mac sola
        Thread.detachNewThread { [self] in
            let s = socket(AF_INET, SOCK_DGRAM, 0)
            var one: Int32 = 1
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, socklen_t(MemoryLayout<Int32>.size))
            var addr = sockaddr_in()
            addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            addr.sin_family = sa_family_t(AF_INET)
            addr.sin_port = BEACON_PORT.bigEndian
            addr.sin_addr.s_addr = in_addr_t(0xffffffff)
            let name = (Host.current().localizedName ?? "Mac").replacingOccurrences(of: "\"", with: "")
            var msg = Array("{\"qvb\":1,\"port\":\(NATIVE_PORT),\"name\": \"\(name)\"}".utf8)
            var warned = false
            var announced = false
            while true {
                if !isConnected {
                    // (con las direcciones de la Mac en el cable, para conectarse por ahí)
                    let ll = linkLocalAddrs().map { "\"\($0)\"" }.joined(separator: ",")
                    msg = Array("{\"qvb\":1,\"port\":\(NATIVE_PORT),\"name\": \"\(name)\",\"ll\":[\(ll)]}".utf8)
                    // A la dirección general y a la de cada red de la Mac (Wi-Fi, Ethernet...),
                    // porque la general a veces sale por otra interfaz.
                    var targets: [in_addr_t] = [in_addr_t(0xffffffff)]
                    var ifap: UnsafeMutablePointer<ifaddrs>?
                    if getifaddrs(&ifap) == 0 {
                        var p = ifap
                        while let cur = p {
                            let i = cur.pointee
                            if let a = i.ifa_addr, a.pointee.sa_family == sa_family_t(AF_INET),
                               (i.ifa_flags & UInt32(IFF_BROADCAST)) != 0, (i.ifa_flags & UInt32(IFF_UP)) != 0,
                               (i.ifa_flags & UInt32(IFF_LOOPBACK)) == 0, let b = i.ifa_dstaddr {
                                b.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { targets.append($0.pointee.sin_addr.s_addr) }
                            }
                            p = i.ifa_next
                        }
                        freeifaddrs(ifap)
                    }
                    if !announced {
                        announced = true
                        let names = targets.map { t -> String in let a = in_addr(s_addr: t); return String(cString: inet_ntoa(a)) }
                        print("[quest] Avisando en la red local para la app del Quest: \(names.joined(separator: ", "))")
                    }
                    for t in targets {
                        addr.sin_addr.s_addr = t
                        let r = withUnsafePointer(to: &addr) {
                            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                                sendto(s, msg, msg.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                            }
                        }
                        if r < 0 && !warned { warned = true; print("[quest] No pude avisar en la red local (¿permiso de Red local?): errno \(errno)") }
                    }
                } else { announced = false }
                sleep(1)
            }
        }
        Thread.detachNewThread { [self] in
            // IPv6 y IPv4 en el mismo puerto: por Wi-Fi llega por IPv4; por el cable USB-C, por IPv6 (link-local)
            let ls = socket(AF_INET6, SOCK_STREAM, 0)
            var one: Int32 = 1, zero: Int32 = 0
            setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
            setsockopt(ls, Int32(IPPROTO_IPV6), IPV6_V6ONLY, &zero, socklen_t(MemoryLayout<Int32>.size))
            var addr = sockaddr_in6()
            addr.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
            addr.sin6_family = sa_family_t(AF_INET6)
            addr.sin6_port = NATIVE_PORT.bigEndian
            addr.sin6_addr = in6addr_any
            let r = withUnsafePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.bind(ls, $0, socklen_t(MemoryLayout<sockaddr_in6>.size))
                }
            }
            if r != 0 || listen(ls, 4) != 0 { print("[quest] No se pudo abrir el puerto \(NATIVE_PORT) para la app del Quest"); return }
            while true {
                var peer = sockaddr_in6()
                var plen = socklen_t(MemoryLayout<sockaddr_in6>.size)
                let c = withUnsafeMutablePointer(to: &peer) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { accept(ls, $0, &plen) }
                }
                if c < 0 { continue }
                setsockopt(c, Int32(IPPROTO_TCP), TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
                var big: Int32 = 4 << 20
                setsockopt(c, SOL_SOCKET, SO_SNDBUF, &big, socklen_t(MemoryLayout<Int32>.size))
                var ipBuf = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))
                var a6 = peer.sin6_addr
                inet_ntop(AF_INET6, &a6, &ipBuf, socklen_t(ipBuf.count))
                var ip = String(cString: ipBuf)
                if ip.hasPrefix("::ffff:") { ip = String(ip.dropFirst(7)) }   // IPv4 (Wi-Fi)
                if ip.hasPrefix("fe80") { print("[quest] Conexión por el cable USB-C") }
                else if CABLE_ONLY {
                    print("[quest] Conexión por Wi-Fi rechazada (VRmin Host: «Solo por cable USB-C»)")
                    Darwin.close(c); continue
                }
                // Emparejado: el primer mensaje del Quest trae su clave (o el código que le mostramos)
                var tv = timeval(tv_sec: 5, tv_usec: 0)
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
                var authOK = false
                var newToken: String?
                if let h = readExact(c, 4) {
                    let n = Int(h[0]) << 24 | Int(h[1]) << 16 | Int(h[2]) << 8 | Int(h[3])
                    if n > 0, n < 4096, let body = readExact(c, n),
                       let o = try? JSONSerialization.jsonObject(with: body) as? [String: Any], (o["type"] as? String) == "auth" {
                        switch pairing.check(o, ip: ip) {
                        case .ok(let t): authOK = true; newToken = t
                        case .needCode(let wrong):
                            var m = Data([3]); m.append(Data("{\"type\":\"auth\",\"ok\":false,\"need\":\"code\",\"wrong\":\(wrong)}".utf8))
                            var len = UInt32(m.count).bigEndian
                            var out = Data(bytes: &len, count: 4); out.append(m)
                            _ = out.withUnsafeBytes { Darwin.send(c, $0.baseAddress!, $0.count, 0) }
                        }
                    }
                }
                if !authOK { print("[quest] Conexión de \(ip) sin emparejar: se pidió el código"); Darwin.close(c); continue }
                tv = timeval(tv_sec: 0, tv_usec: 0)
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
                // Una sola app a la vez: la nueva reemplaza a la anterior
                lock.lock()
                if fd >= 0 { Darwin.shutdown(fd, SHUT_RDWR) }
                fd = c; needKey = true; pending = 0
                lock.unlock()
                print("[quest] App del Quest conectada desde \(ip)")
                var okMsg = Data([3])
                okMsg.append(Data((newToken.map { "{\"type\":\"auth\",\"ok\":true,\"token\":\"\($0)\"}" } ?? "{\"type\":\"auth\",\"ok\":true}").utf8))
                send(okMsg)
                var cfg = Data([3])
                cfg.append(Data("{\"type\":\"config\",\"eye\":\(EYE),\"fps\":\(FPS),\"vw\":\(W),\"vh\":\(H),\"fov\":\"\(FOVEA.on ? FOVEA.text : "0")\",\"codec\":\"\(CODEC_HEVC ? "hevc" : "h264")\"}".utf8))
                send(cfg)
                shared.requestKey()
                onConnect?()
                Thread.detachNewThread { [self] in
                    while true {
                        guard let h = readExact(c, 4) else { break }
                        let n = Int(h[0]) << 24 | Int(h[1]) << 16 | Int(h[2]) << 8 | Int(h[3])
                        if n <= 0 || n > 1 << 20 { break }
                        guard let body = readExact(c, n) else { break }
                        handler(body)
                    }
                    sendQueue.sync {}   // que no quede nada escribiendo en este socket
                    lock.lock(); let gone = fd == c; if gone { fd = -1 }; lock.unlock()
                    Darwin.close(c)
                    if gone { onDisconnect?() }
                    print("[quest] App del Quest desconectada")
                }
            }
        }
    }
}

// MARK: - Mensajes del Quest
struct InView: Decodable { let P: [Float]; let V: [Float] }
// MARK: - Rendimiento (para el indicador del visor)
/// Junta los tiempos de cada etapa: lo que mide el runtime del juego (espera, dibujo, copia) y lo de acá (copia y video)
final class Perf {
    static let shared = Perf()
    private let lock = NSLock()
    private var runtime: [String: Any] = [:]
    private var sumMac = 0.0, nMac = 0, sumEnc = 0.0, nEnc = 0, dropped = 0
    private var last: [String: Any] = [:], lastAt: UInt64 = 0
    func setRuntime(_ d: Data) {
        guard let o = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { return }
        lock.lock(); runtime = o; lock.unlock()
    }
    func addMac(_ ms: Double) { lock.lock(); sumMac += ms; nMac += 1; lock.unlock() }
    func addEnc(_ ms: Double) { lock.lock(); sumEnc += ms; nEnc += 1; lock.unlock() }
    func addDrop() { lock.lock(); dropped += 1; lock.unlock() }
    /// Promedios del último segundo (se calculan una vez por segundo)
    func snapshot() -> [String: Any] {
        lock.lock(); defer { lock.unlock() }
        let now = DispatchTime.now().uptimeNanoseconds
        if now - lastAt < 1_000_000_000 { return last }
        var o = runtime
        o["mac"] = nMac > 0 ? (sumMac / Double(nMac) * 100).rounded() / 100 : 0
        o["video"] = nEnc > 0 ? (sumEnc / Double(nEnc) * 100).rounded() / 100 : 0
        o["recibidos"] = nMac
        o["saltados"] = dropped
        sumMac = 0; nMac = 0; sumEnc = 0; nEnc = 0; dropped = 0
        last = o; lastAt = now
        return o
    }
}

// MARK: - Teclado del visor
/// Escribe en la Mac lo que se toca en el teclado del Quest, y avisa cuándo hay un campo de texto activo
/// (para abrir el teclado solo). Usa el permiso de Accesibilidad (el mismo del mouse).
final class Typing {
    static let shared = Typing()
    private(set) var active = false
    private let q = DispatchQueue(label: "typing", qos: .utility)
    private init() {
        // Cada medio segundo: ¿lo que tiene el foco es un lugar para escribir?
        let t = DispatchSource.makeTimerSource(queue: q)
        t.schedule(deadline: .now() + 1, repeating: 0.5)
        t.setEventHandler { [weak self] in self?.active = Typing.focusedIsText() }
        t.resume()
        timer = t
    }
    private var timer: DispatchSourceTimer?

    private static func focusedIsText() -> Bool {
        guard AXIsProcessTrusted() else { return false }
        let sys = AXUIElementCreateSystemWide()
        AXUIElementSetMessagingTimeout(sys, 0.25)
        var f: CFTypeRef?
        guard AXUIElementCopyAttributeValue(sys, kAXFocusedUIElementAttribute as CFString, &f) == .success, let fv = f,
              CFGetTypeID(fv) == AXUIElementGetTypeID() else { return false }
        let el = fv as! AXUIElement
        AXUIElementSetMessagingTimeout(el, 0.25)
        var r: CFTypeRef?, sr: CFTypeRef?
        AXUIElementCopyAttributeValue(el, kAXRoleAttribute as CFString, &r)
        AXUIElementCopyAttributeValue(el, kAXSubroleAttribute as CFString, &sr)
        let role = r as? String ?? "", sub = sr as? String ?? ""
        if ["AXTextField", "AXTextArea", "AXComboBox", "AXSearchField"].contains(role) || sub == "AXSearchField" || sub == "AXSecureTextField" { return true }
        // Otros elementos que se pueden editar (por ejemplo, editores web)
        var ed: DarwinBoolean = false
        if role == "AXWebArea", AXUIElementIsAttributeSettable(el, kAXValueAttribute as CFString, &ed) == .success, ed.boolValue { return true }
        return false
    }

    private static let codes: [String: CGKeyCode] = [
        "return": 36, "tab": 48, "space": 49, "delete": 51, "escape": 53, "fdelete": 117,
        "left": 123, "right": 124, "down": 125, "up": 126, "home": 115, "end": 119,
        "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7, "c": 8, "v": 9, "b": 11, "q": 12, "w": 13,
        "e": 14, "r": 15, "y": 16, "t": 17, "o": 31, "u": 32, "i": 34, "p": 35, "l": 37, "j": 38, "k": 40, "n": 45, "m": 46]

    static func post(text: String?, key: String?, mods: [String]) {
        var flags: CGEventFlags = []
        for m in mods {
            switch m { case "cmd": flags.insert(.maskCommand); case "shift": flags.insert(.maskShift)
            case "alt": flags.insert(.maskAlternate); case "ctrl": flags.insert(.maskControl); default: break }
        }
        let src = CGEventSource(stateID: .hidSystemState)
        if let key, let code = codes[key] {
            let d = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: true), u = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: false)
            d?.flags = flags; u?.flags = flags
            d?.post(tap: .cghidEventTap); u?.post(tap: .cghidEventTap)
        } else if let text, !text.isEmpty {
            // Texto: se manda como caracteres (no depende de la distribución del teclado de la Mac)
            for ch in text {
                let utf16 = Array(String(ch).utf16)
                let d = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: true), u = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: false)
                d?.keyboardSetUnicodeString(stringLength: utf16.count, unicodeString: utf16)
                u?.keyboardSetUnicodeString(stringLength: utf16.count, unicodeString: utf16)
                d?.flags = flags; u?.flags = flags
                d?.post(tap: .cghidEventTap); u?.post(tap: .cghidEventTap)
            }
        }
    }
}

/// Un pedido para VRmin Host (la app de la Mac escucha en el puerto 9102)
func toHost(_ o: [String: Any]) {
    guard let d = try? JSONSerialization.data(withJSONObject: o) else { return }
    let s = socket(AF_INET, SOCK_DGRAM, 0)
    var a = sockaddr_in()
    a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET)
    a.sin_port = UInt16(9102).bigEndian; a.sin_addr.s_addr = inet_addr("127.0.0.1")
    _ = d.withUnsafeBytes { b in withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(s, b.baseAddress, d.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } } }
    close(s)
}

struct InMsg: Decodable {
    let type: String; let id: UInt32?; let views: [InView]?; let msg: String?; let xr: [Float]?
    // Ventana 2D del Quest
    let mode: String?; let game: String?; let on: Bool?; let u: Float?; let v: Float?; let down: Bool?; let scroll: Float?
    let w: Int?; let h: Int?
    let eye: Int?; let fps: Int?; let mbps: Int?; let maxw: Int?; let audioMac: Bool?; let audio: String?
    let fov: String?   // foveated encoding: "nivel,centro,cx izq,cx der,cy"
    let codec: String?   // "hevc" o "h264"
    // Ajustes avanzados de los juegos
    let scope: String?; let scale: Double?; let auto: Bool?; let fovk: Double?; let index: Bool?; let vive: Bool?; let reset: Bool?
    // Teclado del visor: texto a escribir, o una tecla especial («return», «delete»…) con modificadores («cmd», «shift»…)
    let text: String?; let k: String?; let mods: [String]?
    /// Cambiar de monitor: a qué pantalla (id con el bit de pantalla, o sin él)
    let to: UInt32?
    /// Mensaje armado acá mismo (por ejemplo, apagar el escritorio al conectar la ventana del Quest)
    static func desk(_ on: Bool) -> InMsg {
        try! JSONDecoder().decode(InMsg.self, from: Data("{\"type\":\"desk\",\"on\":\(on)}".utf8))
    }
}

// MARK: - Arranque
let shared = Shared()
let link = Link()
guard let encoder = Encoder(link: link, shared: shared),
      let renderer = Renderer(encoder: encoder, shared: shared) else { exit(1) }
renderer.lobby = Lobby(device: renderer.device, shared: shared)

let game = GameLink(encoder: encoder, shared: shared)
game.start(port: GAME_PORT)

// Sonido: por defecto va solo al Quest. VR_AUDIO=0 lo apaga; VR_AUDIO_MAC=1 lo deja sonar también en la Mac.
var audioTap: AnyObject? = nil
do {
    if #available(macOS 14.2, *) {
        let a = AudioTap(link: link, muteMac: env["VR_AUDIO_MAC"] != "1")
        a.gameEnabled = env["VR_AUDIO"] != "0"
        a.start()
        audioTap = a
        print("Sonido: esperando a que un juego de Wine haga ruido para mandarlo al Quest")
    } else {
        print("Sonido al Quest: necesita macOS 14.2 o más nuevo")
    }
}

// Mensajes del visor (de la página web o de la app nativa)
let renderQueue = DispatchQueue(label: "render", qos: .userInteractive)
let questDecoder = JSONDecoder()
let questLock = NSLock()
func handleQuestMessage(_ line: Data) {
    questLock.lock(); defer { questLock.unlock() }
    guard let m = try? questDecoder.decode(InMsg.self, from: line) else { return }
    if m.type == "key" {
        shared.requestKey()
    } else if m.type == "deskkey" {
        renderer.lobby?.requestDeskKey()
    } else if m.type == "log", let t = m.msg {
        print("[visor] " + t)
    } else if m.type == "framegen" {
        game.halfRate = m.on ?? false
    } else if m.type == "hello" {
        let is2D = m.mode == "2d", isRoom = m.mode == "vr2"
        renderQueue.async {
            renderer.lobby?.mode2D = is2D
            renderer.lobby?.uiInHeadset = isRoom
            if is2D || isRoom { renderer.lobby?.handle2D(InMsg.desk(false)) }   // el escritorio arranca oculto
        }
        print(is2D ? "[quest] Ventana del Quest (2D): lista de juegos y escritorio" : isRoom ? "[quest] Sala VR con menú y ventanas" : "[quest] Modo VR")
    } else if m.type == "rejoin" {
        // «Volver al juego»: el runtime del juego rearma su conexión de imágenes y se pide un cuadro completo
        game.sendLine(Data("{\"resetimg\":1}".utf8))
        shared.requestKey()
        print("[quest] Volver al juego: rearmando la imagen del juego")
    } else if m.type == "vigia" {
        if let on = m.on { Vigia.shared.set(on); if on { shared.requestKey() } }
        if let t = m.msg { Vigia.shared.say(t) }
    } else if m.type == "gamecfg" {   // Ajustes › Avanzado (desde el Quest)
        GameProfiles.shared.apply(scope: m.scope ?? "all", scale: m.scale, auto: m.auto, fovk: m.fovk, index: m.index, vive: m.vive, reset: m.reset ?? false,
                                  eye: m.eye, fps: m.fps, mbps: m.mbps, fov: m.fov)
        // Calidad propia del juego abierto (desde su menú): VRmin Host reinicia con esa, sin volver a abrir el juego
        let cur = GameProfiles.shared.current
        if m.scope == "game" && !cur.isEmpty && (m.eye != nil || m.fps != nil || m.mbps != nil || m.fov != nil || m.reset == true) {
            let q = GameProfiles.shared.quality(for: cur)
            if q.differs { toHost(["qvb": "playgame", "name": cur, "eye": q.eye, "fps": q.fps, "mbps": q.mbps, "fov": q.fov]) }
        }
    } else if m.type == "record" {   // ajustes rápidos del visor: VRmin Host empieza o termina la grabación
        print("[grabar] \(m.on == true ? "empezar" : "parar")")
    } else if m.type == "kb" {
        Typing.post(text: m.text, key: m.k, mods: m.mods ?? [])
    } else if m.type == "winkey", let id = m.id {
        renderQueue.async { renderer.lobby?.requestWinKey(id) }
    } else if ["launch", "report", "quitgame", "desk", "mouse", "win", "wmouse", "wsize", "monitor", "depth3d", "deskcfg", "settings", "deskdisp", "dispswap"].contains(m.type) {
        renderQueue.async { renderer.lobby?.handle2D(m) }
    } else if m.type == "pose", let id = m.id, let vs = m.views {
        if Vigia.shared.on { Vigia.shared.lastPoseAt = Date() }
        if renderer.lobby?.mode2D == true { renderQueue.async { renderer.lobby?.mode2D = false } }
        game.sendLine(line)
        if let x = m.xr { shared.setXR(x) }
        let eyes = vs.compactMap { v -> EyeView? in
            guard let p = mat4(v.P), let w = mat4(v.V) else { return nil }
            return EyeView(proj: p, view: w)
        }
        if eyes.count >= 2 { shared.setPose(Pose(id: id, eyes: eyes)) }
    }
}

let native = NativeLink(shared: shared)
Link.mirror = { native.send($0) }
HeadCam.shared.send = { var d = Data([3]); d.append(Data($0.utf8)); native.send(d) }
renderer.lobby?.isNative = { native.isConnected }
renderer.lobby?.sendNative = { native.send($0) }
native.onConnect = { renderQueue.async { renderer.lobby?.mode2D = false; renderer.lobby?.uiInHeadset = false; renderer.lobby?.stopWindows() }; renderer.lobby?.requestResend() }
renderer.lobby?.queue = renderQueue
if #available(macOS 14.2, *), let a = audioTap as? AudioTap {
    a.gameActive = { shared.gameActive }
    a.deskActive = { native.isConnected && (renderer.lobby?.mode2D == true || renderer.lobby?.uiInHeadset == true) }
}
native.onDisconnect = { renderQueue.async { renderer.lobby?.stopWindows(); renderer.lobby?.stopMonitor() } }
native.onDeskDrop = { p in
    // Se descartó un cuadro del escritorio o de una ventana: pedir uno completo de ese mismo video
    if p.first == 8, p.count >= 5 {
        let i = p.startIndex
        let id = UInt32(p[i + 1]) << 24 | UInt32(p[i + 2]) << 16 | UInt32(p[i + 3]) << 8 | UInt32(p[i + 4])
        renderQueue.async { renderer.lobby?.requestWinKey(id) }
    } else { renderer.lobby?.requestDeskKey() }
}
native.start(handler: handleQuestMessage)

let t0 = DispatchTime.now().uptimeNanoseconds
let timer = DispatchSource.makeTimerSource(flags: .strict, queue: renderQueue)
timer.schedule(deadline: .now(), repeating: .nanoseconds(1_000_000_000 / FPS), leeway: .microseconds(500))
timer.setEventHandler {
    guard link.isConnected || native.isConnected else { return }
    // Ventana 2D del Quest: no hay visor que dibujar, solo la lista y el escritorio
    if native.isConnected, let l = renderer.lobby, l.mode2D || l.uiInHeadset {
        l.tick2D(gameActive: shared.gameActive)
        // Ventana 2D, o sala VR (la habitación la dibuja el Quest): no hace falta dibujar ni mandar video de la sala
        if l.mode2D || l.uiInHeadset { if shared.gameActive { l.setHidden() }; return }
    }
    if shared.gameActive { renderer.lobby?.setHidden(); return }
    guard let p = shared.pose, p.eyes.count >= 2 else { return }
    let t = Float(Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e9)
    renderer.render(pose: p, t: t)
}
timer.resume()

Thread.detachNewThread {
    var warned = false
    while true {
        if link.open(port: SERVER_PORT) {
            print("Renderizador conectado al servidor. Esperando poses del Quest…")
            warned = false
            shared.requestKey()
            link.readLines { line in handleQuestMessage(line) }
            print("Se cortó la conexión con el servidor; reintentando…")
        } else if !warned {
            print("Esperando a que arranque server.py…")
            warned = true
        }
        sleep(1)
    }
}

// Hilo principal: una app sin ícono en el Dock (para poder mostrar la ventana del Modo Centinela)
Vigia.shared.onExit = { shared.requestKey() }
Vigia.shared.listGames = { (renderer.lobby?.gameList() ?? []).filter { $0.vr } }   // (el Modo Centinela es solo para VR)
Vigia.shared.launchGame = { renderer.lobby?.handle2D($0) }
let nsApp = NSApplication.shared
nsApp.setActivationPolicy(.accessory)
nsApp.run()
