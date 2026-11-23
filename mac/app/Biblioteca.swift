// VRmin Host · Biblioteca y Tienda: los juegos de Steam con el estilo de VRmin y el toque de Steam (portadas, página de
// cada juego con su imagen grande, barra de descargas). Steam sigue abierto de fondo para la sesión y las descargas;
// con «Biblioteca de VRmin» (Ajustes) se abre sin su ventana. Las imágenes son las de la tienda de Steam.

import SwiftUI
import AppKit

// MARK: - Descargas (de los appmanifest de Steam)
struct SteamDownload: Identifiable, Equatable {
    let id: String; let name: String; var done: Int64; var total: Int64; let paused: Bool
    var speed: Double = 0   // bytes por segundo (de dos lecturas seguidas)
    var fraction: Double { total > 0 ? min(1, Double(done) / Double(total)) : 0 }
}

/// Cuánto ocupa una carpeta (lo que Steam ya bajó de un juego, en steamapps/downloading/<appid>)
func folderBytes(_ url: URL) -> Int64 {
    guard let en = FileManager.default.enumerator(at: url, includingPropertiesForKeys: [.totalFileAllocatedSizeKey, .fileSizeKey]) else { return 0 }
    var n: Int64 = 0
    for case let f as URL in en {
        let v = try? f.resourceValues(forKeys: [.totalFileAllocatedSizeKey, .fileSizeKey])
        // (lo que ocupa de verdad en el disco, no su tamaño: Steam reserva el tamaño entero de cada archivo al
        // empezar, y esos archivos quedan casi vacíos hasta que se bajan)
        n += Int64(v?.totalFileAllocatedSize ?? v?.fileSize ?? 0)
    }
    return n
}

/// Lo que Steam está bajando o actualizando: los appmanifest con bytes por bajar (StateFlags 512 = en pausa)
func scanDownloads(_ steamapps: URL) -> [SteamDownload] {
    let fm = FileManager.default
    guard let files = try? fm.contentsOfDirectory(atPath: steamapps.path) else { return [] }
    var out: [SteamDownload] = []
    for f in files where f.hasPrefix("appmanifest_") && f.hasSuffix(".acf") {
        let d = parseACF(steamapps.appendingPathComponent(f))
        // (LastOwner 0: Steam lo dejó como parte de otro juego, como Lost Coast con Half-Life 2, y nunca lo baja)
        guard let id = d["appid"], d["LastOwner"] != "0" else { continue }
        let flags = Int(d["StateFlags"] ?? "") ?? 0
        var total = Int64(d["BytesToDownload"] ?? "") ?? 0, done = Int64(d["BytesDownloaded"] ?? "") ?? 0
        // Steam escribe esos números en el appmanifest pocas veces: lo que ya bajó se ve mejor en steamapps/downloading
        // (sin comprimir: se compara con BytesToStage)
        let stage = Int64(d["BytesToStage"] ?? "") ?? 0
        let dl = steamapps.appendingPathComponent("downloading/\(id)")
        if stage > 0, fm.fileExists(atPath: dl.path) {
            let staged = folderBytes(dl)
            if total <= 0 || Double(staged) / Double(stage) > Double(done) / Double(max(total, 1)) { total = stage; done = min(staged, stage) }
        }
        let needs = flags & (1024 | 256 | 512) != 0   // (empezó, está bajando o en pausa; no «hay que actualizar» a secas)
        guard total > 0 || needs else { continue }
        if flags & 4 != 0 && !needs && done >= total { continue }   // (ya terminó)
        out.append(SteamDownload(id: id, name: d["name"] ?? id, done: done, total: total, paused: flags & 512 != 0 && flags & 256 == 0))
    }
    return out.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
}

/// La cuenta de Steam que entra sola (loginusers.vdf: la más reciente que guardó la sesión): su nombre visible
/// ¿Steam tiene una sesión iniciada ahora? (lo anota en el registro de Windows: ActiveProcess › ActiveUser, distinto de 0)
func steamSignedInNow(_ userReg: URL) -> Bool {
    guard let t = try? String(contentsOf: userReg, encoding: .utf8),
          let r = t.range(of: "[Software\\\\Valve\\\\Steam\\\\ActiveProcess]") else { return false }
    let block = t[r.upperBound...].prefix(4000).components(separatedBy: "\n\n").first ?? ""
    guard let v = block.range(of: "\"ActiveUser\"=dword:([0-9a-fA-F]{8})", options: .regularExpression) else { return false }
    return UInt32(block[v].suffix(8), radix: 16).map { $0 != 0 } ?? false
}

func steamSavedAccount(_ loginusers: URL, signedIn: Bool = false) -> String? {
    guard let t = try? String(contentsOf: loginusers, encoding: .utf8) else { return nil }
    // (un bloque por cuenta: "7656…" { "AccountName" "…" "PersonaName" "…" "RememberPassword" "1" "MostRecent" "1" })
    var best: String? = nil
    for block in t.components(separatedBy: "}") {
        func val(_ k: String) -> String? {
            guard let r = block.range(of: "\"\(k)\"\\s+\"([^\"]*)\"", options: .regularExpression) else { return nil }
            let m = block[r]; let parts = m.split(separator: "\"")
            return parts.count >= 3 ? String(parts[parts.count - 1]) : nil
        }
        // (con la sesión iniciada ahora, vale la última cuenta aunque no se haya guardado la contraseña)
        guard val("RememberPassword") == "1" || val("AllowAutoLogin") == "1" || (signedIn && val("MostRecent") == "1") else { continue }
        let name = val("PersonaName") ?? val("AccountName")
        if val("MostRecent") == "1" { return name }
        if best == nil { best = name }
    }
    return best
}

/// Imágenes de un juego en la tienda de Steam
func steamArt(_ id: String, _ file: String) -> URL? { URL(string: "https://cdn.cloudflare.steamstatic.com/steam/apps/\(id)/\(file)") }

/// Bytes en palabras («3,2 GB»)
func bytesText(_ b: Int64) -> String {
    let gb = Double(b) / 1_000_000_000
    let n = gb >= 1 ? String(format: "%.1f GB", gb) : String(format: "%.0f MB", Double(b) / 1_000_000)
    return idioma() == "es" ? n.replacingOccurrences(of: ".", with: ",") : n
}

let STEAM_BLUE = Color(red: 102 / 255, green: 192 / 255, blue: 244 / 255)

// MARK: - Portada (vertical, como la biblioteca de Steam; si el juego no tiene, su imagen horizontal con el nombre)
/// Un color para los juegos sin imagen (los de fuera de Steam), siempre el mismo para cada nombre
func nameGradient(_ name: String) -> LinearGradient {
    var h: UInt32 = 5381
    for u in name.unicodeScalars { h = (h &* 33) &+ u.value }
    let hue = Double(h % 360) / 360
    return LinearGradient(colors: [Color(hue: hue, saturation: 0.55, brightness: 0.75), Color(hue: hue, saturation: 0.7, brightness: 0.18)],
                          startPoint: .topLeading, endPoint: .bottom)
}

struct GameCapsule: View {
    let game: SteamGame
    let vr: Bool?
    let open: Bool
    var progress: Double? = nil   // bajándose: cuánto va
    let play: () -> Void
    @State private var hover = false

    private var named: some View {
        Text(game.name).font(.system(size: 15, weight: .heavy)).lineLimit(3).padding(10)
            .shadow(color: .black.opacity(0.6), radius: 6)
    }
    var body: some View {
        ZStack(alignment: .topTrailing) {
            Group {
                if game.exe != nil {   // de fuera de Steam: sin imagen de la tienda
                    ZStack(alignment: .bottomLeading) { Rectangle().fill(nameGradient(game.name)); named }
                } else {
                    AsyncImage(url: steamArt(game.id, "library_600x900.jpg")) { phase in
                        if let img = phase.image {
                            img.resizable().aspectRatio(contentMode: .fill)
                        } else if phase.error != nil {
                            ZStack(alignment: .bottomLeading) {
                                AsyncImage(url: steamArt(game.id, "header.jpg")) { i in i.resizable().aspectRatio(contentMode: .fill) } placeholder: { Rectangle().fill(nameGradient(game.name)) }
                                    .blur(radius: 18).overlay(Color.black.opacity(0.25))
                                named
                            }
                        } else {
                            ZStack { Color.white.opacity(0.06); ProgressView().controlSize(.small) }
                        }
                    }
                }
            }
            .frame(minWidth: 0, maxWidth: .infinity, minHeight: 0, maxHeight: .infinity).clipped()
            .saturation(game.inLibrary && progress == nil ? 1 : 0.3).opacity(game.inLibrary && progress == nil ? 1 : 0.75)
            if let p = progress {   // (como en Steam: la portada apagada y el avance abajo)
                VStack(spacing: 6) {
                    Spacer()
                    Text(p > 0 ? "\(Int(p * 100)) %" : L("Preparando…")).font(.system(size: 13, weight: .bold)).shadow(color: .black, radius: 4)
                    GeometryReader { geo in
                        ZStack(alignment: .leading) {
                            Capsule().fill(Color.black.opacity(0.5))
                            Capsule().fill(Theme.amber).frame(width: max(6, geo.size.width * p))
                                .animation(.easeOut(duration: 0.6), value: p)
                        }
                    }
                    .frame(height: 5)
                }
                .padding(10)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            Text(game.inGameHub && !game.inLibrary ? "GameHub" : !game.inLibrary ? L("Sin instalar") : game.exe != nil ? "PC" : vr == false ? L("Plano") : "VR").font(.system(size: 10, weight: .bold))
                .padding(.horizontal, 6).padding(.vertical, 2)
                .background(RoundedRectangle(cornerRadius: 5).fill(Color.black.opacity(0.55))).padding(7)
            if game.inLibrary && progress == nil {
                Button(action: play) {
                    Image(systemName: "play.fill").font(.system(size: 17, weight: .bold)).foregroundStyle(Theme.amberDark)
                        .frame(width: 46, height: 46).background(Circle().fill(Theme.amber))
                }
                .buttonStyle(.plain).opacity(hover && !open ? 1 : 0)
                .frame(maxWidth: .infinity, maxHeight: .infinity).offset(y: -12)
                .help(L("Jugar en el Quest"))
            }
        }
        .aspectRatio(2 / 3, contentMode: .fit)
        .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous).stroke(open || hover ? Theme.amber : .clear, lineWidth: 2))
        .shadow(color: .black.opacity(hover ? 0.6 : 0.35), radius: hover ? 16 : 8, y: hover ? 10 : 4)
        .scaleEffect(hover ? 1.04 : 1).offset(y: hover ? -6 : 0)
        .animation(.spring(response: 0.28, dampingFraction: 0.75), value: hover)
        .onHover { hover = $0 }
    }
}

/// Elegir el .exe de un juego de fuera de Steam (o el .bat que lo abre, como los ports que traen uno)
func pickLocalGame(_ m: AppModel) {
    let p = NSOpenPanel()
    p.title = L("Agregar un juego de fuera de Steam")
    p.message = L("Elegí el .exe del juego (de Windows), o el .bat que lo abre. El juego se queda donde está.")
    p.prompt = L("Agregar")
    p.allowedContentTypes = []
    p.allowsOtherFileTypes = true
    p.canChooseDirectories = false
    p.allowsMultipleSelection = false
    guard p.runModal() == .OK, let url = p.url else { return }
    if !["exe", "bat"].contains(url.pathExtension.lowercased()) { m.append(L("No se pudo agregar %@ (tiene que ser un .exe o un .bat de Windows)", url.lastPathComponent)); return }
    m.addLocalGame(url)
}

// MARK: - Biblioteca
struct LibraryView: View {
    @EnvironmentObject var m: AppModel
    @Binding var gameAdv: SteamGame?
    @State private var query = ""
    @State private var selectedID: String? = nil
    @AppStorage("libraryOrder") private var order = "nombre"   // nombre o recientes
    @AppStorage("librarySideFolded") private var folded = false   // la lista de la izquierda, solo con los íconos
    @AppStorage("libraryShowAll") private var showAll = false   // también los juegos de tu cuenta sin instalar
    @AppStorage("libraryKind") private var kind = "vr"   // vr o plano: qué juegos muestra la lista
    @ObservedObject private var account = AccountGames.shared

    private var games: [SteamGame] {
        // (los que se están bajando aparecen desde que empiezan, como en Steam)
        let bajando = m.downloads.filter { d in !m.games.contains { $0.id == d.id } }
            .map { SteamGame(id: $0.id, name: $0.name, installdir: "", inLibrary: true, inGameHub: false) }
        // («Todos»: también los juegos de VR de tu cuenta que no están instalados, como en Steam)
        let cuenta = kind == "plano" ? account.flat : account.vr
        let sinInstalar = !showAll ? [] : cuenta.filter { a in !m.games.contains { $0.id == a.id } && !bajando.contains { $0.id == a.id } }
            .map { SteamGame(id: $0.id, name: $0.name, installdir: "", inLibrary: false, inGameHub: false) }
        // (los planos son los que la tienda de Steam dice que no son de VR; los demás, en VR)
        let instalados = (m.games + bajando).filter { (m.vr[$0.id]?.vr == false) == (kind == "plano") }
        let l = (instalados + sinInstalar).filter { query.isEmpty || $0.name.localizedCaseInsensitiveContains(query) }
        // (los de GameHub, al final: se traen antes de jugar)
        return l.sorted { a, b in
            if a.inLibrary != b.inLibrary { return a.inLibrary }
            if order == "recientes" && a.lastPlayed != b.lastPlayed { return a.lastPlayed > b.lastPlayed }
            return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
        }
    }
    // (también los que se están bajando o están sin instalar: su página tiene «Instalar»)
    private var selected: SteamGame? { selectedID.flatMap { id in m.games.first { $0.id == id } ?? games.first { $0.id == id } } }
    private func isOpen(_ g: SteamGame) -> Bool { g.inLibrary && m.openGames.contains(g.installdir.lowercased()) }
    @State private var lastOpenedID: String? = nil   // (al volver a la grilla, queda donde estaba: en ese juego)
    private func select(_ g: SteamGame?) {
        if let g { lastOpenedID = g.id }
        withAnimation(.easeInOut(duration: 0.25)) { selectedID = g?.id }
    }

    var body: some View {
        GeometryReader { geo in
            // (con la ventana angosta, la lista queda plegada: solo los íconos)
            let small = folded || geo.size.width < 760
            HStack(spacing: 0) {
                sidebar(small, canUnfold: geo.size.width >= 760)
                Rectangle().fill(Theme.line).frame(width: 1)
                // (la página de un juego en lugar de la grilla, no encima: la grilla detrás se acomodaba a su ancho
                // y corría todo hacia la izquierda, una y otra vez. Y el ancho, fijo: el que queda al lado de la lista)
                ZStack {
                    if let g = selected {
                        GameDetail(game: g, gameAdv: $gameAdv) { select(nil) }
                            .id(g.id)   // (cada juego, su propia página: si no, quedaban datos del anterior)
                            .transition(.opacity.combined(with: .offset(y: 24)))
                    } else {
                        grid.transition(.opacity)
                    }
                }
                .frame(width: max(0, geo.size.width - (small ? 58 : 210) - 1)).frame(maxHeight: .infinity).clipped()
                .safeAreaInset(edge: .bottom, spacing: 0) { DownloadsBar() }
            }
        }
        .onAppear {
            AccountGames.shared.start(steamDir: m.steamapps.deletingLastPathComponent(), cache: m.data.appendingPathComponent("tienda_cache.json"))
        }
    }

    private func sidebar(_ small: Bool, canUnfold: Bool) -> some View {
        VStack(alignment: small ? .center : .leading, spacing: 6) {
            HStack(spacing: 6) {
                if !small {
                    HStack(spacing: 6) {
                        Image(systemName: "magnifyingglass").foregroundStyle(Theme.text2)
                        TextField(L("Buscar en tu biblioteca"), text: $query).textFieldStyle(.plain)
                    }
                    .padding(.horizontal, 9).padding(.vertical, 6)
                    .background(RoundedRectangle(cornerRadius: 7).fill(Color.white.opacity(0.07)))
                }
                if canUnfold {
                    Button { withAnimation(.easeInOut(duration: 0.2)) { folded.toggle(); if folded { query = "" } } } label: {
                        Image(systemName: "sidebar.left").frame(width: 26, height: 26)
                            .background(RoundedRectangle(cornerRadius: 7).fill(Color.white.opacity(0.07)))
                    }
                    .buttonStyle(.plain).help(folded ? L("Mostrar la lista") : L("Plegar la lista"))
                }
            }
            if !small {
                Picker("", selection: $kind) {
                    Text("VR").tag("vr")
                    Text(L("Planos")).tag("plano")
                }
                .pickerStyle(.segmented).labelsHidden().padding(.top, 6)
                Text(L("JUEGOS · %d", games.count)).font(.system(size: 11, weight: .semibold)).foregroundStyle(Theme.text2)
                    .padding(.top, 4).padding(.leading, 4)
            } else {
                Button { kind = kind == "vr" ? "plano" : "vr" } label: {
                    Text(kind == "vr" ? "VR" : L("Plano")).font(.system(size: 10, weight: .bold)).frame(width: 34, height: 20)
                        .background(RoundedRectangle(cornerRadius: 5).fill(Theme.amber.opacity(0.18)))
                }
                .buttonStyle(.plain).help(kind == "vr" ? L("Mostrando los juegos de VR: tocá para ver los planos") : L("Mostrando los juegos planos: tocá para ver los de VR"))
            }
            ScrollView {
                VStack(alignment: small ? .center : .leading, spacing: 2) {
                    ForEach(games) { g in
                        Button { select(g) } label: {
                            HStack(spacing: 8) {
                                Group {
                                    if g.exe != nil { Rectangle().fill(nameGradient(g.name)) }
                                    else { AsyncImage(url: steamArt(g.id, "capsule_sm_120.jpg")) { i in i.resizable().aspectRatio(contentMode: .fill) } placeholder: { Color.white.opacity(0.08) } }
                                }
                                .frame(width: small ? 30 : 18, height: small ? 30 : 18).clipShape(RoundedRectangle(cornerRadius: small ? 6 : 3))
                                .overlay(alignment: .topTrailing) {
                                    if small && isOpen(g) { Circle().fill(Theme.amber).frame(width: 8, height: 8).offset(x: 3, y: -3) }
                                }
                                if !small {
                                    Text(g.name).lineLimit(1)
                                    Spacer(minLength: 0)
                                    if isOpen(g) { Circle().fill(Theme.amber).frame(width: 6, height: 6) }
                                }
                            }
                            .font(.system(size: 12.5))
                            .foregroundStyle(selectedID == g.id ? Theme.text : Theme.text2)
                            .opacity(g.inLibrary ? 1 : 0.6)
                            .padding(.horizontal, small ? 4 : 6).padding(.vertical, small ? 3 : 5)
                            .background(RoundedRectangle(cornerRadius: 6).fill(selectedID == g.id ? Theme.amber.opacity(0.16) : .clear))
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .help(g.name)
                    }
                }
            }
            Button { pickLocalGame(m) } label: {
                if small { Image(systemName: "plus") } else { Label(L("Agregar un juego"), systemImage: "plus") }
            }
            .buttonStyle(Pill()).help(L("Un juego de fuera de Steam: elegís su .exe (o su .bat)"))
        }
        .padding(10)
        .frame(width: small ? 58 : 210)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(LinearGradient(colors: [Theme.amber.opacity(0.025), Color.white.opacity(0.02)], startPoint: .top, endPoint: .bottom))
    }

    private var grid: some View {
        ScrollViewReader { proxy in
        ScrollView {
            VStack(alignment: .leading, spacing: 4) {
                HStack(alignment: .firstTextBaseline) {
                    Text(L("Biblioteca")).font(.system(size: 18, weight: .bold))
                    Spacer()
                    Picker("", selection: $showAll) {
                        Text(L("Instalados")).tag(false)
                        Text(L("Todos")).tag(true)
                    }
                    .pickerStyle(.segmented).labelsHidden().fixedSize()
                    .help(L("«Todos» suma los juegos de VR de tu cuenta que no están instalados"))
                    Picker(L("Ordenar"), selection: $order) {
                        Text(L("Por nombre")).tag("nombre")
                        Text(L("Jugados hace poco")).tag("recientes")
                    }
                    .pickerStyle(.menu).fixedSize()
                }
                Text(m.steamRunning ? L("Tus juegos, listos para el Quest") : L("Tus juegos. Los de Steam necesitan que Steam esté abierto."))
                    .font(.callout).foregroundStyle(Theme.text2).padding(.bottom, 10)
                if !m.steamRunning && m.steamInstalled {
                    HStack {
                        Image(systemName: "info.circle").foregroundStyle(Theme.amber)
                        Text(L("Steam está cerrado.")).font(.callout)
                        Spacer()
                        Button(L("Abrir Steam")) { m.run(L("Abriendo Steam"), "abrir_steam.sh") }.buttonStyle(Pill(primary: true)).disabled(m.busyTask != nil)
                    }
                    .padding(10).background(RoundedRectangle(cornerRadius: 8).fill(Theme.amber.opacity(0.12))).padding(.bottom, 8)
                }
                steamVRRow
                if games.isEmpty {
                    Text(query.isEmpty ? L("Todavía no hay juegos. Buscalos en la Tienda, o agregá uno de fuera de Steam con «Agregar un juego».") : L("Ningún juego se llama así."))
                        .font(.callout).foregroundStyle(Theme.text2).padding(.top, 20)
                }
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 130, maximum: 190), spacing: 16)], spacing: 16) {
                    ForEach(games) { g in
                        GameCapsule(game: g, vr: m.vr[g.id]?.vr, open: isOpen(g), progress: m.downloads.first { $0.id == g.id }?.fraction) { m.play(g) }
                            .onTapGesture { select(g) }
                            .id(g.id)
                    }
                }
                .padding(.vertical, 8)
            }
            .padding(18)
        }
        .onAppear { if let id = lastOpenedID { proxy.scrollTo(id, anchor: .center) } }
        }
    }

    /// SteamVR solo (como al ponerse un Valve Index), o el botón para instalarlo
    @ViewBuilder private var steamVRRow: some View {
        if m.steamVRInstalled || m.steamInstalled {
            HStack(spacing: 10) {
                Image(systemName: "visionpro").foregroundStyle(m.steamVRInstalled ? Theme.amber : Theme.text2)
                VStack(alignment: .leading, spacing: 2) {
                    Text("SteamVR").font(.system(size: 13, weight: .semibold))
                    Text(!m.steamVRInstalled ? L("Sin instalar. Con SteamVR real tenés su menú en el visor, y los juegos hechos para otros controles andan mejor.")
                         : m.steamVRRunning ? L("Abierto en el visor: su menú tiene tu biblioteca de Steam")
                         : L("Su sala y su menú en el visor, con tu biblioteca de Steam (experimental)"))
                        .font(.caption).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
                }
                Spacer()
                if !m.steamVRInstalled {
                    Button(L("Instalar SteamVR")) { m.installSteamVR() }.buttonStyle(Pill())
                } else if m.steamVRRunning {
                    Button(L("Cerrar SteamVR")) { m.closeSteamVR() }.buttonStyle(Pill())
                } else {
                    Button(L("Abrir SteamVR")) { m.openSteamVR() }.buttonStyle(Pill(primary: true))
                }
            }
            .padding(10)
            .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Color.white.opacity(0.05)))
            .padding(.bottom, 6)
        }
    }
}

// MARK: - La página de un juego
struct GameDetail: View {
    @EnvironmentObject var m: AppModel
    let game: SteamGame
    @Binding var gameAdv: SteamGame?
    let back: () -> Void
    @State private var size: Int64 = 0

    private var open: Bool { game.inLibrary && m.openGames.contains(game.installdir.lowercased()) }
    private static let runtimes: [(id: String, name: String)] = [
        ("auto", "Automático"), ("openxr", "OpenXR"), ("openvr", "OpenComposite"), ("steamvr", "SteamVR real"), ("plano", "Sin VR"),
    ]

    var body: some View {
        let v = m.vr[game.id]
        GeometryReader { geo in
            // (con la ventana angosta, los datos van debajo del botón y los cuadros, uno debajo del otro)
            let wide = geo.size.width >= 900, mid = geo.size.width >= 720
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    hero
                    Group {
                        if wide {
                            HStack(spacing: 18) { mainButton; stats(v); Spacer(minLength: 0); moreMenu(v) }
                        } else {
                            VStack(alignment: .leading, spacing: 12) {
                                HStack { mainButton; Spacer(minLength: 0); moreMenu(v) }
                                HStack(spacing: 18) { stats(v) }
                            }
                        }
                    }
                    .padding(.horizontal, 26).padding(.top, 4)
                    // (sin instalar no hay nada que elegir: se elige después de bajarlo)
                    if game.inLibrary {
                        Group {
                            if mid {
                                HStack(alignment: .top, spacing: 14) { vrCard(v, segmented: true); qualityCard.frame(width: 220) }
                            } else {
                                VStack(alignment: .leading, spacing: 14) { vrCard(v, segmented: geo.size.width >= 600); qualityCard }
                            }
                        }
                        .padding(.horizontal, 26).padding(.vertical, 18)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            // (sin barra de desplazamiento, que se desplaza igual: con el Mac en «mostrar siempre las barras», la barra
            // aparecía y desaparecía, cada vez el contenido cambiaba de ancho y la página se corría a un lado y volvía)
            .scrollIndicators(.never)
        }
        .background(Theme.bg)
        .onAppear {
            let acf = (game.library ?? m.steamapps).appendingPathComponent("appmanifest_\(game.id).acf")
            size = Int64(parseACF(acf)["SizeOnDisk"] ?? "") ?? 0
        }
    }

    private var hero: some View {
        ZStack(alignment: .bottomLeading) {
            Group {
                if game.exe != nil { Rectangle().fill(nameGradient(game.name)) }
                else {
                    AsyncImage(url: steamArt(game.id, "library_hero.jpg")) { phase in
                        if let i = phase.image { i.resizable().aspectRatio(contentMode: .fill) }
                        else if phase.error != nil {
                            AsyncImage(url: steamArt(game.id, "header.jpg")) { i in i.resizable().aspectRatio(contentMode: .fill) } placeholder: { Color.white.opacity(0.05) }
                                .blur(radius: 12)
                        } else { Color.white.opacity(0.05) }
                    }
                }
            }
            .frame(minWidth: 0, maxWidth: .infinity, minHeight: 240, maxHeight: 240).clipped()
            LinearGradient(colors: [.clear, Theme.bg], startPoint: .center, endPoint: .bottom)
            Group {
                if game.exe != nil { title }
                else {
                    AsyncImage(url: steamArt(game.id, "logo.png")) { phase in
                        if let i = phase.image { i.resizable().aspectRatio(contentMode: .fit).frame(maxWidth: 320, maxHeight: 96, alignment: .bottomLeading) }
                        else { title }
                    }
                }
            }
            .padding(.leading, 26).padding(.bottom, 20)
            Button(action: back) { Label(L("Biblioteca"), systemImage: "chevron.left").font(.callout) }
                .buttonStyle(.plain).padding(.horizontal, 10).padding(.vertical, 5)
                .background(RoundedRectangle(cornerRadius: 7).fill(Color.black.opacity(0.45)))
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading).padding(14)
        }
        .frame(height: 240)
    }
    private var title: some View {
        Text(game.name).font(.system(size: 32, weight: .heavy)).lineLimit(2).minimumScaleFactor(0.6).shadow(color: .black.opacity(0.6), radius: 10)
    }

    @ViewBuilder private var mainButton: some View {
        if let d = m.downloads.first(where: { $0.id == game.id }) {
            HStack(spacing: 10) {
                Button {} label: { Label(d.paused ? L("EN PAUSA · %d %%", Int(d.fraction * 100)) : d.total > 0 ? L("DESCARGANDO · %d %%", Int(d.fraction * 100)) : L("PREPARANDO LA DESCARGA"), systemImage: d.paused ? "pause.circle" : "arrow.down.circle") }
                    .buttonStyle(BigButton()).disabled(true)
                DownloadControls(d: d)
            }
        } else if let t = m.uninstalling[game.id] {
            // (Steam tarda en mostrar su ventanita de confirmar: mientras, que se vea que se está haciendo)
            HStack(spacing: 12) {
                Button {} label: { Label(L("DESINSTALANDO…"), systemImage: "trash") }.buttonStyle(BigButton()).disabled(true)
                TimelineView(.periodic(from: t, by: 5)) { c in
                    Text(c.date.timeIntervalSince(t) < 15 ? L("Pidiéndoselo a Steam…") : L("Confirmalo en la ventanita de Steam."))
                        .font(.callout).foregroundStyle(Theme.text2)
                }
            }
        } else if !game.inLibrary && !game.inGameHub {
            Button { m.installGame(game.id, game.name) } label: { Label(L("INSTALAR"), systemImage: "arrow.down") }
                .buttonStyle(BigButton()).disabled(!m.steamInstalled)
        } else if !game.inLibrary {
            Button { m.importGame(game) } label: { Label(L("TRAER DE GAMEHUB"), systemImage: "arrow.down") }
                .buttonStyle(BigButton()).disabled(m.busyTask != nil || !m.steamInstalled || m.steamRunning)
                .help(m.steamRunning ? L("Cerrá Steam primero") : L("Copia instantánea desde GameHub"))
        } else if open {
            Button { m.forceClose(game) } label: { Label(L("FORZAR CIERRE"), systemImage: "xmark") }
                .buttonStyle(BigButton()).help(L("Cierra el juego aunque no responda (Steam sigue abierto)"))
        } else if m.steamRunning || game.exe != nil {
            HStack(spacing: 10) {
                Button { m.play(game) } label: { Label(L("JUGAR EN EL QUEST"), systemImage: "play.fill") }.buttonStyle(BigButton())
                Button { m.playOnMac(game) } label: { Label(L("En la Mac"), systemImage: "macbook") }
                    .buttonStyle(Pill()).fixedSize().help(L("Abrirlo sin VR, en una ventana de la Mac (sin el Quest)"))
            }
        } else {
            Button { m.run(L("Abriendo Steam"), "abrir_steam.sh") } label: { Label(L("ABRIR STEAM"), systemImage: "arrow.right") }
                .buttonStyle(BigButton()).disabled(m.busyTask != nil)
        }
    }

    @ViewBuilder private func stats(_ v: VRJuego?) -> some View {
        stat(open ? L("Abierto") : estado(v), L("Estado"))
        stat(v.map { L($0.runtimeNombre) } ?? "…", L("Sistema de VR"))
        if game.lastPlayed > 0 { stat(lastPlayedText, L("Última vez")) }
        if size > 0 { stat(bytesText(size), L("Tamaño")) }
    }
    private var lastPlayedText: String {
        let f = RelativeDateTimeFormatter(); f.locale = Locale(identifier: idioma() == "es" ? "es_AR" : "en_US")
        return f.localizedString(for: Date(timeIntervalSince1970: TimeInterval(game.lastPlayed)), relativeTo: Date())
    }

    private func vrCard(_ v: VRJuego?, segmented: Bool) -> some View {
        let pick = Picker(L("Sistema de VR"), selection: Binding(get: { v?.perfil.pedido ?? "auto" }, set: { m.setVR(game.id, "runtime", $0) })) {
            ForEach(Self.runtimes, id: \.id) { o in Text(L(o.name)).tag(o.id) }
        }
        return VStack(alignment: .leading, spacing: 8) {
            Text(L("Sistema de VR")).font(.system(size: 13, weight: .semibold))
            // (las cinco opciones juntas no se achican: con la ventana angosta, en un menú)
            Group {
                if segmented { pick.pickerStyle(.segmented) } else { pick.pickerStyle(.menu).fixedSize() }
            }
            .labelsHidden().disabled(v == nil || !game.inLibrary)
            Text(L("Si un juego anda mal con OpenComposite, probá SteamVR real, que suele ser lo más estable; si SteamVR real va lento o no abre, probá OpenComposite."))
                .font(.caption).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .topLeading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Theme.card))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.line))
    }
    private var qualityCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(L("Calidad")).font(.system(size: 13, weight: .semibold))
            Text(m.adv[game.installdir] != nil ? L("Este juego tiene ajustes propios.") : L("Usa los ajustes de «Todos los juegos»."))
                .font(.caption).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
            Button(L("Calidad y rendimiento…")) { m.loadAdv(); gameAdv = game }.buttonStyle(Pill()).disabled(!game.inLibrary)
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .topLeading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Theme.card))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.line))
    }

    /// Lo demás de un juego: cómo abrirlo, diagnóstico, estado, registros, desinstalar
    private func moreMenu(_ v: VRJuego?) -> some View {
        Menu {
            if let v {
                if game.exe == nil {
                    Picker(L("Cómo abrirlo"), selection: Binding(get: { v.perfil.lanzar }, set: { m.setVR(game.id, "lanzar", $0) })) {
                        Text(L("Desde Steam")).tag("steam")
                        Text(L("Directo (para probar)")).tag("directo")
                    }
                }
                Toggle(L("Diagnóstico detallado (al abrir directo)"), isOn: Binding(get: { v.perfil.diagnostico },
                                                                              set: { m.setVR(game.id, "diagnostico", $0 ? "1" : "0") }))
                Picker(L("Estado"), selection: Binding(get: { v.perfil.estado }, set: { m.setVR(game.id, "estado", $0) })) {
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
                Button(L("Volver a detectar")) { m.redetect(game.id) }
                Button(L("Juntar registros del juego")) { m.gameLogs(game) }
            }
            if let exe = game.exe {
                Button(L("Mostrar en el Finder")) { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: exe)]) }
                Button(L("Quitar de la biblioteca")) { back(); m.removeLocalGame(game) }
            } else if game.inLibrary {
                Button(L("Ver en la tienda de Steam")) { NSWorkspace.shared.open(URL(string: "https://store.steampowered.com/app/\(game.id)")!) }
                Button(L("Desinstalar…")) { m.uninstallGame(game) }.disabled(!m.steamRunning)
            }
        } label: {
            Image(systemName: "ellipsis").frame(width: 30, height: 30)
                .background(RoundedRectangle(cornerRadius: 7).fill(Color.white.opacity(0.08)))
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
        .help(L("Más opciones de este juego"))
    }

    private func estado(_ v: VRJuego?) -> String {
        if !game.inLibrary { return game.inGameHub ? L("En GameHub") : L("Sin instalar") }
        switch v?.perfil.estado {
        case "funciona": return L("Funciona")
        case "con problemas": return L("Anda con problemas")
        case "no funciona": return L("No funciona por ahora")
        case nil: return L("Revisando el juego…")
        default: return L("Sin probar todavía")
        }
    }
    private func stat(_ value: String, _ label: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(value).font(.system(size: 13, weight: .semibold)).lineLimit(1)
            Text(label).font(.caption).foregroundStyle(Theme.text2)
        }
    }
}

/// El botón grande de jugar (como el de Steam, en ámbar)
struct BigButton: ButtonStyle {
    @Environment(\.isEnabled) private var enabled
    func makeBody(configuration c: Configuration) -> some View {
        c.label
            .font(.system(size: 15, weight: .heavy))
            .padding(.horizontal, 30).padding(.vertical, 11)
            .foregroundStyle(Theme.amberDark)
            .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Theme.amber.opacity(c.isPressed ? 0.8 : 1)))
            .scaleEffect(c.isPressed ? 0.96 : 1)
            .animation(.easeOut(duration: 0.12), value: c.isPressed)
            .opacity(enabled ? 1 : 0.4)
            .fixedSize()
    }
}

/// «· 24 MB/s · faltan 3 min»
func speedText(_ d: SteamDownload) -> String {
    guard d.speed > 100_000, !d.paused else { return "" }
    var t = String(format: " · %.0f MB/s", d.speed / 1_000_000)
    let left = Double(d.total - d.done) / d.speed
    if left > 0 && left < 86400 {
        t += " · " + (left < 60 ? L("falta menos de un minuto") : L("faltan %d min", Int((left / 60).rounded(.up))))
    }
    return idioma() == "es" ? t.replacingOccurrences(of: ".", with: ",") : t
}

// MARK: - Barra de descargas (abajo, como la de Steam, en ámbar)
struct DownloadsBar: View {
    @EnvironmentObject var m: AppModel
    var body: some View {
        if let d = m.downloads.first {
            let k = d.fraction
            HStack(spacing: 12) {
                Text(d.paused ? L("En pausa") : L("Descargando")).font(.system(size: 13, weight: .bold)).foregroundStyle(Theme.amberText)
                Text(d.name).font(.callout).lineLimit(1)
                if m.downloads.count > 1 { Text(L("y %d más", m.downloads.count - 1)).font(.caption).foregroundStyle(Theme.text2) }
                GeometryReader { geo in
                    ZStack(alignment: .leading) {
                        Capsule().fill(Color.white.opacity(0.1))
                        Capsule().fill(LinearGradient(colors: [Theme.amberText, Theme.amber], startPoint: .leading, endPoint: .trailing))
                            .frame(width: geo.size.width * k)
                            .animation(.easeOut(duration: 0.6), value: k)
                    }
                }
                .frame(height: 6).frame(minWidth: 60)
                Text(d.total <= 0 ? L("Preparando…") : "\(Int(k * 100)) % · \(bytesText(d.done)) / \(bytesText(d.total))" + speedText(d))
                    .font(.caption.monospacedDigit()).foregroundStyle(Theme.text2).lineLimit(1).fixedSize()
                DownloadControls(d: d)
            }
            .padding(.horizontal, 16).frame(height: 46)
            .background(Theme.amber.opacity(0.10))
            .background(Theme.bg)
            .overlay(Rectangle().fill(Theme.amber.opacity(0.5)).frame(height: 1), alignment: .top)
            .transition(.move(edge: .bottom))
        }
    }
}

/// Pausar / seguir y cancelar (con confirmación) una descarga
struct DownloadControls: View {
    @EnvironmentObject var m: AppModel
    let d: SteamDownload
    @State private var askCancel = false
    var body: some View {
        HStack(spacing: 6) {
            Button { m.pauseDownload(d) } label: { Image(systemName: d.paused ? "play.fill" : "pause.fill").frame(width: 14) }
                .help(d.paused ? L("Seguir la descarga") : L("Pausar la descarga"))
            Button { askCancel = true } label: { Image(systemName: "xmark").frame(width: 14) }
                .help(L("Cancelar la descarga"))
        }
        .buttonStyle(Pill())
        .alert(L("¿Cancelar la descarga de %@?", d.name), isPresented: $askCancel) {
            Button(L("Cancelar la descarga"), role: .destructive) { m.cancelDownload(d) }
            Button(L("Seguir bajando"), role: .cancel) {}
        } message: {
            Text(L("Se borra lo que ya bajó. Steam se reinicia de fondo un momento."))
        }
    }
}

// MARK: - Tienda (la búsqueda de la tienda de Steam; instalar se lo pide al Steam de VRmin)
struct StoreItem: Identifiable, Decodable {
    struct Price: Decodable { let currency: String?; let final: Int? }
    let id: Int; let name: String; let tiny_image: String?; let price: Price?
}

/// Los juegos de VR de tu cuenta que no están instalados (para la Tienda, como en GameHub): los de tu biblioteca de Steam
/// (appcache/librarycache tiene una carpeta por cada uno, jugado o no; la configuración de tus cuentas solo tiene los que
/// abriste en esta compu), y si son de VR según la tienda (se pregunta de a uno, despacio, y se guarda en tienda_cache.json)
final class AccountGames: ObservableObject {
    static let shared = AccountGames()
    struct Info: Codable { let n: String; let vr: Bool; var game: Bool? = nil }   // (game false: no es un juego: programas, demos, contenido)
    @Published var vr: [(id: String, name: String)] = []
    @Published var flat: [(id: String, name: String)] = []
    @Published var checking: (done: Int, total: Int)? = nil
    private var started = false

    func start(steamDir: URL, cache: URL) {
        guard !started else { return }
        started = true
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self else { return }
            var ids = Set<String>()
            let lib = steamDir.appendingPathComponent("appcache/librarycache")
            for a in (try? FileManager.default.contentsOfDirectory(atPath: lib.path)) ?? [] where !a.isEmpty && a.allSatisfy(\.isNumber) {
                ids.insert(a)
            }
            let ud = steamDir.appendingPathComponent("userdata")
            for u in (try? FileManager.default.contentsOfDirectory(atPath: ud.path)) ?? [] {
                guard let t = try? String(contentsOf: ud.appendingPathComponent("\(u)/config/localconfig.vdf"), encoding: .utf8),
                      let r = t.range(of: "\"apps\"") else { continue }
                let rest = String(t[r.upperBound...].prefix(600_000))
                if let re = try? NSRegularExpression(pattern: "\n\t{5}\"(\\d+)\"\n\t{5}\\{") {
                    for mm in re.matches(in: rest, range: NSRange(rest.startIndex..., in: rest)) {
                        if let g = Range(mm.range(at: 1), in: rest) { ids.insert(String(rest[g])) }
                    }
                }
            }
            var known = (try? JSONDecoder().decode([String: Info].self, from: Data(contentsOf: cache))) ?? [:]
            func publish() {
                func list(_ vr: Bool) -> [(id: String, name: String)] {
                    known.filter { ids.contains($0.key) && $0.value.vr == vr && ($0.value.game ?? ($0.value.n != $0.key)) }.map { (id: $0.key, name: $0.value.n) }
                        .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
                }
                let v = list(true), f = list(false)
                DispatchQueue.main.async { self.vr = v; self.flat = f }
            }
            publish()
            let missing = ids.filter { known[$0] == nil }.sorted { (Int($0) ?? 0) < (Int($1) ?? 0) }
            for (i, id) in missing.enumerated() {
                DispatchQueue.main.async { self.checking = (i, missing.count) }
                guard let url = URL(string: "https://store.steampowered.com/api/appdetails?appids=\(id)&l=english"),
                      let d = try? Data(contentsOf: url),
                      let j = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any],
                      let e = j[id] as? [String: Any] else { Thread.sleep(forTimeInterval: 5); continue }
                if let x = e["data"] as? [String: Any], (x["type"] as? String) == "game" {
                    let cats = (x["categories"] as? [[String: Any]] ?? []).compactMap { $0["description"] as? String }
                    known[id] = Info(n: x["name"] as? String ?? id, vr: cats.contains { $0.contains("VR") }, game: true)
                } else {
                    known[id] = Info(n: id, vr: false, game: false)
                }
                if i % 10 == 9 || i == missing.count - 1 {
                    try? JSONEncoder().encode(known).write(to: cache)
                    publish()
                }
                Thread.sleep(forTimeInterval: 1.3)   // (la tienda no quiere muchas preguntas seguidas)
            }
            DispatchQueue.main.async { self.checking = nil }
        }
    }
}

struct StoreView: View {
    @EnvironmentObject var m: AppModel
    @ObservedObject private var account = AccountGames.shared
    @State private var query = ""
    @State private var results: [StoreItem] = []
    @State private var searching = false
    @State private var failed = false
    @State private var task: DispatchWorkItem? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(L("Tienda")).font(.system(size: 18, weight: .bold))
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass").foregroundStyle(Theme.text2)
                TextField(L("Buscar en la tienda de Steam"), text: $query).textFieldStyle(.plain).onSubmit { search() }
                if searching { ProgressView().controlSize(.small) }
            }
            .padding(.horizontal, 10).padding(.vertical, 7).frame(maxWidth: 420)
            .background(RoundedRectangle(cornerRadius: 7).fill(Color.white.opacity(0.07)))
            .onChange(of: query) { _ in
                task?.cancel()
                let w = DispatchWorkItem { search() }
                task = w
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.45, execute: w)
            }
            if failed { Text(L("No se pudo buscar. ¿Hay internet?")).font(.callout).foregroundStyle(Theme.amberText) }
            Text(results.isEmpty ? L("Buscá un juego por su nombre. Instalarlo lo baja en el Steam de VRmin, y aparece en tu Biblioteca y en el Quest.")
                                 : L("Resultados de la tienda de Steam"))
                .font(.callout).foregroundStyle(Theme.text2)
            ScrollView {
                if results.isEmpty && query.isEmpty { accountSection }
                VStack(spacing: 10) {
                    ForEach(results) { r in row(r) }
                }
            }
        }
        .padding(18)
        .onAppear {
            let steam = m.steamapps.deletingLastPathComponent()
            AccountGames.shared.start(steamDir: steam, cache: m.data.appendingPathComponent("tienda_cache.json"))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .safeAreaInset(edge: .bottom, spacing: 0) { DownloadsBar() }
    }

    /// Tus juegos de VR sin instalar, con su botón de instalar
    @ViewBuilder private var accountSection: some View {
        let list = account.vr.filter { a in !m.games.contains { $0.id == a.id } }
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(L("Tus juegos de VR sin instalar")).font(.system(size: 15, weight: .semibold))
                if let c = account.checking {
                    ProgressView().controlSize(.small)
                    Text(L("Revisando tu cuenta… %d de %d", c.done, c.total)).font(.caption).foregroundStyle(Theme.text2)
                }
            }
            if list.isEmpty && account.checking == nil {
                Text(L("No encontramos juegos de VR tuyos sin instalar.")).font(.callout).foregroundStyle(Theme.text2)
            }
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 200, maximum: 280), spacing: 14)], spacing: 14) {
                ForEach(list, id: \.id) { a in
                    let downloading = m.downloads.contains { $0.id == a.id }
                    VStack(alignment: .leading, spacing: 6) {
                        AsyncImage(url: steamArt(a.id, "header.jpg")) { i in i.resizable().aspectRatio(contentMode: .fill) } placeholder: { Color.white.opacity(0.06) }
                            .aspectRatio(460 / 215, contentMode: .fit).clipShape(RoundedRectangle(cornerRadius: 7))
                        HStack {
                            Text(a.name).font(.system(size: 12.5, weight: .semibold)).lineLimit(1)
                            Spacer()
                            if downloading { Text(L("Descargando")).font(.caption.weight(.semibold)).foregroundStyle(Theme.amberText) }
                            else { Button(L("Instalar")) { m.installGame(a.id, a.name) }.buttonStyle(Pill(primary: true)).disabled(!m.steamInstalled) }
                        }
                    }
                }
            }
        }
        .padding(.bottom, 16)
    }

    private func row(_ r: StoreItem) -> some View {
        let id = String(r.id)
        let mine = m.games.first { $0.id == id && $0.inLibrary }
        let downloading = m.downloads.contains { $0.id == id }
        return HStack(spacing: 12) {
            AsyncImage(url: steamArt(id, "capsule_231x87.jpg")) { i in i.resizable().aspectRatio(contentMode: .fill) } placeholder: { Color.white.opacity(0.06) }
                .frame(width: 130, height: 49).clipShape(RoundedRectangle(cornerRadius: 6))
            VStack(alignment: .leading, spacing: 2) {
                Text(r.name).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                if let v = mine.flatMap({ m.vr[$0.id] }) { Text(L("Sistema de VR: %@", L(v.runtimeNombre))).font(.caption).foregroundStyle(Theme.text2) }
            }
            Spacer()
            if mine != nil {
                Text(L("En tu biblioteca")).font(.caption.weight(.semibold)).foregroundStyle(Color(red: 190 / 255, green: 238 / 255, blue: 17 / 255))
            } else if downloading {
                Text(L("Descargando")).font(.caption.weight(.semibold)).foregroundStyle(STEAM_BLUE)
            } else {
                if let p = r.price, let c = p.final, let cur = p.currency {
                    Text(priceText(c, cur)).font(.caption).foregroundStyle(Theme.text2)
                }
                Button(L("Instalar")) { m.installGame(id, r.name) }.buttonStyle(Pill(primary: true)).disabled(!m.steamInstalled)
                    .help(L("Si el juego es tuyo, se baja de fondo y lo ves en la barra de abajo. Si no, Steam te muestra dónde comprarlo."))
            }
            Button { NSWorkspace.shared.open(URL(string: "https://store.steampowered.com/app/\(id)")!) } label: { Image(systemName: "safari") }
                .buttonStyle(Pill()).help(L("Ver en la tienda de Steam"))
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 9).fill(Theme.card))
        .overlay(RoundedRectangle(cornerRadius: 9).strokeBorder(Theme.line))
    }

    private func priceText(_ cents: Int, _ currency: String) -> String {
        let f = NumberFormatter(); f.numberStyle = .currency; f.currencyCode = currency
        f.locale = Locale(identifier: idioma() == "es" ? "es_AR" : "en_US")
        return f.string(from: NSNumber(value: Double(cents) / 100)) ?? ""
    }

    private func search() {
        let q = query.trimmingCharacters(in: .whitespaces)
        guard q.count >= 2, var c = URLComponents(string: "https://store.steampowered.com/api/storesearch/") else { results = []; return }
        c.queryItems = [URLQueryItem(name: "term", value: q), URLQueryItem(name: "l", value: idioma() == "es" ? "spanish" : "english"),
                        URLQueryItem(name: "cc", value: Locale.current.region?.identifier ?? "US")]
        guard let url = c.url else { return }
        searching = true; failed = false
        struct R: Decodable { let items: [StoreItem] }
        URLSession.shared.dataTask(with: url) { data, _, _ in
            let r = data.flatMap { try? JSONDecoder().decode(R.self, from: $0) }
            DispatchQueue.main.async {
                guard q == query.trimmingCharacters(in: .whitespaces) else { return }   // (llegó una búsqueda vieja)
                searching = false
                if let r { results = r.items } else { failed = true }
            }
        }.resume()
    }
}

// MARK: - Ajustes: las dos formas de usar Steam
struct LibraryModePicker: View {
    @EnvironmentObject var m: AppModel
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            option(true, L("Biblioteca de VRmin"), L("MÁS LIVIANA"),
                   L("VRmin muestra tus juegos, la tienda y las descargas. Steam queda abierto de fondo, sin su ventana, solo para tu sesión y las descargas."))
            option(false, L("Ventana de Steam"), nil,
                   L("Como siempre: la ventana de Steam para Windows, con todo lo de Steam (amigos, chat, la tienda completa)."))
            Text(L("Podés cambiarlo cuando quieras; tus juegos y partidas no se tocan. Vale la próxima vez que se abre Steam."))
                .font(.caption).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
            if let a = m.steamAccount {
                HStack {
                    Image(systemName: "person.crop.circle").foregroundStyle(Theme.text2)
                    Text(L("Cuenta de Steam: %@", a)).font(.callout)
                    Spacer()
                    if m.steamRunning {
                        if m.steamShown {
                            Button(L("Ocultar Steam")) { m.hideSteam() }.buttonStyle(Pill()).help(L("Esconde la ventana de Steam; Steam sigue abierto de fondo"))
                        } else {
                            Button(L("Mostrar Steam")) { m.showSteam() }.buttonStyle(Pill())
                        }
                    }
                    Button(L("Cerrar sesión")) { askSignOut = true }.buttonStyle(Pill()).disabled(m.busyTask != nil)
                }
                .padding(.top, 2)
                .alert(L("¿Cerrar la sesión de Steam?"), isPresented: $askSignOut) {
                    Button(L("Cerrar sesión"), role: .destructive) { m.signOutSteam() }
                    Button(L("Cancelar"), role: .cancel) {}
                } message: {
                    Text(L("Se cierra Steam (y el juego que esté abierto). La próxima vez que se abra, pide iniciar sesión. Tus juegos y partidas no se tocan."))
                }
            }
        }
    }
    @State private var askSignOut = false
    private func option(_ vrmin: Bool, _ title: String, _ badge: String?, _ text: String) -> some View {
        let on = m.libraryVRmin == vrmin
        return Button { m.libraryVRmin = vrmin } label: {
            HStack(alignment: .top, spacing: 12) {
                ZStack {
                    Circle().strokeBorder(on ? Theme.amber : Color.white.opacity(0.35), lineWidth: 2).frame(width: 18, height: 18)
                    if on { Circle().fill(Theme.amber).frame(width: 8, height: 8) }
                }
                .padding(.top, 1)
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 8) {
                        Text(title).font(.system(size: 14, weight: .semibold))
                        if let badge { Text(badge).font(.system(size: 10.5, weight: .semibold)).foregroundStyle(Theme.amberText) }
                    }
                    Text(text).font(.callout).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
            }
            .padding(14)
            .background(RoundedRectangle(cornerRadius: 10).fill(on ? Theme.amber.opacity(0.08) : Theme.card))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(on ? Theme.amber : Theme.line, lineWidth: 1.5))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .animation(.easeInOut(duration: 0.2), value: on)
    }
}
