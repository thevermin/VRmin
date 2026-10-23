// Prueba (CI): abre por su nombre una textura compartida creada por un juego bajo Wine + DXMT (como la copia directa)
guard CommandLine.arguments.count >= 2, let dev = MTLCreateSystemDefaultDevice() else { print("sin GPU"); exit(2) }
guard let t = GpuShare.open(CommandLine.arguments[1], device: dev) else { print("no se pudo abrir"); exit(1) }
print("OK: \(t.width)x\(t.height) formato \(t.pixelFormat.rawValue)")
