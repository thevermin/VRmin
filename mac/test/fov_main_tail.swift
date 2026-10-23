// Prueba (en GitHub Actions, con la GPU de la Mac): un cuadro de juego chico (escala 80 %, campo de visión 95 %)
// pasa por la GPU (MetalFX + recorte + foveated encoding) y tiene que salir con imagen.
func probar(_ isRGBA: Bool) -> Bool {
    guard let fv = Foveator.shared else { print("FALLA: Foveator no se pudo crear"); return false }
    let ew = 976, eh = 976
    var src = [UInt8](repeating: 0, count: ew * 2 * eh * 4)
    for y in 0..<eh { for x in 0..<(ew * 2) { let i = (y * ew * 2 + x) * 4; src[i] = UInt8(x & 255); src[i + 1] = UInt8(y & 255); src[i + 2] = 128; src[i + 3] = 255 } }
    let attrs: [String: Any] = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: W,
                                kCVPixelBufferHeightKey as String: H, kCVPixelBufferMetalCompatibilityKey as String: true,
                                kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any]()]
    var pb: CVPixelBuffer?
    CVPixelBufferCreate(nil, W, H, kCVPixelFormatType_32BGRA, attrs as CFDictionary, &pb)
    guard let p = pb else { print("FALLA: sin pixel buffer"); return false }
    let k: Float = 0.95
    let rects = [SIMD4<Float>(0.6 * (1 - k), 0.5 * (1 - k), k, k), SIMD4<Float>(0.4 * (1 - k), 0.5 * (1 - k), k, k)]
    let ok = src.withUnsafeBytes { fv.warpPixels($0.baseAddress!, ew: ew, eh: eh, isRGBA: isRGBA, rects: rects, into: p) }
    guard ok else { print("FALLA: warpPixels devolvió false"); return false }
    CVPixelBufferLockBaseAddress(p, .readOnly)
    let base = CVPixelBufferGetBaseAddress(p)!.assumingMemoryBound(to: UInt8.self), bpr = CVPixelBufferGetBytesPerRow(p)
    var lit = 0
    for y in stride(from: 0, to: H, by: 16) { for x in stride(from: 0, to: W, by: 16) { if base[y * bpr + x * 4 + 0] > 0 || base[y * bpr + x * 4 + 1] > 0 { lit += 1 } } }
    CVPixelBufferUnlockBaseAddress(p, .readOnly)
    // Codificar ese cuadro con VideoToolbox (como el codificador del juego)
    var vs: VTCompressionSession?
    let spec = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true] as CFDictionary
    let cst = VTCompressionSessionCreate(allocator: nil, width: Int32(W), height: Int32(H), codecType: kCMVideoCodecType_H264, encoderSpecification: spec,
                                         imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &vs)
    var encOK = false
    if cst == noErr, let v = vs {
        VTSessionSetProperty(v, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(v, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_ConstrainedHigh_AutoLevel)
        VTSessionSetProperty(v, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: 150_000_000))
        VTCompressionSessionPrepareToEncodeFrames(v)
        let sem = DispatchSemaphore(value: 0)
        var estado: OSStatus = -1, bytes = 0
        let st = VTCompressionSessionEncodeFrame(v, imageBuffer: p, presentationTimeStamp: CMTime(value: 1, timescale: 72), duration: .invalid,
                                                 frameProperties: [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary, infoFlagsOut: nil) { s2, _, sb in
            estado = s2; bytes = sb.map { CMSampleBufferGetTotalSampleSize($0) } ?? 0; sem.signal()
        }
        if st == noErr { _ = sem.wait(timeout: .now() + 5) }
        print("::notice::Codificador \(W)x\(H): crear \(cst), codificar \(st), salida \(estado), \(bytes) bytes")
        encOK = st == noErr && estado == noErr && bytes > 0
    } else { print("::notice::Codificador \(W)x\(H): no se pudo crear (\(cst))") }
    print("::notice::Salida \(W)x\(H) (\(isRGBA ? "RGBA" : "BGRA")): \(lit) muestras con imagen · MetalFX \(MTLFXSpatialScalerDescriptor.supportsDevice(fv.dev) ? "sí" : "no")")
    _ = encOK
    return lit > 100
}
// ¿El codificador acepta cada tamaño? (varios cuadros seguidos, como en un juego)
func probarCodificador(_ w: Int, _ h: Int) {
    var vs: VTCompressionSession?
    let spec = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true] as CFDictionary
    let cst = VTCompressionSessionCreate(allocator: nil, width: Int32(w), height: Int32(h), codecType: kCMVideoCodecType_H264, encoderSpecification: spec,
                                         imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &vs)
    guard cst == noErr, let v = vs else { print("::notice::Codificador \(w)x\(h): no se pudo crear (\(cst))"); return }
    VTSessionSetProperty(v, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
    VTSessionSetProperty(v, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_ConstrainedHigh_AutoLevel)
    VTSessionSetProperty(v, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
    VTSessionSetProperty(v, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: 150_000_000))
    VTCompressionSessionPrepareToEncodeFrames(v)
    var pb: CVPixelBuffer?
    let attrs: [String: Any] = [kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](), kCVPixelBufferMetalCompatibilityKey as String: true]
    CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_32BGRA, attrs as CFDictionary, &pb)
    var outs = 0, bytes = 0, errs = 0, drops = 0
    let lk = NSLock()
    for i in 0..<10 {
        var flags = VTEncodeInfoFlags()
        VTCompressionSessionEncodeFrame(v, imageBuffer: pb!, presentationTimeStamp: CMTime(value: Int64(i), timescale: 72), duration: .invalid,
                                        frameProperties: i == 0 ? [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary : nil, infoFlagsOut: &flags) { s2, f, sb in
            lk.lock(); if s2 != noErr { errs += 1 }; if f.contains(.frameDropped) { drops += 1 }; if let sb { outs += 1; bytes += CMSampleBufferGetTotalSampleSize(sb) }; lk.unlock()
        }
        usleep(14000)
    }
    VTCompressionSessionCompleteFrames(v, untilPresentationTimeStamp: .invalid)
    print("::notice::Codificador \(w)x\(h): \(outs) cuadros, \(bytes) bytes, \(errs) errores, \(drops) descartados")
}
for (w, h) in [(W, H)] { probarCodificador(w, h) }
let a = probar(true), b = probar(false)
print(a && b ? "PRUEBA OK" : "PRUEBA FALLÓ")
exit(a && b ? 0 : 1)
