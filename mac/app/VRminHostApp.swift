// VRmin Host · App de Mac (versión 0.3)
// La parte de la Mac de VRmin: comparte la pantalla, las ventanas y los juegos VR de Steam con el Quest.
// Todo lo necesario viaja dentro de la app (scripts, servidor, renderizador, runtime OpenXR).
// Lo que se descarga o se crea (motor Wine, Windows con Steam, registros) va a la carpeta de datos:
//   ~/Library/Application Support/QuestVRBridge   (o la carpeta del prototipo si ya existe)
//
// Compilar: bash crear_app.sh

import SwiftUI
import AppKit
import ServiceManagement

// MARK: - Juegos
struct SteamGame: Identifiable, Hashable {
    let id: String          // appid de Steam
    let name: String
    let installdir: String
    let inLibrary: Bool     // está en el Steam de QuestVRBridge
    let inGameHub: Bool     // está descargado en GameHub
}

// MARK: - Runtime de VR de cada juego (lo decide recursos/vrjuegos.py; acá solo se muestra y se elige)
struct VRDeteccion: Decodable, Equatable {
    let apis: [String]; let motor: String; let graficos: [String]; let exe: String
    let recomendado: String; let sugerencias: [String]
}
struct VRPerfil: Decodable, Equatable {
    let pedido: String; let runtime: String; let avisos: [String]; let args: [String]
    let lanzar: String; let exe: String; let diagnostico: Bool; let estado: String; let nota: String
}
struct VRJuego: Decodable, Equatable {
    let id: String; let nombre: String; let deteccion: VRDeteccion; let perfil: VRPerfil; let runtimeNombre: String
    let vr: Bool?   // false: juego plano (según la tienda de Steam)
}
struct VRLista: Decodable { let juegos: [VRJuego]; let steamvr: Bool; let driverSteamvr: Bool; let opencomposite: Bool }

/// Las opciones de runtime, con su explicación corta
let RUNTIME_OPCIONES: [(id: String, nombre: String)] = [
    ("auto", "Automático"),
    ("openxr", "OpenXR (directo a VRmin)"),
    ("openvr", "OpenVR / SteamVR vía OpenComposite"),
    ("steamvr", "SteamVR real · experimental"),
    ("oculus", "Oculus (ReVive) · próximamente"),
    ("plano", "Sin VR (pantalla plana)"),
]
let ESTADOS_JUEGO = ["sin probar", "funciona", "con problemas", "no funciona"]
/// Cosas de Steam que no son juegos
let NOT_GAMES: Set<String> = ["228980", "250820", "1070560", "1391110", "1628350"]

func parseACF(_ url: URL) -> [String: String] {
    guard let s = try? String(contentsOf: url, encoding: .utf8),
          let re = try? NSRegularExpression(pattern: "^\\s*\"(\\w+)\"\\s+\"([^\"]*)\"", options: [.anchorsMatchLines])
    else { return [:] }
    var d: [String: String] = [:]
    for m in re.matches(in: s, range: NSRange(s.startIndex..., in: s)) {
        guard let k = Range(m.range(at: 1), in: s), let v = Range(m.range(at: 2), in: s) else { continue }
        let key = String(s[k])
        if d[key] == nil { d[key] = String(s[v]) }   // la primera aparición es la del juego
    }
    return d
}

func scanLibrary(_ steamapps: URL) -> [String: (name: String, dir: String)] {
    var out: [String: (name: String, dir: String)] = [:]
    let fm = FileManager.default
    guard let files = try? fm.contentsOfDirectory(atPath: steamapps.path) else { return out }
    for f in files where f.hasPrefix("appmanifest_") && f.hasSuffix(".acf") {
        let d = parseACF(steamapps.appendingPathComponent(f))
        guard let id = d["appid"], let dir = d["installdir"], !NOT_GAMES.contains(id) else { continue }
        if fm.fileExists(atPath: steamapps.appendingPathComponent("common/\(dir)").path) {
            out[id] = (d["name"] ?? dir, dir)
        }
    }
    return out
}

// MARK: - Arreglos de compatibilidad (en la copia local del jugador, con respaldo)
/// Algunos juegos de SteamVR traen controles con salidas de vibración "none" (un error del juego que
/// SteamVR ignora pero OpenComposite no). Se quitan esas entradas; el original queda como .qvb-original.
func fixSteamVRBindings(gameDir: URL) -> [String] {
    let fm = FileManager.default
    var fixed: [String] = []
    guard let en = fm.enumerator(at: gameDir, includingPropertiesForKeys: [.fileSizeKey]) else { return fixed }
    for case let url as URL in en {
        guard url.pathExtension.lowercased() == "json" else { continue }
        let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        guard size > 0, size < 2_000_000, let data = try? Data(contentsOf: url),
              let text = String(data: data, encoding: .utf8), text.contains("\"haptics\""), text.lowercased().contains("\"none\""),
              let root = try? JSONSerialization.jsonObject(with: data) else { continue }
        var changed = false
        func clean(_ v: Any) -> Any {
            if var d = v as? [String: Any] {
                for (k, val) in d {
                    if ["haptics", "poses", "skeleton"].contains(k.lowercased()), let arr = val as? [Any] {
                        let kept = arr.filter { item in
                            guard let e = item as? [String: Any], let out = e["output"] as? String else { return true }
                            return out.lowercased() != "none"
                        }
                        if kept.count != arr.count { changed = true }
                        d[k] = kept.map(clean)
                    } else {
                        d[k] = clean(val)
                    }
                }
                return d
            }
            if let a = v as? [Any] { return a.map(clean) }
            return v
        }
        let out = clean(root)
        guard changed, let newData = try? JSONSerialization.data(withJSONObject: out, options: [.prettyPrinted]) else { continue }
        let backup = url.appendingPathExtension("qvb-original")
        if !fm.fileExists(atPath: backup.path) { try? fm.copyItem(at: url, to: backup) }
        if (try? newData.write(to: url)) != nil {
            fixed.append(url.path.replacingOccurrences(of: gameDir.path + "/", with: ""))
        }
    }
    return fixed
}

// MARK: - Modelo
final class AppModel: ObservableObject {
    let res: URL        // archivos de la app (dentro del .app)
    let data: URL       // datos del usuario

    @Published var log: [String] = []
    @Published var serverRunning = false
    @Published var questURL: String?
    @Published var questConnected = false
    @Published var pairCode: String?          // código para emparejar un Quest nuevo (se muestra grande)
    @Published var pairDevice = ""
    @Published var statsLine = ""
    @Published var audioOK = false
    @Published var busyTask: String?
    @Published var steamRunning = false
    @Published var openGames = Set<String>()   // carpetas (en minúsculas) de los juegos abiertos ahora
    /// Modo Centinela: herramientas de Android (adb) para que el visor apoyado no apague la pantalla
    @Published var adbInstalled = false
    @Published var vigiaActive = false   // el Modo Centinela está andando (lo dice el renderizador)
    @Published var adbState = ""   // "device" (listo), "unauthorized" (aceptar en el visor), "" (sin Quest por cable)
    var adbPath: URL { data.appendingPathComponent("herramientas/platform-tools/adb") }
    private var adbChecking = false
    private func checkAdb() {
        adbInstalled = FileManager.default.isExecutableFile(atPath: adbPath.path)
        guard adbInstalled, !adbChecking else { if !adbInstalled { adbState = "" }; return }
        adbChecking = true
        let adb = adbPath
        let needVer = questAppVersion == nil
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let p = Process(); let pipe = Pipe()
            p.executableURL = adb; p.arguments = ["devices"]
            p.standardOutput = pipe; p.standardError = FileHandle.nullDevice
            var st = ""
            if (try? p.run()) != nil {
                let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
                p.waitUntilExit()
                for line in out.split(separator: "\n").dropFirst() {
                    let f = line.split(whereSeparator: { $0 == "\t" || $0 == " " })
                    if f.count >= 2 { st = String(f[1]); if st == "device" { break } }
                }
            }
            // Versión de VRmin instalada en el visor (para avisar si hay una nueva)
            var ver: String?
            if st == "device" && needVer {
                let q = Process(); let qp = Pipe()
                q.executableURL = adb; q.arguments = ["shell", "dumpsys", "package", "com.vrmin.app"]
                q.standardOutput = qp; q.standardError = FileHandle.nullDevice
                if (try? q.run()) != nil {
                    let o = String(decoding: qp.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
                    q.waitUntilExit()
                    if let r = o.range(of: "versionName=[^\\s]+", options: .regularExpression) { ver = String(o[r].dropFirst(12)) }
                    else if q.terminationStatus == 0 { ver = "" }   // no está instalada
                }
            }
            DispatchQueue.main.async {
                guard let self else { return }
                self.adbChecking = false
                if self.adbState != st { self.adbState = st }
                if st != "device" { if self.questAppVersion != nil { self.questAppVersion = nil } }
                else if let ver { self.questAppVersion = ver }
            }
        }
    }

    // MARK: Actualizaciones (las versiones publicadas en GitHub: estables y, si se eligió, beta)
    struct Nueva: Equatable { let version: String; let url: URL }
    @Published var newHost: Nueva?              // otra VRmin Host para instalar (más nueva, o la estable si esta es beta)
    @Published var hostBack = false             // newHost es volver a la estable (esta es una beta)
    @Published var latestApk: Nueva?            // la app del Quest del canal elegido
    @Published var betaApks: Set<String> = []   // versiones de la app del Quest que salieron solo como beta
    @Published var questAppVersion: String?     // la del visor (por cable): nil = no se sabe, "" = no está instalada
    /// Recibir también las versiones beta (pre-release en GitHub); apagado = solo las estables
    @Published var beta: Bool = UserDefaults.standard.bool(forKey: "beta") {
        didSet { UserDefaults.standard.set(beta, forKey: "beta"); checkUpdates() } }
    /// "0.40" es más nueva que "0.39" (se compara por partes, como números)
    static func newer(_ a: String, than b: String) -> Bool {
        let x = a.split(separator: ".").map { Int($0) ?? 0 }, y = b.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(x.count, y.count) {
            let p = i < x.count ? x[i] : 0, q = i < y.count ? y[i] : 0
            if p != q { return p > q }
        }
        return false
    }
    @Published var updateMsg: String?           // resultado de «Buscar actualizaciones»
    func checkUpdates(manual: Bool = false) {
        if manual { updateMsg = L("Buscando…") }
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/thevermin/VRmin/releases?per_page=30")!)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        let beta = self.beta
        URLSession.shared.dataTask(with: req) { [weak self] d, _, _ in
            guard let d, let list = try? JSONSerialization.jsonObject(with: d) as? [[String: Any]] else {
                if manual { DispatchQueue.main.async { self?.updateMsg = L("No se pudo buscar. ¿Hay internet?") } }
                return
            }
            struct Rel { let v: String; let pre: Bool; var host: URL?; var apk: Nueva? }
            var rels: [Rel] = []
            for j in list where (j["draft"] as? Bool) != true {
                guard let tag = j["tag_name"] as? String else { continue }
                var r = Rel(v: tag.trimmingCharacters(in: CharacterSet(charactersIn: "vV")), pre: (j["prerelease"] as? Bool) == true)
                for a in (j["assets"] as? [[String: Any]]) ?? [] {
                    guard let n = a["name"] as? String, let s = a["browser_download_url"] as? String, let u = URL(string: s) else { continue }
                    if n.hasSuffix(".zip") && n.contains("Host") { r.host = u }   // «VRmin Host 0.40.zip»
                    if n.hasSuffix(".apk"), let m = n.range(of: "[0-9]+(\\.[0-9]+)+", options: .regularExpression) {   // «VRmin-0.70.apk»
                        r.apk = Nueva(version: String(n[m]), url: u)
                    }
                }
                rels.append(r)
            }
            rels.sort { AppModel.newer($0.v, than: $1.v) }   // la más nueva primero
            let mine = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
            let canal = rels.filter { beta || !$0.pre }
            var host: Nueva?, back = false
            if let r = canal.first(where: { $0.host != nil }), AppModel.newer(r.v, than: mine) { host = Nueva(version: r.v, url: r.host!) }
            else if !beta, rels.contains(where: { $0.pre && $0.v == mine }),   // esta es una beta y se eligió volver a las estables
                    let r = canal.first(where: { $0.host != nil }) { host = Nueva(version: r.v, url: r.host!); back = true }
            let apk = canal.first(where: { $0.apk != nil })?.apk
            let stableApks = Set(rels.filter { !$0.pre }.compactMap { $0.apk?.version })
            let betaApks = Set(rels.filter { $0.pre }.compactMap { $0.apk?.version }).subtracting(stableApks)
            DispatchQueue.main.async {
                self?.newHost = host; self?.hostBack = back
                self?.latestApk = apk; self?.betaApks = betaApks
                if manual { self?.updateMsg = host == nil ? L("Estás al día") : back ? L("Podés volver a la estable: botón arriba") : L("Hay una versión nueva: botón arriba") }
            }
        }.resume()
    }
    /// Baja la VRmin Host nueva (o la estable), revisa su firma y se reemplaza (se cierra y se abre la otra)
    func updateHost() {
        guard let n = newHost else { return }
        run(L("Actualizando VRmin Host"), "actualizar_host.sh",
            [n.url.absoluteString, Bundle.main.bundlePath, String(ProcessInfo.processInfo.processIdentifier)]) {
            NSApp.terminate(nil)
        }
    }
    /// La app del visor: hay otra para instalar (más nueva, la estable si tiene una beta, o no está instalada)
    var questAppAction: Bool {
        guard adbState == "device", let a = latestApk, let v = questAppVersion else { return false }
        return v.isEmpty || AppModel.newer(a.version, than: v) || questAppBack
    }
    var questAppBack: Bool {   // volver a la estable (el visor tiene una beta y ya no se reciben)
        guard !beta, let a = latestApk, let v = questAppVersion, !v.isEmpty else { return false }
        return betaApks.contains(v) && a.version != v
    }
    /// Instala (o actualiza) VRmin en el visor por el cable, con adb
    func installQuestApp() {
        guard let a = latestApk else { return }
        run(questAppVersion == "" ? L("Instalando VRmin en el Quest") : L("Actualizando VRmin en el Quest"),
            "actualizar_app_quest.sh", [adbPath.path, a.url.absoluteString]) { [weak self] in self?.questAppVersion = nil }
    }
    @Published var steamStuck = false          // Steam avisó que se trabó (su bucle principal dejó de responder)
    private var steamLogSeen: UInt64 = 0
    private var steamSince = Date.distantFuture   // desde cuándo está abierto (al arrancar se traba un rato y no cuenta)
    @Published var engineReady = false
    @Published var openComposite = false
    @Published var steamInstalled = false
    @Published var games: [SteamGame] = []
    /// Detección y perfil de VR de cada juego (por número de Steam)
    @Published var vr: [String: VRJuego] = [:]
    @Published var steamVRInstalado = false

    // Calidad (se guarda entre usos; se aplica al encender o reiniciar el servidor)
    @Published var fps: Int = UserDefaults.standard.object(forKey: "fps") as? Int ?? 90 {
        didSet { UserDefaults.standard.set(fps, forKey: "fps"); settingsChanged() } }
    @Published var eye: Int = UserDefaults.standard.object(forKey: "eye") as? Int ?? 1024 {
        didSet { UserDefaults.standard.set(eye, forKey: "eye"); settingsChanged() } }
    @Published var mbps: Int = UserDefaults.standard.object(forKey: "mbps") as? Int ?? 70 {
        didSet { UserDefaults.standard.set(mbps, forKey: "mbps"); settingsChanged() } }
    /// Foveated encoding (lo elige el Quest): "nivel,centro,cx izq,cx der,cy"; "0" = apagado
    @Published var fov: String = UserDefaults.standard.string(forKey: "fov") ?? "0" {
        didSet { UserDefaults.standard.set(fov, forKey: "fov"); settingsChanged() } }
    /// Códec de los juegos: HEVC (por defecto) o H.264 (opción temporal, mientras probamos HEVC)
    @Published var codec: String = UserDefaults.standard.string(forKey: "codec") ?? "hevc" {
        didSet { UserDefaults.standard.set(codec, forKey: "codec"); settingsChanged() } }
    /// Calidad propia del juego que se está por abrir (o abierto); nil = la general
    private var gameQ: (eye: Int, fps: Int, mbps: Int, fov: String)?
    private var runEye: Int { gameQ?.eye ?? eye }
    private var runFps: Int { gameQ?.fps ?? fps }
    private var runMbps: Int { gameQ?.mbps ?? mbps }
    private var runFov: String { gameQ?.fov ?? fov }
    @Published var audioMac: Bool = UserDefaults.standard.bool(forKey: "audioMac") {
        didSet { UserDefaults.standard.set(audioMac, forKey: "audioMac"); settingsChanged() } }
    /// Sonido de los juegos: solo en el visor, solo en la Mac, o en los dos
    @Published var audioOnlyMac: Bool = UserDefaults.standard.bool(forKey: "audioOnlyMac") {
        didSet { UserDefaults.standard.set(audioOnlyMac, forKey: "audioOnlyMac"); settingsChanged() } }
    var audioMode: Int {
        get { audioOnlyMac ? 1 : audioMac ? 2 : 0 }
        set { audioOnlyMac = newValue == 1; audioMac = newValue == 2 }
    }
    /// Idioma de la app (y de los mensajes de los scripts y del renderizador): "en" (por defecto) o "es"
    @Published var lang: String = idioma() {
        didSet { UserDefaults.standard.set(lang, forKey: "lang"); loadVR() } }
    /// Que la Mac no se duerma mientras el servidor está encendido
    @Published var keepAwake: Bool = UserDefaults.standard.object(forKey: "keepAwake") as? Bool ?? true {
        didSet { UserDefaults.standard.set(keepAwake, forKey: "keepAwake"); updateAwake() } }
    /// Abrir VRmin Host al iniciar sesión en la Mac (así el Quest siempre la encuentra)
    @Published var openAtLogin: Bool = SMAppService.mainApp.status == .enabled {
        didSet {
            guard openAtLogin != (SMAppService.mainApp.status == .enabled) else { return }
            do { if openAtLogin { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() } }
            catch { append(L("No se pudo cambiar el inicio automático: %@", error.localizedDescription)) }
        }
    }
    private var awake: NSObjectProtocol?
    private func updateAwake() {
        if keepAwake && serverRunning {
            if awake == nil { awake = ProcessInfo.processInfo.beginActivity(options: [.idleSystemSleepDisabled, .userInitiated], reason: "VRmin Host: el Quest está usando la Mac") }
        } else if let a = awake { ProcessInfo.processInfo.endActivity(a); awake = nil }
    }
    @Published var pendingRestart = false
    private var restartAfterStop = false
    private var runningWith = ""
    private var settingsKey: String { "\(runFps)/\(runEye)/\(runMbps)/\(audioMac)/\(audioOnlyMac)/\(cableOnly)/\(runFov)/\(codec)" }
    /// Conexión con el Quest solo por el cable USB-C (rechaza la conexión por Wi-Fi de la sala VR)
    @Published var cableOnly: Bool = UserDefaults.standard.bool(forKey: "cableOnly") {
        didSet {
            UserDefaults.standard.set(cableOnly, forKey: "cableOnly")
            // se aplica ya: el servidor se reinicia y el Quest se vuelve a conectar solo
            if serverRunning && cableOnly != oldValue { restartServer() } else { settingsChanged() }
        } }
    private func settingsChanged() { gameQ = nil; pendingRestart = serverRunning && settingsKey != runningWith }
    func resetQuality() { fps = 90; eye = 1024; mbps = 70; audioMac = false; audioOnlyMac = false }

    private var server: Process?
    private var checkedGames: Set<String> = []
    private var timer: Timer?
    private var partial: [String: String] = [:]

    var steamapps: URL { data.appendingPathComponent("prefijo/drive_c/Program Files (x86)/Steam/steamapps") }
    var runtimeLog: URL { data.appendingPathComponent("prefijo/drive_c/questvrbridge/questvrbridge_log.txt") }

    init() {
        let fm = FileManager.default
        res = (Bundle.main.resourceURL ?? Bundle.main.bundleURL).appendingPathComponent("qvb")
        // Carpeta de datos: la elegida antes, o el prototipo existente, o Application Support
        let home = fm.homeDirectoryForCurrentUser
        let proto = home.appendingPathComponent("Documents/QuestVRBridge/prototipo")
        if let saved = UserDefaults.standard.string(forKey: "dataDir") {
            data = URL(fileURLWithPath: saved)
        } else if fm.fileExists(atPath: proto.appendingPathComponent("motor").path) {
            data = proto
        } else {
            data = home.appendingPathComponent("Library/Application Support/QuestVRBridge")
        }
        try? fm.createDirectory(at: data, withIntermediateDirectories: true)
        UserDefaults.standard.set(data.path, forKey: "dataDir")
        if !fm.fileExists(atPath: res.appendingPathComponent("servidor.sh").path) {
            append(L("ATENCIÓN: faltan archivos dentro de la app (%@). Volvé a crearla con crear_app.sh", res.path))
        }
        append(L("Datos en: %@", data.path))
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.refresh() }
        checkUpdates()
        _ = Timer.scheduledTimer(withTimeInterval: 6 * 3600, repeats: true) { [weak self] _ in self?.checkUpdates() }
        startRemoteStart()
    }

    /// La versión del motor que espera esta app (MARCA_MOTOR en recursos/entorno.sh)
    lazy var marcaMotor: String = {
        let s = (try? String(contentsOf: res.appendingPathComponent("entorno.sh"), encoding: .utf8)) ?? ""
        guard let r = s.range(of: "MARCA_MOTOR=\"[^\"]*\"", options: .regularExpression) else { return "" }
        return String(s[r]).replacingOccurrences(of: "MARCA_MOTOR=", with: "").replacingOccurrences(of: "\"", with: "")
    }()

    // MARK: Estado
    func refresh() {
        let fm = FileManager.default
        let marca = (try? String(contentsOf: data.appendingPathComponent("prefijo/.wine_usado"), encoding: .utf8)) ?? ""
        engineReady = fm.isExecutableFile(atPath: data.appendingPathComponent("motor/wswine.bundle/bin/wine").path)
            && !marcaMotor.isEmpty && marca.trimmingCharacters(in: .whitespacesAndNewlines) == marcaMotor   // (otra versión del motor: hay que actualizarla)
        openComposite = fm.fileExists(atPath: data.appendingPathComponent("prefijo/drive_c/questvrbridge/opencomposite/bin/vrclient_x64.dll").path)
        steamInstalled = fm.fileExists(atPath: data.appendingPathComponent("prefijo/drive_c/Program Files (x86)/Steam/steam.exe").path)

        let mine = scanLibrary(steamapps)
        let gh = scanLibrary(fm.homeDirectoryForCurrentUser.appendingPathComponent("GameHub/steamapps"))
        var list: [SteamGame] = []
        for id in Set(mine.keys).union(gh.keys) {
            let info = mine[id] ?? gh[id]!
            list.append(SteamGame(id: id, name: info.name, installdir: info.dir,
                                  inLibrary: mine[id] != nil, inGameHub: gh[id] != nil))
        }
        list.sort { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        if list != games { games = list; loadVR() }
        // Arreglos de compatibilidad apenas un juego aparece en la biblioteca
        // (así también sirven si el juego se abre directo desde Steam)
        for g in list where g.inLibrary && !checkedGames.contains(g.id) {
            checkedGames.insert(g.id)
            let dir = steamapps.appendingPathComponent("common/\(g.installdir)")
            DispatchQueue.global(qos: .utility).async { [weak self] in
                let fixed = fixSteamVRBindings(gameDir: dir)
                DispatchQueue.main.async {
                    for f in fixed { self?.append(L("Arreglo de compatibilidad en %@: %@ (original guardado como .qvb-original)", g.name, f)) }
                }
            }
        }

        checkAdb()
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
            p.arguments = ["-if", "steam\\.exe"]
            p.standardOutput = FileHandle.nullDevice
            p.standardError = FileHandle.nullDevice
            var running = false
            if (try? p.run()) != nil { p.waitUntilExit(); running = p.terminationStatus == 0 }
            // Juegos abiertos ahora (por la carpeta steamapps\common\<carpeta>\ en la línea de comandos)
            var open = Set<String>()
            if running {
                let ps = Process(); let pipe = Pipe()
                ps.executableURL = URL(fileURLWithPath: "/bin/ps"); ps.arguments = ["-axo", "command="]
                ps.standardOutput = pipe; ps.standardError = FileHandle.nullDevice
                if (try? ps.run()) != nil {
                    let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
                    ps.waitUntilExit()
                    for line in out.split(separator: "\n") {
                        guard let r = line.range(of: "steamapps\\common\\", options: .caseInsensitive) else { continue }
                        let rest = line[r.upperBound...]
                        if let e = rest.firstIndex(of: "\\") { open.insert(String(rest[..<e]).lowercased()) }
                    }
                }
            }
            DispatchQueue.main.async { if self?.openGames != open { self?.openGames = open } }
            // ¿Steam se trabó? Lo avisa él mismo en su registro («BMainLoop appears to have stalled»)
            var stuck = false
            let logURL = self?.data.appendingPathComponent("logs/steam.log")
            if running, let logURL, let h = try? FileHandle(forReadingFrom: logURL) {
                let size = (try? h.seekToEnd()) ?? 0
                let seen = DispatchQueue.main.sync { self?.steamLogSeen ?? 0 }
                let from = size < seen ? 0 : seen
                if size > from {
                    try? h.seek(toOffset: max(from, size > 65536 ? size - 65536 : 0))
                    if let d = try? h.readToEnd(), String(decoding: d, as: UTF8.self).contains("appears to have stalled") { stuck = true }
                }
                try? h.close()
                DispatchQueue.main.async { self?.steamLogSeen = size }
            }
            DispatchQueue.main.async {
                guard let self else { return }
                if running && !self.steamRunning { self.steamSince = Date() }
                self.steamRunning = running
                if !running { self.steamStuck = false; self.steamSince = .distantFuture }
                else if stuck && !self.steamStuck && Date().timeIntervalSince(self.steamSince) > 120 {
                    self.steamStuck = true
                    self.append(L("⚠︎ Steam se trabó. Tocá «Reiniciar Steam» (cierra Steam y el juego, y lo vuelve a abrir)."))
                }
            }
        }
    }

    // MARK: Registro
    func append(_ line: String) {
        log.append(idioma() == "es" ? line : LogEn.translate(line))
        if log.count > 800 { log.removeFirst(log.count - 800) }
    }

    private func ingest(_ s: String, tag: String) {
        let buf = ((partial[tag] ?? "") + s).replacingOccurrences(of: "\r", with: "\n")
        var lines = buf.components(separatedBy: "\n")
        partial[tag] = lines.removeLast()
        for l in lines {
            let t = l.trimmingCharacters(in: .whitespaces)
            if t.isEmpty { continue }
            if t.allSatisfy({ "#=-.% 0123456789".contains($0) }) { continue }   // barras de progreso
            handleLine(t, tag: tag)
        }
    }

    private func handleLine(_ l: String, tag: String) {
        if tag == "srv", let r = l.range(of: "[emparejar] ") {
            let rest = l[r.upperBound...].split(separator: " ", maxSplits: 2).map(String.init)
            if rest.first == "CODIGO", rest.count >= 2 {
                pairCode = rest[1]; pairDevice = rest.count > 2 ? rest[2] : "Quest"
                append(L("🔑 Un Quest quiere conectarse: escribí el código %@ en el Quest", rest[1]))
            } else if rest.first == "LISTO" {
                pairCode = nil
                append(L("✓ Quest emparejado: ya se puede conectar solo"))
            } else if rest.first == "VENCIDO" {
                pairCode = nil
            }
            return
        }
        if tag == "srv" {
            if let r = l.range(of: "https://") {
                questURL = String(l[r.lowerBound...]).trimmingCharacters(in: .whitespaces)
            }
            if l.contains("[+] Quest conectado") || l.contains("[quest] App del Quest conectada") { questConnected = true }
            if l.contains("[vigía] Modo Centinela: el juego se ve en la Mac") { vigiaActive = true }
            if l.contains("[reporte] pedido desde el visor") { report() }
            // El juego abierto mandó imagen: si estaba «sin probar», queda «funciona» (no hace falta marcarlo a mano)
            if l.contains("[juego] Primer cuadro recibido") {
                for g in games where g.inLibrary && openGames.contains(g.installdir.lowercased()) {
                    if (vr[g.id]?.perfil.estado ?? "sin probar") == "sin probar" { setVR(g.id, "estado", "funciona") }
                    let id = g.id   // el runtime con el que se abrió anduvo (el modo automático lo sigue usando)
                    DispatchQueue.global(qos: .utility).async { [weak self] in _ = self?.python(["exito", id]) }
                }
            }
            if l.contains("[vigía] Modo Centinela terminado") { vigiaActive = false }
            // Grabar desde el visor (ajustes rápidos del Quest)
            if l.contains("[grabar] empezar") && !recording { toggleRecording() }
            if l.contains("[grabar] parar") && recording { toggleRecording() }
            if l.contains("[-] Quest desconectado") || l.contains("[quest] App del Quest desconectada") { questConnected = false }
            if l.hasPrefix("Quest ") && l.contains("poses/s") {   // estadísticas de cada segundo: no van al registro
                statsLine = l
                audioOK = l.contains("sonido OK")
                return
            }
        }
        append(l)
    }

    private func makeProcess(_ argv: [String], tag: String, extraEnv: [String: String] = [:]) -> Process {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: argv[0])
        p.arguments = Array(argv.dropFirst())
        p.currentDirectoryURL = data
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:/opt/homebrew/bin"
        env["VRMIN_LANG"] = idioma()
        if env["USER"] == nil { env["USER"] = NSUserName() }
        env["QVB_DATA"] = data.path
        for (k, v) in extraEnv { env[k] = v }
        p.environment = env
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        pipe.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            if d.isEmpty { h.readabilityHandler = nil; return }
            let s = String(decoding: d, as: UTF8.self)
            DispatchQueue.main.async { self?.ingest(s, tag: tag) }
        }
        return p
    }

    /// Corre un comando y espera hasta `timeout` segundos (si se pasa, lo corta: al cerrar la app, un adb trabado
    /// con el visor dormido dejaba a VRmin Host a medio cerrar)
    private func shell(_ cmd: String, timeout: TimeInterval = 10) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/bash")
        p.arguments = ["-c", cmd]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return }
        let end = Date().addingTimeInterval(timeout)
        while p.isRunning && Date() < end { Thread.sleep(forTimeInterval: 0.05) }
        if p.isRunning { p.terminate() }
    }

    private func killLeftovers() {
        shell("pkill -f 'qvb/server.py'; pkill -f 'qvb/QuestRenderer'; pkill -f 'QuestVRBridge/server.py'; pkill -f 'renderer/QuestRenderer'; sleep 0.3")
    }

    // MARK: Encendido a distancia (desde la app del Quest)
    /// Con el servidor apagado, la app se anuncia en la red como "lista" y espera el pedido del Quest (UDP 9102)
    /// para encender el servidor sola. Así alcanza con tener la app abierta.
    private func startRemoteStart() {
        let name = (Host.current().localizedName ?? "Mac").replacingOccurrences(of: "\"", with: "")
        Thread.detachNewThread { [weak self] in
            let s = socket(AF_INET, SOCK_DGRAM, 0)
            var one: Int32 = 1
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, socklen_t(MemoryLayout<Int32>.size))
            let msg = Array("{\"qvb\":1,\"port\":9100,\"idle\":true,\"name\": \"\(name)\"}".utf8)
            while true {
                guard let self else { return }
                let running = DispatchQueue.main.sync { self.serverRunning }
                if !running {
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
                    var addr = sockaddr_in()
                    addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
                    addr.sin_family = sa_family_t(AF_INET)
                    addr.sin_port = UInt16(9101).bigEndian
                    for t in targets {
                        addr.sin_addr.s_addr = t
                        _ = withUnsafePointer(to: &addr) {
                            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(s, msg, msg.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
                        }
                    }
                }
                sleep(1)
            }
        }
        // Cable USB-C: el Quest pregunta «¿quién está?» a todos (IPv6 ff02::1, puerto 9103) y la app contesta
        // con el nombre de la Mac y si el servidor está encendido (así se encuentra y se enciende por el cable)
        Thread.detachNewThread { [weak self] in
            let s = socket(AF_INET6, SOCK_DGRAM, 0)
            var one: Int32 = 1
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
            var addr = sockaddr_in6()
            addr.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
            addr.sin6_family = sa_family_t(AF_INET6)
            addr.sin6_port = UInt16(9103).bigEndian
            addr.sin6_addr = in6addr_any
            let r = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(s, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) } }
            if r != 0 {
                DispatchQueue.main.async { self?.append(L("⚠︎ No se pudo abrir el puerto 9103 para el cable USB-C (errno %ld): el Quest se conecta por las direcciones del cable", Int(errno))) }
                return
            }
            var buf = [UInt8](repeating: 0, count: 256)
            while true {
                var from = sockaddr_in6()
                var fl = socklen_t(MemoryLayout<sockaddr_in6>.size)
                let n = withUnsafeMutablePointer(to: &from) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(s, &buf, buf.count, 0, $0, &fl) } }
                guard n > 0, String(decoding: buf.prefix(n), as: UTF8.self).contains("probe") else { continue }
                guard let self else { return }
                let running = DispatchQueue.main.sync { self.serverRunning }
                let reply = Array("{\"qvb\":1,\"port\":9100,\"idle\":\(running ? "false" : "true"),\"usb\":true,\"name\": \"\(name)\"}".utf8)
                _ = withUnsafePointer(to: &from) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(s, reply, reply.count, 0, $0, fl) } }
            }
        }
        Thread.detachNewThread { [weak self] in
            // (IPv6 y IPv4: los pedidos llegan por Wi-Fi o por el cable USB-C)
            let s = socket(AF_INET6, SOCK_DGRAM, 0)
            var one: Int32 = 1, zero: Int32 = 0
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
            setsockopt(s, Int32(IPPROTO_IPV6), IPV6_V6ONLY, &zero, socklen_t(MemoryLayout<Int32>.size))
            var addr = sockaddr_in6()
            addr.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
            addr.sin6_family = sa_family_t(AF_INET6)
            addr.sin6_port = UInt16(9102).bigEndian
            addr.sin6_addr = in6addr_any
            let r = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(s, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) } }
            if r != 0 { DispatchQueue.main.async { self?.append(L("Aviso: no pude escuchar los pedidos del Quest (puerto 9102)")) }; return }
            var buf = [UInt8](repeating: 0, count: 256)
            while true {
                let n = recv(s, &buf, buf.count, 0)
                guard n > 0 else { continue }
                let txt = String(decoding: buf.prefix(n), as: UTF8.self)
                // Ajustes de juegos VR elegidos en el Quest: guardarlos y reiniciar el servidor para aplicarlos
                if txt.contains("\"settings\""), let o = try? JSONSerialization.jsonObject(with: Data(txt.utf8)) as? [String: Any] {
                    DispatchQueue.main.async {
                        guard let self else { return }
                        if let v = o["eye"] as? Int, (512...2560).contains(v) { self.eye = v }
                        if let v = o["fps"] as? Int, [72, 80, 90, 120].contains(v) { self.fps = v }
                        if let v = o["mbps"] as? Int, (10...300).contains(v) { self.mbps = v }
                        if let v = o["codec"] as? String, ["hevc", "h264"].contains(v) { self.codec = v }
                        if let v = o["audioMac"] as? Bool { self.audioMac = v }
                        if let v = o["audio"] as? String { self.audioMode = v == "mac" ? 1 : v == "both" ? 2 : 0 }
                        if let v = o["fov"] as? String, v.count < 80, v.allSatisfy({ "0123456789.,".contains($0) }) { self.fov = v }
                        self.append(L("⚙︎ Ajustes desde el Quest: %ld×%ld por ojo · %ld Hz · %ld Mbps", self.eye, self.eye, self.fps, self.mbps))
                        if self.serverRunning { self.restartServer() }
                    }
                    continue
                }
                // Un juego con calidad propia (o que vuelve a la general): reiniciar con esa y después abrirlo
                if txt.contains("\"playgame\""), let o = try? JSONSerialization.jsonObject(with: Data(txt.utf8)) as? [String: Any],
                   let e = o["eye"] as? Int, let f = o["fps"] as? Int, let mb = o["mbps"] as? Int, let fv = o["fov"] as? String {
                    let id = (o["id"] as? String).flatMap { $0.allSatisfy(\.isNumber) && !$0.isEmpty ? $0 : nil }   // sin id: el juego ya está abierto
                    DispatchQueue.main.async { self?.playWith(id: id, name: o["name"] as? String ?? "", eye: e, fps: f, mbps: mb, fov: fv) }
                    continue
                }
                guard txt.contains("\"start\"") else { continue }
                DispatchQueue.main.async {
                    guard let self, !self.serverRunning else { return }
                    self.append(L("▶︎ El Quest pidió conectarse: encendiendo el servidor"))
                    self.startServer()
                }
            }
        }
    }

    // MARK: Servidor
    func startServer() {
        guard server == nil else { return }
        killLeftovers()   // por si quedó algo abierto de una vez anterior (ocupando los puertos)
        append(L("▶︎ Encendiendo el servidor · %ld×%ld por ojo · %ld Hz · %ld Mbps", runEye, runEye, runFps, runMbps) + (audioMac ? L(" · sonido también en la Mac") : ""))
        let p = makeProcess(["/bin/bash", res.appendingPathComponent("servidor.sh").path], tag: "srv",
                            extraEnv: ["VR_FPS": "\(runFps)", "VR_EYE": "\(runEye)", "VR_MBPS": "\(runMbps)",
                                       "VR_AUDIO_MAC": audioMac ? "1" : "0", "VR_AUDIO": audioOnlyMac ? "0" : "1",
                                       "VR_CABLE_ONLY": cableOnly ? "1" : "0", "VR_FOV": runFov, "VR_CODEC": codec,
                                       "VR_GEN_EYE": "\(eye)", "VR_GEN_FPS": "\(fps)", "VR_GEN_MBPS": "\(mbps)", "VR_GEN_FOV": fov])
        p.terminationHandler = { [weak self] _ in
            DispatchQueue.main.async {
                guard let self else { return }
                self.server = nil; self.serverRunning = false; self.questConnected = false; self.vigiaActive = false
                self.questURL = nil; self.statsLine = ""; self.audioOK = false; self.pendingRestart = false
                self.append(L("■ Servidor detenido"))
                self.updateAwake()
                if self.restartAfterStop { self.restartAfterStop = false; self.startServer() }
            }
        }
        do { try p.run(); server = p; serverRunning = true; runningWith = settingsKey; pendingRestart = false; updateAwake() }
        catch { append(L("No se pudo encender el servidor: %@", error.localizedDescription)) }
    }

    // MARK: Grabar (por ejemplo, para un trailer): la pantalla de la Mac y, por el cable, lo que se ve en el visor
    @Published var recording = false
    @Published var recSince = Date()
    /// Ajustes avanzados de cada juego (los mismos que en el Quest): perfiles_juegos.json en la carpeta de datos.
    /// "" = todos los juegos; si no, la carpeta de Steam del juego. El renderizador lo vuelve a leer cuando cambia.
    struct AdvP: Codable, Equatable { var scale: Double?; var auto: Bool?; var fovk: Double?; var index: Bool?
                                      var eye: Int?; var fps: Int?; var mbps: Int?; var fov: String? }   // (calidad propia, se elige en el Quest)
    @Published var adv: [String: AdvP] = [:]
    private var advFile: URL { data.appendingPathComponent("perfiles_juegos.json") }
    func loadAdv() { adv = (try? JSONDecoder().decode([String: AdvP].self, from: Data(contentsOf: advFile))) ?? [:] }
    func setAdv(_ key: String, _ change: (inout AdvP) -> Void) {
        loadAdv()
        var p = adv[key] ?? AdvP(); change(&p); adv[key] = p
        if let d = try? JSONEncoder().encode(adv) { try? d.write(to: advFile) }
    }
    func resetAdv(_ key: String) {
        loadAdv(); adv[key] = nil
        if let d = try? JSONEncoder().encode(adv) { try? d.write(to: advFile) }
    }

    /// Grabar también el visor (por el cable); si no, solo el escritorio de la Mac
    @Published var recQuest = UserDefaults.standard.object(forKey: "recQuest") as? Bool ?? true {
        didSet { UserDefaults.standard.set(recQuest, forKey: "recQuest") }
    }
    var recDir: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Movies/VRmin") }
    private var recStop: URL { data.appendingPathComponent("grabar.parar") }
    func toggleRecording() {
        if recording {   // grabar.sh termina al ver este archivo (y guarda todo)
            FileManager.default.createFile(atPath: recStop.path, contents: nil)
            append(L("Terminando la grabación…"))
            return
        }
        try? FileManager.default.removeItem(at: recStop)
        let adb = (recQuest && adbInstalled && adbState == "device") ? adbPath.path : ""
        let p = makeProcess(["/bin/bash", res.appendingPathComponent("grabar.sh").path, adb, recDir.path, recStop.path], tag: "task")
        p.terminationHandler = { [weak self] _ in
            DispatchQueue.main.async { self?.recording = false; self?.append(L("Grabación guardada en Películas › VRmin")) }
        }
        do { try p.run(); recording = true; recSince = Date() }
        catch { append(L("No se pudo ejecutar %@: %@", "grabar.sh", error.localizedDescription)) }
    }
    func openRecordings() {
        try? FileManager.default.createDirectory(at: recDir, withIntermediateDirectories: true)
        NSWorkspace.shared.open(recDir)
    }

    /// «Solo Modo Centinela»: enciende el servidor y abre VRmin en el visor (por el cable) directo en el Modo Centinela
    func startVigiaOnly() {
        if !serverRunning { startServer() }
        let name = (Host.current().localizedName ?? "Mac").replacingOccurrences(of: "\"", with: "")
        run(L("Modo Centinela"), "vigia_quest.sh", [name], exclusive: false)
    }

    func restartServer() {
        guard server != nil else { startServer(); return }
        restartAfterStop = true
        stopServer()
    }

    func stopServer(wait: Bool = false) {
        guard let p = server else { return }
        p.terminate()   // servidor.sh cierra servidor y renderizador al recibir esta señal
        // (por si quedó el Modo Centinela: el sensor de proximidad del visor vuelve a la normalidad)
        if adbState == "device" {
            let a = adbPath.path.replacingOccurrences(of: "'", with: "'\\''")
            let cmd = "'\(a)' shell am broadcast -a com.oculus.vrpowermanager.automation_disable"
            if wait { shell(cmd, timeout: 2) } else { DispatchQueue.global(qos: .utility).async { [weak self] in self?.shell(cmd) } }
        }
        if wait {
            for _ in 0..<30 where p.isRunning { Thread.sleep(forTimeInterval: 0.1) }
            killLeftovers()
        }
    }

    // MARK: Tareas (un script a la vez)
    func run(_ title: String, _ script: String, _ args: [String] = [], exclusive: Bool = true, then: (() -> Void)? = nil) {
        if exclusive {
            guard busyTask == nil else { return }
            busyTask = title
        }
        append("▶︎ \(title)")
        let p = makeProcess(["/bin/bash", res.appendingPathComponent(script).path] + args, tag: "task")
        p.terminationHandler = { [weak self] pr in
            DispatchQueue.main.async {
                guard let self else { return }
                self.append(pr.terminationStatus == 0 ? L("✓ %@: listo", title) : L("✗ %@: terminó con un error (código %ld)", title, Int(pr.terminationStatus)))
                if exclusive { self.busyTask = nil }
                self.refresh()
                if pr.terminationStatus == 0 { then?() }
            }
        }
        do { try p.run() }
        catch { append(L("No se pudo ejecutar %@: %@", script, error.localizedDescription)); if exclusive { busyTask = nil } }
    }

    // MARK: Runtime de VR por juego
    private func python(_ args: [String]) -> Data? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        p.arguments = [res.appendingPathComponent("vrjuegos.py").path] + args
        var env = ProcessInfo.processInfo.environment
        env["QVB_DATA"] = data.path
        env["VRMIN_LANG"] = idioma()
        p.environment = env
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return nil }
        let d = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return p.terminationStatus == 0 ? d : nil
    }

    /// Lee (en segundo plano) qué API de VR usa cada juego y qué runtime le toca
    func loadVR() {
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self, let d = self.python(["listar"]), let l = try? JSONDecoder().decode(VRLista.self, from: d) else { return }
            var map: [String: VRJuego] = [:]
            for j in l.juegos { map[j.id] = j }
            DispatchQueue.main.async { if map != self.vr { self.vr = map }; self.steamVRInstalado = l.steamvr }
        }
    }

    /// Guarda una elección para un juego (runtime, lanzar, estado, diagnostico…) y vuelve a leer
    func setVR(_ id: String, _ key: String, _ value: String) {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            _ = self.python(["poner", id, key, value])
            DispatchQueue.main.async { self.loadVR() }
        }
    }

    func redetect(_ id: String) {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            _ = self?.python(["detectar", id])
            DispatchQueue.main.async { self?.loadVR() }
        }
    }

    /// «Algo anda mal»: un .zip en el Escritorio con los registros (también se pide desde el Quest)
    func report() {
        let f = data.appendingPathComponent("logs/vrmin_host.log")
        try? (log.joined(separator: "\n") + "\n").write(to: f, atomically: true, encoding: .utf8)
        run(L("Juntando un reporte"), "reporte.sh", [f.path], exclusive: false)
    }

    func gameLogs(_ g: SteamGame) { run(L("Juntando registros de %@", g.name), "registros_juego.sh", [g.id], exclusive: false) }

    /// Abre un juego con su calidad (la propia o la general): si no es la que corre, reinicia el servidor antes
    private func playWith(id: String?, name: String, eye e: Int, fps f: Int, mbps mb: Int, fov fv: String) {
        let general = e == eye && f == fps && mb == mbps && fv == fov
        gameQ = general ? nil : (e, f, mb, fv)
        append(L("⚙︎ Calidad para %@: %ld×%ld por ojo · %ld Hz · %ld Mbps (reiniciando el servidor)", name, runEye, runEye, runFps, runMbps))
        restartServer()
        guard let id else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 6) { [weak self] in
            self?.run(L("Abriendo %@", name), "jugar.sh", [id], exclusive: false)
        }
    }

    func play(_ g: SteamGame) {
        if !serverRunning { append(L("Primero encendé el servidor (o conectate desde el Quest).")); return }
        if !steamRunning { append(L("Primero abrí Steam y esperá a ver tu biblioteca.")); return }
        // ¿Este juego quiere otra calidad que la que corre ahora?
        loadAdv()
        let q = adv[g.installdir]
        let e = q?.eye ?? eye, f = q?.fps ?? fps, mb = q?.mbps ?? mbps, fv = q?.fov ?? fov
        if e != runEye || f != runFps || mb != runMbps || fv != runFov {
            playWith(id: g.id, name: g.name, eye: e, fps: f, mbps: mb, fov: fv); return
        }
        let dir = steamapps.appendingPathComponent("common/\(g.installdir)")
        let args = [g.id]   // el runtime y los argumentos los decide jugar.sh (perfil del juego)
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let fixed = fixSteamVRBindings(gameDir: dir)
            DispatchQueue.main.async {
                guard let self else { return }
                for f in fixed { self.append(L("Arreglo de compatibilidad aplicado: %@ (original guardado como .qvb-original)", f)) }
                self.run(L("Abriendo %@", g.name), "jugar.sh", args, exclusive: false)
            }
        }
    }

    /// Cierra ese juego a la fuerza (si no responde): primero le pide que se cierre y, si no, lo termina
    func forceClose(_ g: SteamGame) {
        run(L("Cerrando %@", g.name), "cerrar_juego.sh", [g.installdir], exclusive: false)
    }

    func importGame(_ g: SteamGame) {
        run(L("Importando %@ desde GameHub", g.name), "importar_gamehub.sh", [g.id])
    }

    func closeAll(then: (() -> Void)? = nil) {
        append(L("▶︎ Cerrando Steam y los juegos"))
        DispatchQueue.global().async { [weak self] in
            guard let self else { return }
            self.shell("QVB_DATA='\(self.data.path)' bash '\(self.res.appendingPathComponent("cerrar.sh").path)'")
            DispatchQueue.main.async { self.steamStuck = false; self.append(L("■ Steam y juegos cerrados")); self.refresh(); then?() }
        }
    }

    /// Steam trabado: cerrarlo a la fuerza y volver a abrirlo
    func restartSteam() {
        closeAll { [weak self] in self?.run(L("Abriendo Steam"), "abrir_steam.sh") }
    }

    func openLogs() {
        let logs = data.appendingPathComponent("logs")
        try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
        NSWorkspace.shared.open(logs)
    }
    func openData() { NSWorkspace.shared.open(data) }
    func openRuntimeLog() {
        if FileManager.default.fileExists(atPath: runtimeLog.path) { NSWorkspace.shared.open(runtimeLog) }
        else { append(L("Todavía no hay registro del runtime (se crea al abrir un juego VR).")) }
    }
}


// MARK: - Estilo (el mismo de la app del Quest: grafito + ámbar)
enum Theme {
    static let bg = Color(red: 20 / 255, green: 20 / 255, blue: 22 / 255)
    static let card = Color.white.opacity(0.055)
    static let line = Color.white.opacity(0.08)
    static let text = Color(red: 243 / 255, green: 243 / 255, blue: 244 / 255)
    static let text2 = Color(red: 243 / 255, green: 243 / 255, blue: 244 / 255).opacity(0.62)
    static let amber = Color(red: 230 / 255, green: 168 / 255, blue: 74 / 255)
    static let amberText = Color(red: 242 / 255, green: 194 / 255, blue: 122 / 255)
    static let amberDark = Color(red: 26 / 255, green: 19 / 255, blue: 8 / 255)
}

/// El visor del logo (grilla de 26×14: una píldora con el arco de la nariz abajo al medio)
struct VisorShape: Shape {
    func path(in r: CGRect) -> Path {
        let k = min(r.width / 26, r.height / 14)
        func p(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: r.minX + x * k, y: r.minY + y * k) }
        var path = Path()
        path.move(to: p(7, 0))
        path.addLine(to: p(19, 0))
        path.addArc(center: p(19, 7), radius: 7 * k, startAngle: .degrees(-90), endAngle: .degrees(90), clockwise: false)
        path.addLine(to: p(16.6, 14))
        path.addQuadCurve(to: p(9.4, 14), control: p(13, 7.2))
        path.addLine(to: p(7, 14))
        path.addArc(center: p(7, 7), radius: 7 * k, startAngle: .degrees(90), endAngle: .degrees(270), clockwise: false)
        path.closeSubpath()
        return path
    }
}

/// Logo: «VR» en negrita, «min» fino, el visor ámbar como punto de la «i», y «Host»
struct Logo: View {
    var size: CGFloat = 30
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: size * 0.28) {
            HStack(alignment: .firstTextBaseline, spacing: 0) {
                Text("VR").font(.system(size: size, weight: .bold))
                Text("m").font(.system(size: size, weight: .ultraLight))
                Text("\u{0131}").font(.system(size: size, weight: .ultraLight))
                    .overlay(alignment: .top) {
                        VisorShape().fill(Theme.amber)
                            .frame(width: size * 0.53, height: size * 0.285)
                            .offset(y: size * 0.02)
                    }
                Text("n").font(.system(size: size, weight: .ultraLight))
            }
            .kerning(-size * 0.02)
            Text("Host").font(.system(size: size * 0.62, weight: .regular)).foregroundStyle(Theme.text2)
        }
        .foregroundStyle(Theme.text)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("VRmin Host")
    }
}

/// Tarjeta de vidrio oscuro con título
struct Card<Content: View>: View {
    let title: String
    var icon: String? = nil
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                if let icon { Image(systemName: icon).foregroundStyle(Theme.text2) }
                Text(title).font(.system(size: 15, weight: .semibold)).foregroundStyle(Theme.text2)
            }
            content
        }
        .padding(18)
        .frame(maxWidth: .infinity, alignment: .topLeading)
        .background(RoundedRectangle(cornerRadius: 9, style: .continuous).fill(Theme.card))
        .overlay(RoundedRectangle(cornerRadius: 9, style: .continuous).strokeBorder(Theme.line))
    }
}

/// Botón principal (ámbar) y secundario (gris translúcido), tipo píldora
struct Pill: ButtonStyle {
    var primary = false
    @Environment(\.isEnabled) private var enabled
    func makeBody(configuration c: Configuration) -> some View {
        c.label
            .font(.system(size: 13, weight: .semibold))
            .padding(.horizontal, 14).padding(.vertical, 7)
            .foregroundStyle(primary ? Theme.amberDark : Theme.text)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(primary ? Theme.amber.opacity(c.isPressed ? 0.8 : 1) : Color.white.opacity(c.isPressed ? 0.16 : 0.10)))
            .opacity(enabled ? 1 : 0.4)
    }
}

/// Estado: lleno ámbar = listo, aro ámbar = a medias, gris = apagado
struct Dot: View {
    let on: Bool
    var warn = false
    var body: some View {
        ZStack {
            if on { Circle().fill(Theme.amber) }
            else if warn { Circle().strokeBorder(Theme.amber, lineWidth: 1.6) }
            else { Circle().fill(Color.white.opacity(0.22)) }
        }
        .frame(width: 9, height: 9)
    }
}

// MARK: - Vista
struct ContentView: View {
    @EnvironmentObject var m: AppModel
    @State private var showSettings = false
    @State private var showLog = false
    @State private var showVigiaHelp = false
    @State private var tut: TutPresent? = nil   // tutorial o Ayuda en pantalla

    var body: some View {
        // (toda la ventana se desplaza: así los juegos se ven enteros, sin una lista chica con su propia barra)
        ScrollView {
        VStack(alignment: .leading, spacing: 16) {
            header
            HStack(alignment: .top, spacing: 16) {
                connectionCard
                prepCard
            }
            gamesCard
            recordCard
            logSection
        }
        .padding(.horizontal, 22).padding(.bottom, 20).padding(.top, 8)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(Theme.bg.ignoresSafeArea())
        .foregroundStyle(Theme.text)
        .tint(Theme.amber)
        .sheet(isPresented: $showSettings) { SettingsView().environmentObject(m) }
        .sheet(item: $tut) { p in TutorialSheet(p: p) }
        .onAppear {   // la bienvenida, la primera vez
            if Tutoriales.enabled("bienvenida"), let t = Tutoriales.find("bienvenida") {
                UserDefaults.standard.set(true, forKey: "tutNo_bienvenida")   // (una sola vez; sigue en la Ayuda)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { tut = TutPresent(tut: t, auto: true) }
            }
        }
        .overlay {
            if let code = m.pairCode {
                ZStack {
                    Color.black.opacity(0.55).ignoresSafeArea()
                    VStack(spacing: 14) {
                        Image(systemName: "lock.shield").font(.system(size: 34)).foregroundStyle(Theme.amber)
                        Text(L("Emparejar un Quest")).font(.system(size: 20, weight: .bold))
                        Text(L("Escribí este código en el Quest para permitirle usar esta Mac:"))
                            .font(.callout).foregroundStyle(Theme.text2).multilineTextAlignment(.center)
                        Text(code.map { String($0) }.joined(separator: " "))
                            .font(.system(size: 44, weight: .bold, design: .monospaced)).foregroundStyle(Theme.amberText)
                        Text(L("Vence en 5 minutos. Si no lo pediste vos, cerrá este aviso y no lo compartas."))
                            .font(.caption).foregroundStyle(Theme.text2).multilineTextAlignment(.center)
                        Button(L("Cerrar")) { m.pairCode = nil }.buttonStyle(Pill())
                    }
                    .padding(28).frame(width: 380)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Theme.bg))
                }
            }
        }
    }

    // Logo, estado general y ajustes
    private var header: some View {
        HStack(alignment: .center, spacing: 12) {
            Logo(size: 30)
            Spacer()
            if let t = m.busyTask {
                ProgressView().controlSize(.small)
                Text(t).font(.callout).foregroundStyle(Theme.text2).lineLimit(1)
            }
            HStack(spacing: 8) {
                // «Lista» solo si de verdad está todo: motor, Steam instalado y abierto
                let falta = !m.engineReady ? L("Falta instalar el motor") : !m.steamInstalled ? L("Falta instalar Steam")
                    : !m.steamRunning ? L("Falta abrir Steam") : m.steamStuck ? L("Steam se trabó") : nil
                Dot(on: m.questConnected && falta == nil, warn: falta != nil || (m.serverRunning && !m.questConnected))
                Text(falta ?? (m.questConnected ? L("Quest conectado") : m.serverRunning ? L("Esperando al Quest") : L("Lista")))
                    .font(.system(size: 13, weight: .semibold))
            }
            .padding(.horizontal, 12).padding(.vertical, 7)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(Color.white.opacity(0.07)))
            if let n = m.newHost {
                Button { m.updateHost() } label: {
                    Label(m.hostBack ? L("Volver a la %@ (estable)", n.version) : L("Actualizar a la %@", n.version),
                          systemImage: m.hostBack ? "arrow.uturn.backward.circle.fill" : "arrow.down.circle.fill")
                }
                    .buttonStyle(Pill(primary: true)).disabled(m.busyTask != nil)
                    .help(m.hostBack ? L("Tenés una versión beta y elegiste solo versiones estables: se baja la estable, se cierra esta y se abre la otra.")
                                     : L("Hay una versión nueva de VRmin Host: se baja, se cierra esta y se abre la nueva."))
            }
            // Apoyar el proyecto (VRmin es gratis)
            Button { NSWorkspace.shared.open(URL(string: "https://ko-fi.com/thevermin")!) } label: {
                Label(L("Apoyar en Ko-fi"), systemImage: "cup.and.saucer.fill")
            }
                .buttonStyle(Pill(primary: true))
                .help(L("VRmin es gratis. Si te sirve, podés invitarme un café en Ko-fi"))
            Button { tut = TutPresent(tut: nil) } label: { Image(systemName: "questionmark.circle") }
                .buttonStyle(Pill())
                .help(L("Ayuda"))
                .accessibilityLabel(L("Ayuda"))
            Button { showSettings = true } label: { Image(systemName: "slider.horizontal.3") }
                .buttonStyle(Pill())
                .help(L("Ajustes"))
                .accessibilityLabel(L("Ajustes"))
        }
    }

    private var connectionCard: some View {
        Card(title: L("Conexión"), icon: "visionpro") {
            VStack(alignment: .leading, spacing: 6) {
                Text(m.questConnected ? L("El Quest está usando esta Mac")
                     : m.serverRunning ? L("Encendida: abrí VRmin en el Quest")
                     : L("Abrí VRmin en el Quest y tocá Conectar"))
                    .font(.system(size: 17, weight: .semibold))
                Text(m.serverRunning ? L("Se apaga sola cuando cerrás esta app.")
                     : L("No hace falta encender nada acá: el Quest la enciende al conectarse."))
                    .font(.callout).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
                Toggle(L("Solo por cable USB-C"), isOn: $m.cableOnly).toggleStyle(.switch)
                    .font(.callout)
                    .help(L("Los juegos van siempre por el cable: se rechaza la conexión por Wi-Fi de la sala VR. Se aplica al reiniciar el servidor."))
                if !m.statsLine.isEmpty {
                    Text(m.statsLine).font(.system(size: 11, design: .monospaced)).foregroundStyle(Theme.text2).lineLimit(2)
                }
            }
            HStack {
                Button(m.serverRunning ? L("Apagar") : L("Encender ahora")) {
                    if m.serverRunning { m.stopServer() } else { m.startServer() }
                }
                .buttonStyle(Pill())
                if m.audioOK {
                    Label(L("Sonido OK"), systemImage: "speaker.wave.2.fill").font(.caption).foregroundStyle(Theme.amberText)
                }
            }
        }
    }

    private func row<A: View>(_ title: String, _ ok: Bool, _ status: String, warn: Bool = false,
                              @ViewBuilder action: () -> A) -> some View {
        HStack(spacing: 10) {
            Dot(on: ok, warn: warn)
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.system(size: 14, weight: .semibold)).lineLimit(1)
                Text(status).font(.caption).foregroundStyle(Theme.text2).lineLimit(3)
            }
            Spacer()
            action()
        }
    }

    private var prepCard: some View {
        Card(title: L("Para los juegos VR"), icon: "gamecontroller") {
            row(L("Motor de juegos de Windows"), m.engineReady && m.openComposite,
                !m.engineReady ? L("Sin instalar (≈270 MB)") : (m.openComposite ? L("Instalado") : L("Falta una parte (OpenComposite)")),
                warn: m.engineReady && !m.openComposite) {
                if !(m.engineReady && m.openComposite) {
                    Button(m.engineReady ? L("Completar") : L("Instalar")) { m.run(L("Instalando el motor"), "instalar_motor.sh") }
                        .buttonStyle(Pill(primary: true)).disabled(m.busyTask != nil)
                }
            }
            row(L("Steam para Windows"), m.steamRunning,
                !m.steamInstalled ? L("Sin instalar") : (m.steamRunning ? (m.steamStuck ? L("Se trabó · reinicialo") : L("Abierto")) : L("Cerrado · tarda ~1 minuto en abrir")),
                warn: m.steamInstalled && (!m.steamRunning || m.steamStuck)) {
                if !m.steamInstalled {
                    Button(L("Instalar")) { m.run(L("Instalando Steam"), "instalar_steam.sh") }
                        .buttonStyle(Pill(primary: true)).disabled(m.busyTask != nil || !m.engineReady)
                } else if !m.steamRunning {
                    Button(L("Abrir")) { m.run(L("Abriendo Steam"), "abrir_steam.sh") }
                        .buttonStyle(Pill(primary: true)).disabled(m.busyTask != nil || !m.engineReady)
                } else {
                    if m.steamStuck {
                        Button(L("Reiniciar Steam")) { m.restartSteam() }.buttonStyle(Pill(primary: true)).disabled(m.busyTask != nil)
                    }
                    else { Button(L("Mostrar")) { m.run(L("Mostrando Steam"), "abrir_steam.sh", exclusive: false) }.buttonStyle(Pill()) }
                    Button(L("Cerrar")) { m.closeAll() }.buttonStyle(Pill())
                }
            }
            row(L("Modo Centinela"), m.adbInstalled && m.adbState == "device",
                L("Visor apoyado") + " · " + (!m.adbInstalled ? L("Sin instalar")
                : m.adbState == "device" ? L("Listo")
                : m.adbState == "unauthorized" ? L("Tocá «Permitir» en el visor")
                : L("Conectá el Quest por cable USB-C")),
                warn: m.adbState != "device") {
                Button { showVigiaHelp.toggle() } label: { Image(systemName: "questionmark") }
                    .buttonStyle(Pill())
                    .help(L("Cómo activar el modo desarrollador"))
                    .popover(isPresented: $showVigiaHelp, arrowEdge: .bottom) { vigiaHelp }
                if !m.adbInstalled {
                    Button(L("Instalar")) { m.run(L("Instalando las herramientas del Modo Centinela"), "instalar_adb.sh") }
                        .buttonStyle(Pill(primary: true)).disabled(m.busyTask != nil)
                } else if m.vigiaActive {
                    Label(L("Funcionando"), systemImage: "checkmark.circle.fill")
                        .font(.system(size: 13, weight: .semibold)).foregroundStyle(Theme.amberText)
                        .help(L("El Modo Centinela está andando: el juego se ve en la ventana «VRmin · Modo Centinela». Para salir, cerrá esa ventana o apretá Esc."))
                } else if m.adbState == "device" {
                    Button(L("Empezar")) {
                        // primero su tutorial (con «Empezar»); si lo apagaste, empieza directo
                        if Tutoriales.enabled("centinela"), let t = Tutoriales.find("centinela") {
                            tut = TutPresent(tut: t, auto: true, start: (L("Empezar el Modo Centinela"), { m.startVigiaOnly() }))
                        } else { m.startVigiaOnly() }
                    }
                        .buttonStyle(Pill(primary: true))
                        .help(L("Abre VRmin en el visor directo en el Modo Centinela, sin tocarlo. Después abrí un juego con «Jugar»."))
                }
            }
            row(L("App del Quest"), m.adbState == "device" && !(m.questAppVersion ?? "").isEmpty && !m.questAppAction,
                questAppStatus, warn: m.questAppAction) {
                if m.questAppAction {
                    Button(m.questAppVersion == "" ? L("Instalar") : m.questAppBack ? L("Volver a la estable") : L("Actualizar")) { m.installQuestApp() }
                        .buttonStyle(Pill(primary: true)).disabled(m.busyTask != nil)
                        .help(m.questAppBack ? L("Se desinstala la beta del visor y se instala la estable, por el cable USB-C. Los ajustes del visor vuelven a los de fábrica.")
                              : L("Se instala por el cable USB-C, sin SideQuest. Tus ajustes del visor se conservan."))
                }
            }
        }
    }

    private var questAppStatus: String {
        guard m.adbInstalled else { return L("Para instalarla desde acá: herramientas del Modo Centinela") }
        if m.adbState == "unauthorized" { return L("Tocá «Permitir» en el visor") }
        guard m.adbState == "device", let v = m.questAppVersion else { return L("Conectá el Quest por cable USB-C para ver su versión") }
        if v.isEmpty { return L("No está instalada en el visor") }
        if m.questAppBack, let a = m.latestApk { return L("Versión %@ (beta) · estable: %@", v, a.version) }
        if let a = m.latestApk, AppModel.newer(a.version, than: v) { return L("Versión %@ · hay una nueva: %@", v, a.version) }
        return L("Versión %@ · al día", v)
    }

    /// Instrucciones: modo desarrollador del Quest (una sola vez) para el Modo Centinela
    private var vigiaHelp: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(L("Modo Centinela: pantalla del visor siempre encendida")).font(.system(size: 15, weight: .bold))
            Text(L("Con el visor apoyado en el escritorio, el sensor que está entre las lentes cree que te lo sacaste y apaga la pantalla. Con el modo desarrollador y estas herramientas, VRmin lo evita mientras el juego esté abierto (y lo vuelve a la normalidad al cerrarlo). Se hace una sola vez:"))
                .font(.callout).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
            ForEach(Array([
                L("En el celular, abrí la app Meta Horizon › Dispositivos › tu Quest › Ajustes del visor › Modo desarrollador, y activalo. (Si te lo pide, creá una organización de desarrollador gratuita en developers.meta.com: solo hace falta un nombre.)"),
                L("Reiniciá el Quest."),
                L("Tocá «Instalar» acá (baja adb, de Google, ≈15 MB)."),
                L("Conectá el Quest a la Mac con el cable USB-C, ponete el visor y tocá «Permitir» en el aviso de depuración USB (marcá «Permitir siempre desde esta computadora»)."),
                L("Listo: el punto se pone en verde. Ya no hace falta tapar el sensor.")
            ].enumerated()), id: \.offset) { i, t in
                HStack(alignment: .top, spacing: 8) {
                    Text("\(i + 1).").font(.callout.weight(.semibold)).foregroundStyle(Theme.amberText)
                    Text(t).font(.callout).fixedSize(horizontal: false, vertical: true)
                }
            }
            Text(L("Sin modo desarrollador también funciona: tapá el sensor con cinta o un papel doblado."))
                .font(.caption).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
        }
        .padding(18).frame(width: 420)
    }

    /// Grabar un video: la Mac y (por el cable) el visor, en Películas › VRmin
    /// Grabar un video: una sola fila (es una función simple)
    private var recordCard: some View {
        HStack(spacing: 10) {
            Image(systemName: "record.circle").foregroundStyle(Theme.text2)
            Text(L("Grabación")).font(.system(size: 13, weight: .semibold)).foregroundStyle(Theme.text2)
            Button { m.toggleRecording() } label: {
                Label(m.recording ? L("Detener") : L("Grabar"), systemImage: m.recording ? "stop.fill" : "record.circle")
            }
            .buttonStyle(Pill(primary: !m.recording)).controlSize(.small)
            if m.recording {
                TimelineView(.periodic(from: .now, by: 1)) { ctx in
                    let s = Int(ctx.date.timeIntervalSince(m.recSince))
                    HStack(spacing: 5) {
                        Circle().fill(Color.red).frame(width: 7, height: 7)
                        Text(String(format: "%d:%02d", s / 60, s % 60)).font(.system(size: 12, weight: .semibold).monospacedDigit())
                    }
                }
            }
            Toggle(L("También el visor"), isOn: $m.recQuest)
                .toggleStyle(.switch).controlSize(.mini).font(.caption)
                .disabled(m.recording)
                .help(L("Con el visor por cable (adb) se graba también lo que ves en el visor. Apagado: solo el escritorio de la Mac."))
            if m.recQuest {
                Text(m.adbInstalled && m.adbState == "device" ? L("Puede bajar los fps del visor") : L("Solo con el visor por cable"))
                    .font(.caption).foregroundStyle(Theme.text2).lineLimit(1)
                    .help(m.adbInstalled && m.adbState == "device" ? L("Grabar el visor usa su codificador de video: puede bajar los cuadros por segundo en el visor.")
                          : L("El visor se graba solo si está conectado por cable (como en el Modo Centinela). Sin sonido."))
            }
            Spacer()
            Button { m.openRecordings() } label: { Image(systemName: "folder") }
                .buttonStyle(.borderless).help(L("Abrir la carpeta de las grabaciones (Películas › VRmin)"))
        }
        .padding(.horizontal, 14).padding(.vertical, 8)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.white.opacity(0.05)))
    }

    private var gamesCard: some View {
        Card(title: L("Juegos"), icon: "square.grid.2x2") {
            if m.games.isEmpty {
                Text(L("Todavía no hay juegos. Instalalos desde Steam y aparecen acá (y en el Quest)."))
                    .font(.callout).foregroundStyle(Theme.text2)
            } else {
                let flat = sortedGames.filter { m.vr[$0.id]?.vr == false }
                let vrGames = sortedGames.filter { m.vr[$0.id]?.vr != false }
                VStack(alignment: .leading, spacing: 10) {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 170), spacing: 12, alignment: .top)], alignment: .leading, spacing: 12) {
                        ForEach(vrGames) { g in gameCard(g) }
                    }
                    if !flat.isEmpty {
                        Text(L("Juegos planos · se abren como una ventana de la Mac (en el visor, en Studio)"))
                            .font(.callout.weight(.semibold)).foregroundStyle(Theme.text2).padding(.top, 6)
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 170), spacing: 12, alignment: .top)], alignment: .leading, spacing: 12) {
                            ForEach(flat) { g in gameCard(g) }
                        }
                    }
                }
                .padding(.vertical, 2)
                Text(L("Lo más cómodo es elegirlos desde el Quest, en Juegos. ¿No aparece un juego? Abrilo directamente desde Steam."))
                    .font(.caption).foregroundStyle(Theme.text2)
            }
        }
    }

    private func isOpen(_ g: SteamGame) -> Bool { g.inLibrary && m.openGames.contains(g.installdir.lowercased()) }
    /// En orden alfabético; el juego abierto, primero
    private var sortedGames: [SteamGame] {
        m.games.sorted { a, b in
            let oa = isOpen(a), ob = isOpen(b)
            if oa != ob { return oa }
            return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
        }
    }

    /// Un juego: su imagen de Steam (con «Forzar cierre» encima si está abierto), nombre, estado y acciones
    private func gameCard(_ g: SteamGame) -> some View {
        let open = isOpen(g)
        return VStack(alignment: .leading, spacing: 6) {
            ZStack(alignment: .topTrailing) {
                AsyncImage(url: URL(string: "https://cdn.cloudflare.steamstatic.com/steam/apps/\(g.id)/header.jpg")) { img in
                    img.resizable().aspectRatio(contentMode: .fill)
                } placeholder: {
                    ZStack { Color.white.opacity(0.06); Image(systemName: "gamecontroller").font(.system(size: 22)).foregroundStyle(Theme.text2) }
                }
                .frame(maxWidth: .infinity).frame(height: 70).clipped()
                .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 6, style: .continuous).stroke(open ? Theme.amber : .clear, lineWidth: 2))
                if open {
                    Button(L("Forzar cierre")) { m.forceClose(g) }
                        .buttonStyle(Pill(primary: true))
                        .help(L("Cierra el juego aunque no responda (Steam sigue abierto)"))
                        .padding(6)
                }
            }
            HStack(spacing: 6) {
                Dot(on: g.inLibrary && m.vr[g.id]?.perfil.estado == "funciona",
                    warn: g.inLibrary && ["sin probar", "con problemas"].contains(m.vr[g.id]?.perfil.estado ?? "sin probar"))
                Text(g.name).font(.system(size: 12, weight: .semibold)).lineLimit(1)
                if open { Text(L("Abierto")).font(.caption.weight(.semibold)).foregroundStyle(Theme.amberText) }
                Spacer(minLength: 0)
            }
            Text(gameStatus(g)).font(.caption2).foregroundStyle(Theme.text2).lineLimit(1).help(gameStatus(g))
            HStack(spacing: 6) {
                if g.inLibrary {
                    if !open {
                        Button(L("Jugar")) { m.play(g) }
                            .buttonStyle(Pill(primary: true))
                            .disabled(!m.serverRunning || !m.steamRunning)
                    }
                    if let v = m.vr[g.id] { vrMenu(g, v) }
                } else {
                    Button(L("Traer")) { m.importGame(g) }
                        .buttonStyle(Pill())
                        .disabled(m.busyTask != nil || !m.steamInstalled || m.steamRunning)
                        .help(m.steamRunning ? L("Cerrá Steam primero") : L("Copia instantánea desde GameHub"))
                }
            }
        }
    }

    /// Lo que se ve debajo del nombre de un juego: con qué runtime abre, qué se detectó, y si ya se probó
    private func gameStatus(_ g: SteamGame) -> String {
        guard g.inLibrary else { return L("En GameHub · se puede traer sin volver a bajarlo") }
        guard let v = m.vr[g.id] else { return L("En tu biblioteca · revisando qué VR usa…") }
        let apis = v.deteccion.apis.isEmpty ? L("API no detectada") : v.deteccion.apis.map { $0 == "openxr" ? "OpenXR" : $0 == "openvr" ? "OpenVR" : L("Oculus") }.joined(separator: " + ")
        var s = "\(L(v.runtimeNombre))\(v.perfil.pedido == "auto" ? " (auto)" : "") · \(apis) · \(L(v.deteccion.motor)) · \(L(v.perfil.estado))"
        if let a = v.perfil.avisos.first { s += L("\nAviso: ") + a }
        return s
    }

    /// Menú de VR de un juego: runtime, cómo abrirlo, diagnóstico, estado y registros
    private func vrMenu(_ g: SteamGame, _ v: VRJuego) -> some View {
        Menu {
            Picker(L("Runtime de VR"), selection: Binding(get: { v.perfil.pedido }, set: { m.setVR(g.id, "runtime", $0) })) {
                ForEach(RUNTIME_OPCIONES, id: \.id) { o in
                    Text(o.id == "auto" ? L("Automático (%@)", L(RUNTIME_OPCIONES.first { $0.id == v.deteccion.recomendado }?.nombre ?? v.deteccion.recomendado)) : L(o.nombre)).tag(o.id)
                }
            }
            Picker(L("Cómo abrirlo"), selection: Binding(get: { v.perfil.lanzar }, set: { m.setVR(g.id, "lanzar", $0) })) {
                Text(L("Desde Steam")).tag("steam")
                Text(L("Directo (para probar)")).tag("directo")
            }
            Toggle(L("Diagnóstico detallado (al abrir directo)"), isOn: Binding(get: { v.perfil.diagnostico },
                                                                          set: { m.setVR(g.id, "diagnostico", $0 ? "1" : "0") }))
            Picker(L("Estado"), selection: Binding(get: { v.perfil.estado }, set: { m.setVR(g.id, "estado", $0) })) {
                ForEach(ESTADOS_JUEGO, id: \.self) { e in Text({ let t = L(e); return t.prefix(1).uppercased() + String(t.dropFirst()) }()).tag(e) }
            }
            Divider()
            Section(L("Detectado")) {
                Text(L("APIs: ") + (v.deteccion.apis.isEmpty ? L("ninguna conocida") : v.deteccion.apis.joined(separator: ", ")))
                Text(L("Motor: %@", L(v.deteccion.motor)) + (v.deteccion.graficos.isEmpty ? "" : L(" · gráficos: ") + v.deteccion.graficos.joined(separator: ", ")))
                if !v.perfil.args.isEmpty { Text(L("Argumentos: ") + v.perfil.args.joined(separator: " ")) }
                ForEach(v.deteccion.sugerencias, id: \.self) { Text(L("Sugerencia: ") + $0) }
            }
            Divider()
            Button(L("Volver a detectar")) { m.redetect(g.id) }
            Button(L("Juntar registros del juego")) { m.gameLogs(g) }
        } label: {
            Image(systemName: "slider.horizontal.3")
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .help(L("VR de este juego: runtime, cómo abrirlo y registros"))
        .accessibilityLabel(L("Opciones de VR de %@", g.name))
    }

    // Registro: escondido; se abre si hace falta revisar algo
    private var logSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button {
                withAnimation(.easeOut(duration: 0.2)) { showLog.toggle() }
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: showLog ? "chevron.down" : "chevron.right").font(.system(size: 11, weight: .semibold))
                    Text(L("Registro")).font(.system(size: 13, weight: .semibold))
                    if let last = m.log.last, !showLog {
                        Text(last).font(.caption).foregroundStyle(Theme.text2).lineLimit(1)
                    }
                }
                .foregroundStyle(Theme.text2)
            }
            .buttonStyle(.plain)
            HStack(spacing: 10) {
                Button { m.report() } label: { Label(L("Algo anda mal"), systemImage: "exclamationmark.bubble") }
                    .buttonStyle(Pill())
                Text(L("Junta los registros (Mac, visor y juegos) en un .zip en el Escritorio, para mandarlo cuando pidas ayuda. No incluye claves ni datos personales."))
                    .font(.caption).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
            }
            if showLog {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 1) {
                            ForEach(Array(m.log.enumerated()), id: \.offset) { i, line in
                                Text(line).font(.system(size: 11, design: .monospaced))
                                    .foregroundStyle(Theme.text2)
                                    .textSelection(.enabled)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                                    .id(i)
                            }
                        }
                        .padding(10)
                    }
                    .frame(height: 220)
                    .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(Color.black.opacity(0.25)))
                    .onChange(of: m.log.count) { _, n in if n > 0 { proxy.scrollTo(n - 1, anchor: .bottom) } }
                    .onAppear { if !m.log.isEmpty { proxy.scrollTo(m.log.count - 1, anchor: .bottom) } }
                }
                HStack {
                    Button(L("Carpeta de registros")) { m.openLogs() }.buttonStyle(Pill())
                    Button(L("Registro del runtime VR")) { m.openRuntimeLog() }.buttonStyle(Pill())
                }
            }
        }
    }
}

// MARK: - Ajustes (lo justo: calidad de los juegos VR, sonido y la Mac)
struct SettingsView: View {
    @EnvironmentObject var m: AppModel
    @Environment(\.dismiss) private var dismiss

    private func group<C: View>(_ title: String, _ hint: String? = nil, @ViewBuilder _ c: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.system(size: 13, weight: .semibold))
            c()
            if let hint { Text(hint).font(.caption).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true) }
        }
    }

    @State private var advKey = ""
    /// Avanzado: escala de render, campo de visión, escala automática y controles de Index, para todos los juegos o uno
    private var advancedCard: some View {
        let vrGames = m.games.filter { $0.inLibrary && m.vr[$0.id]?.vr != false }
        let all = m.adv[""] ?? AppModel.AdvP(), own = advKey.isEmpty ? nil : m.adv[advKey]
        let scale = own?.scale ?? all.scale ?? 1, fovk = own?.fovk ?? all.fovk ?? 1
        let auto = own?.auto ?? all.auto ?? false, index = own?.index ?? all.index ?? false
        let again = L("Hay que volver a abrir el juego para que se aplique.")
        return Card(title: L("Avanzado"), icon: "slider.horizontal.3") {
            group(L("Configurando"), L("Si un juego tiene ajustes propios, siempre se usan esos. Si no, usa los de «Todos los juegos». Para que vuelva a usar los generales: «Usar los de todos los juegos».")) {
                HStack {
                    Picker(L("Configurando"), selection: $advKey) {
                        Text(L("Todos los juegos")).tag("")
                        ForEach(vrGames) { g in Text(g.name + (m.adv[g.installdir] != nil ? " ★" : "")).tag(g.installdir) }
                    }.labelsHidden()
                    if !advKey.isEmpty && own != nil {
                        Button(L("Usar los de todos los juegos")) { m.resetAdv(advKey) }.buttonStyle(Pill())
                    }
                }
            }
            group(L("Escala de render"), L("El juego dibuja menos píxeles y la Mac los agranda con MetalFX: más cuadros por segundo") + "\n" + again) {
                Picker(L("Escala de render"), selection: Binding(get: { Int((scale * 100).rounded()) }, set: { v in m.setAdv(advKey) { $0.scale = Double(v) / 100 } })) {
                    ForEach([50, 60, 70, 80, 90, 100], id: \.self) { Text("\($0) %").tag($0) }
                }.pickerStyle(.segmented).labelsHidden()
            }
            group(L("Campo de visión"), L("Las esquinas casi no se ven por las lentes: si el juego no las dibuja, rinde más") + "\n" + again) {
                Picker(L("Campo de visión"), selection: Binding(get: { Int((fovk * 100).rounded()) }, set: { v in m.setAdv(advKey) { $0.fovk = Double(v) / 100 } })) {
                    ForEach([80, 85, 90, 95, 100], id: \.self) { Text("\($0) %").tag($0) }
                }.pickerStyle(.segmented).labelsHidden()
            }
            Toggle(L("Escala automática"), isOn: Binding(get: { auto }, set: { v in m.setAdv(advKey) { $0.auto = v } })).toggleStyle(.switch)
            Toggle(L("Emular controles de Valve Index"), isOn: Binding(get: { index }, set: { v in m.setAdv(advKey) { $0.index = v } })).toggleStyle(.switch)
            Text(again).font(.caption).foregroundStyle(Theme.text2)
        }
        .onAppear { m.loadAdv() }
    }

    var body: some View {
        ScrollView {
        VStack(alignment: .leading, spacing: 18) {
            HStack {
                Text(L("Ajustes")).font(.system(size: 22, weight: .bold))
                Spacer()
                Button(L("Listo")) { dismiss() }.buttonStyle(Pill(primary: true)).keyboardShortcut(.defaultAction)
            }

            Card(title: L("Juegos VR"), icon: "visionpro") {
                Text(L("También se cambian desde el Quest, en Ajustes.")).font(.caption).foregroundStyle(Theme.text2)
                group(L("Nitidez (píxeles por ojo)")) {
                    Picker(L("Nitidez"), selection: $m.eye) {
                        Text("1024").tag(1024); Text("1280").tag(1280); Text("1536").tag(1536); Text("1832").tag(1832); Text("2048").tag(2048)
                        Text("2304").tag(2304); Text("2560").tag(2560)
                    }.pickerStyle(.segmented).labelsHidden()
                    Text(L("2304 y 2560 son para Macs con chip Pro, Max o Ultra.")).font(.caption).foregroundStyle(Theme.text2)
                }
                group(L("Códec de video"), L("HEVC da mejor imagen con los mismos Mbps. H.264 queda como opción temporal, por si HEVC anda mal.")) {
                    Picker(L("Códec"), selection: $m.codec) {
                        Text("HEVC").tag("hevc"); Text("H.264").tag("h264")
                    }.pickerStyle(.segmented).labelsHidden()
                }
                group(L("Foveated encoding"), L("El borde de cada ojo va más liviano: menos demora y un centro más nítido. Con 2048 conviene dejarlo encendido.")) {
                    Picker(L("Foveated encoding"), selection: Binding(get: { Int(m.fov.split(separator: ",").first ?? "0") ?? 0 },
                                                                     set: { lv in
                        var parts = m.fov.split(separator: ",").map(String.init)
                        if lv == 0 { m.fov = "0" } else if parts.count >= 2 { parts[0] = "\(lv)"; m.fov = parts.joined(separator: ",") } else { m.fov = "\(lv),1" }
                    })) {
                        Text(L("Apagado")).tag(0); Text(L("Suave")).tag(1); Text(L("Medio")).tag(2); Text(L("Fuerte")).tag(3)
                    }.pickerStyle(.segmented).labelsHidden()
                }
                group(L("Fluidez (cuadros por segundo)")) {
                    Picker(L("Fluidez"), selection: $m.fps) {
                        Text("72").tag(72); Text("90").tag(90); Text("120").tag(120)
                    }.pickerStyle(.segmented).labelsHidden()
                }
                group(L("Calidad de imagen (Mbps)")) {
                    Picker(L("Calidad"), selection: $m.mbps) {
                        Text("40").tag(40); Text("70").tag(70); Text("100").tag(100); Text("150").tag(150); Text("200").tag(200)
                    }.pickerStyle(.segmented).labelsHidden()
                }
                group(L("Sonido de los juegos")) {
                    Picker(L("Sonido"), selection: Binding(get: { m.audioMode }, set: { m.audioMode = $0 })) {
                        Text(L("Visor")).tag(0); Text("Mac").tag(1); Text(L("Los dos")).tag(2)
                    }.pickerStyle(.segmented).labelsHidden()
                }
                HStack {
                    Button(L("Recomendado")) { m.resetQuality() }.buttonStyle(Pill())
                    Spacer()
                    if m.pendingRestart {
                        Button(L("Aplicar")) { m.restartServer() }.buttonStyle(Pill(primary: true))
                    }
                }
                Text(m.pendingRestart ? L("Aplicar corta la conexión unos segundos. Si cambiaste la nitidez, volvé a abrir el juego.")
                     : L("Recomendado para una MacBook Air: 1024 · 90 · 70."))
                    .font(.caption).foregroundStyle(m.pendingRestart ? Theme.amberText : Theme.text2)
                    .fixedSize(horizontal: false, vertical: true)
            }

            advancedCard

            Card(title: L("Esta Mac"), icon: "laptopcomputer") {
                HStack {
                    Text(m.lang == "es" ? "Idioma" : "Language")
                    Spacer()
                    Picker(m.lang == "es" ? "Idioma" : "Language", selection: $m.lang) {
                        Text("English").tag("en"); Text("Español").tag("es")
                    }.pickerStyle(.segmented).labelsHidden().frame(width: 200)
                }
                Toggle(L("Abrir VRmin Host al iniciar la Mac"), isOn: $m.openAtLogin).toggleStyle(.switch)
                Toggle(L("No dormir mientras el Quest la usa"), isOn: $m.keepAwake).toggleStyle(.switch)
                Toggle(L("Recibir versiones beta"), isOn: $m.beta).toggleStyle(.switch)
                    .help(L("Las betas traen lo nuevo antes, pero pueden fallar. Si lo apagás, VRmin Host te ofrece volver a la versión estable."))
                HStack {
                    Text(m.data.path).font(.caption).foregroundStyle(Theme.text2).lineLimit(1).truncationMode(.middle)
                    Spacer()
                    Button(L("Mostrar datos")) { m.openData() }.buttonStyle(Pill())
                }
                HStack {
                    Text(L("Quest emparejados con esta Mac")).font(.callout)
                    Spacer()
                    Button(L("Olvidar todos")) {
                        try? FileManager.default.removeItem(at: m.data.appendingPathComponent("emparejados.json"))
                        m.append(L("Se olvidaron los Quest emparejados: la próxima vez van a pedir el código"))
                    }.buttonStyle(Pill())
                }
            }

            HStack {
                Text(L("VRmin Host %@ · en el Quest: VRmin", Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""))
                    .font(.caption).foregroundStyle(Theme.text2)
                Spacer()
                if let msg = m.updateMsg { Text(msg).font(.caption).foregroundStyle(Theme.text2) }
                Button(L("Buscar actualizaciones")) { m.checkUpdates(manual: true) }.buttonStyle(Pill())
            }
            // Crédito del proyecto original (lo exige la licencia también en las versiones modificadas)
            Text("VRmin by thevermin · github.com/thevermin/vrmin · GPL-3.0")
                .font(.caption).foregroundStyle(Theme.text2)
        }
        .padding(22)
        }   // (ScrollView: con «Avanzado», los ajustes no entran en una pantalla chica)
        .frame(width: 520)
        .frame(maxHeight: 760)
        .background(Theme.bg)
        .foregroundStyle(Theme.text)
        .tint(Theme.amber)
        .preferredColorScheme(.dark)
    }
}

// MARK: - App
final class AppDelegate: NSObject, NSApplicationDelegate {
    var model: AppModel?
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) { model?.stopServer(wait: true) }
}

@main
struct VRminHostApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup("VRmin Host") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 820, minHeight: 640)
                .preferredColorScheme(.dark)
                .onAppear { delegate.model = model }
        }
        .windowStyle(.hiddenTitleBar)
    }
}
