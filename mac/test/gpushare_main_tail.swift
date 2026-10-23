// Prueba de la copia directa (CI): un proceso comparte una textura de Metal con un nombre en el sistema (como DXMT)
// y otro la abre con GpuShare.open y lee los colores.
//   gpusharetest servidor NOMBRE   |   gpusharetest cliente NOMBRE
let args = CommandLine.arguments
guard args.count >= 3, let dev = MTLCreateSystemDefaultDevice(), let q = dev.makeCommandQueue() else { print("sin GPU"); exit(2) }
let name = args[2]
let W = 64, H = 32
func pattern() -> [UInt8] { var b = [UInt8](repeating: 0, count: W * H * 4); for i in 0..<(W * H) { b[i * 4] = UInt8(i % 251); b[i * 4 + 1] = 7; b[i * 4 + 2] = UInt8((i / W) * 5 % 256); b[i * 4 + 3] = 255 }; return b }
if args[1] == "servidor" {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: W, height: H, mipmapped: false)
    d.usage = [.shaderRead]; d.storageMode = .private
    guard let t = dev.makeSharedTexture(descriptor: d) else { print("makeSharedTexture falló"); exit(3) }
    let p = pattern()
    let buf = dev.makeBuffer(bytes: p, length: p.count, options: .storageModeShared)!
    let cb = q.makeCommandBuffer()!, bl = cb.makeBlitCommandEncoder()!
    bl.copy(from: buf, sourceOffset: 0, sourceBytesPerRow: W * 4, sourceBytesPerImage: W * H * 4, sourceSize: MTLSize(width: W, height: H, depth: 1),
            to: t, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
    bl.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    guard let h = t.makeSharedTextureHandle() else { print("sin handle"); exit(4) }
    typealias PortFn = @convention(c) (AnyObject, Selector) -> mach_port_t
    let sel = NSSelectorFromString("createMachPort")
    guard let m = class_getInstanceMethod(type(of: h), sel) else { print("sin createMachPort"); exit(5) }
    let port = unsafeBitCast(method_getImplementation(m), to: PortFn.self)(h, sel)
    typealias Reg = @convention(c) (mach_port_t, UnsafePointer<CChar>, mach_port_t, Int32) -> kern_return_t
    var bp: mach_port_t = 0
    _ = task_get_special_port(mach_task_self_, TASK_BOOTSTRAP_PORT, &bp)
    let kr = name.withCString { unsafeBitCast(dlsym(UnsafeMutableRawPointer(bitPattern: -2), "bootstrap_register2")!, to: Reg.self)(bp, $0, port, 0) }
    print("servidor: registrado (\(kr))"); fflush(stdout)
    sleep(20)
    exit(0)
} else {
    guard let t = GpuShare.open(name, device: dev) else { print("cliente: no se pudo abrir"); exit(6) }
    let buf = dev.makeBuffer(length: W * H * 4, options: .storageModeShared)!
    let cb = q.makeCommandBuffer()!, bl = cb.makeBlitCommandEncoder()!
    bl.copy(from: t, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0), sourceSize: MTLSize(width: W, height: H, depth: 1),
            to: buf, destinationOffset: 0, destinationBytesPerRow: W * 4, destinationBytesPerImage: W * H * 4)
    bl.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    let got = Array(UnsafeBufferPointer(start: buf.contents().assumingMemoryBound(to: UInt8.self), count: W * H * 4))
    if got == pattern() { print("cliente: OK, la textura compartida se lee igual (\(t.width)x\(t.height))"); exit(0) }
    print("cliente: los colores no coinciden"); exit(7)
}
