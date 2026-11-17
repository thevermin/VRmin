// VRmin Host · Biblioteca y Tienda: los juegos de Steam con el estilo de VRmin y el toque de Steam (portadas, página de
// cada juego con su imagen grande, barra de descargas). Steam sigue abierto de fondo para la sesión y las descargas;
// con «Biblioteca de VRmin» (Ajustes) se abre sin su ventana. Las imágenes son las de la tienda de Steam.

import SwiftUI
import AppKit

// MARK: - Descargas (de los appmanifest de Steam)
struct SteamDownload: Identifiable, Equatable {
    let id: String; let name: String; let done: Int64; let total: Int64; let paused: Bool
}

/// Lo que Steam está bajando o actualizando: los appmanifest con bytes por bajar (StateFlags 512 = en pausa)
func scanDownloads(_ steamapps: URL) -> [SteamDownload] {
    let fm = FileManager.default
    guard let files = try? fm.contentsOfDirectory(atPath: steamapps.path) else { return [] }
    var out: [SteamDownload] = []
    for f in files where f.hasPrefix("appmanifest_") && f.hasSuffix(".acf") {
        let d = parseACF(steamapps.appendingPathComponent(f))
        guard let id = d["appid"], let total = Int64(d["BytesToDownload"] ?? ""), total > 0 else { continue }
        let done = Int64(d["BytesDownloaded"] ?? "") ?? 0
        let flags = Int(d["StateFlags"] ?? "") ?? 0
        if done >= total && flags & 4 != 0 { continue }   // (ya terminó)
        out.append(SteamDownload(id: id, name: d["name"] ?? id, done: done, total: total, paused: flags & 512 != 0 && flags & 256 == 0))
    }
    return out.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
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
struct GameCapsule: View {
    let game: SteamGame
    let vr: Bool?
    let open: Bool
    let play: () -> Void
    @State private var hover = false
    var body: some View {
        ZStack(alignment: .topTrailing) {
            AsyncImage(url: steamArt(game.id, "library_600x900.jpg")) { phase in
                if let img = phase.image {
                    img.resizable().aspectRatio(contentMode: .fill)
                } else if phase.error != nil {
                    ZStack(alignment: .bottomLeading) {
                        AsyncImage(url: steamArt(game.id, "header.jpg")) { i in i.resizable().aspectRatio(contentMode: .fill) } placeholder: { Color.white.opacity(0.06) }
                            .blur(radius: 18).overlay(Color.black.opacity(0.25))
                        Text(game.name).font(.system(size: 15, weight: .heavy)).lineLimit(3).padding(10)
                            .shadow(color: .black.opacity(0.6), radius: 6)
                    }
                } else {
                    ZStack { Color.white.opacity(0.06); ProgressView().controlSize(.small) }
                }
            }
            .frame(minWidth: 0, maxWidth: .infinity, minHeight: 0, maxHeight: .infinity).clipped()
            Text(vr == false ? L("Plano") : "VR").font(.system(size: 10, weight: .bold))
                .padding(.horizontal, 6).padding(.vertical, 2)
                .background(RoundedRectangle(cornerRadius: 5).fill(Color.black.opacity(0.55))).padding(7)
            Button(action: play) {
                Image(systemName: "play.fill").font(.system(size: 17, weight: .bold)).foregroundStyle(Theme.amberDark)
                    .frame(width: 46, height: 46).background(Circle().fill(Theme.amber))
            }
            .buttonStyle(.plain).opacity(hover && !open ? 1 : 0)
            .frame(maxWidth: .infinity, maxHeight: .infinity).offset(y: -12)
            .help(L("Jugar en el Quest"))
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

// MARK: - Biblioteca
struct LibraryView: View {
    @EnvironmentObject var m: AppModel
    @Binding var gameAdv: SteamGame?
    @State private var query = ""
    @State private var selected: SteamGame? = nil

    private var games: [SteamGame] {
        m.games.filter { $0.inLibrary && (query.isEmpty || $0.name.localizedCaseInsensitiveContains(query)) }
    }
    private func isOpen(_ g: SteamGame) -> Bool { m.openGames.contains(g.installdir.lowercased()) }

    var body: some View {
        HStack(spacing: 0) {
            sidebar
            Rectangle().fill(Theme.line).frame(width: 1)
            ZStack {
                grid
                if let g = selected {
                    GameDetail(game: g, gameAdv: $gameAdv) { withAnimation(.easeInOut(duration: 0.25)) { selected = nil } }
                        .transition(.opacity.combined(with: .offset(y: 24)))
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .safeAreaInset(edge: .bottom, spacing: 0) { DownloadsBar() }
        }
    }

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass").foregroundStyle(Theme.text2)
                TextField(L("Buscar en tu biblioteca"), text: $query).textFieldStyle(.plain)
            }
            .padding(.horizontal, 9).padding(.vertical, 6)
            .background(RoundedRectangle(cornerRadius: 7).fill(Color.white.opacity(0.07)))
            Text(L("JUEGOS · %d", games.count)).font(.system(size: 11, weight: .semibold)).foregroundStyle(Theme.text2)
                .padding(.top, 8).padding(.leading, 4)
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(games) { g in
                        Button { withAnimation(.easeInOut(duration: 0.25)) { selected = g } } label: {
                            HStack(spacing: 8) {
                                AsyncImage(url: steamArt(g.id, "capsule_sm_120.jpg")) { i in i.resizable().aspectRatio(contentMode: .fill) } placeholder: { Color.white.opacity(0.08) }
                                    .frame(width: 18, height: 18).clipShape(RoundedRectangle(cornerRadius: 3))
                                Text(g.name).lineLimit(1)
                                Spacer(minLength: 0)
                                if isOpen(g) { Circle().fill(Theme.amber).frame(width: 6, height: 6) }
                            }
                            .font(.system(size: 12.5))
                            .foregroundStyle(selected?.id == g.id ? Theme.text : Theme.text2)
                            .padding(.horizontal, 6).padding(.vertical, 5)
                            .background(RoundedRectangle(cornerRadius: 6).fill(selected?.id == g.id ? STEAM_BLUE.opacity(0.16) : .clear))
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        }
        .padding(10)
        .frame(width: 210)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(LinearGradient(colors: [Color(red: 42 / 255, green: 71 / 255, blue: 94 / 255).opacity(0.25), .clear], startPoint: .top, endPoint: .bottom))
    }

    private var grid: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 4) {
                Text(L("Biblioteca")).font(.system(size: 18, weight: .bold))
                Text(m.steamRunning ? L("Tus juegos de Steam, listos para el Quest") : L("Tus juegos de Steam. Para jugar, Steam tiene que estar abierto."))
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
                if games.isEmpty {
                    Text(query.isEmpty ? L("Todavía no hay juegos. Buscalos en la Tienda.") : L("Ningún juego se llama así."))
                        .font(.callout).foregroundStyle(Theme.text2).padding(.top, 20)
                }
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 130, maximum: 190), spacing: 16)], spacing: 16) {
                    ForEach(games) { g in
                        GameCapsule(game: g, vr: m.vr[g.id]?.vr, open: isOpen(g)) { m.play(g) }
                            .onTapGesture { withAnimation(.easeInOut(duration: 0.25)) { selected = g } }
                    }
                }
                .padding(.vertical, 8)
            }
            .padding(18)
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

    private var open: Bool { m.openGames.contains(game.installdir.lowercased()) }
    private static let runtimes: [(id: String, name: String)] = [
        ("auto", "Automático"), ("openxr", "OpenXR"), ("openvr", "OpenComposite"), ("steamvr", "SteamVR real"), ("plano", "Sin VR"),
    ]

    var body: some View {
        let v = m.vr[game.id]
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                ZStack(alignment: .bottomLeading) {
                    AsyncImage(url: steamArt(game.id, "library_hero.jpg")) { phase in
                        if let i = phase.image { i.resizable().aspectRatio(contentMode: .fill) }
                        else if phase.error != nil {
                            AsyncImage(url: steamArt(game.id, "header.jpg")) { i in i.resizable().aspectRatio(contentMode: .fill) } placeholder: { Color.white.opacity(0.05) }
                                .blur(radius: 12)
                        } else { Color.white.opacity(0.05) }
                    }
                    .frame(height: 240).frame(maxWidth: .infinity).clipped()
                    LinearGradient(colors: [.clear, Theme.bg], startPoint: .center, endPoint: .bottom)
                    AsyncImage(url: steamArt(game.id, "logo.png")) { phase in
                        if let i = phase.image { i.resizable().aspectRatio(contentMode: .fit).frame(maxWidth: 320, maxHeight: 96, alignment: .bottomLeading) }
                        else { Text(game.name).font(.system(size: 32, weight: .heavy)).shadow(color: .black.opacity(0.6), radius: 10) }
                    }
                    .padding(.leading, 26).padding(.bottom, 20)
                    Button(action: back) { Label(L("Biblioteca"), systemImage: "chevron.left").font(.callout) }
                        .buttonStyle(.plain).padding(.horizontal, 10).padding(.vertical, 5)
                        .background(RoundedRectangle(cornerRadius: 7).fill(Color.black.opacity(0.45)))
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading).padding(14)
                }
                .frame(height: 240)

                HStack(spacing: 18) {
                    if open {
                        Button { m.forceClose(game) } label: { Label(L("FORZAR CIERRE"), systemImage: "xmark") }
                            .buttonStyle(BigButton()).help(L("Cierra el juego aunque no responda (Steam sigue abierto)"))
                    } else if m.steamRunning {
                        Button { m.play(game) } label: { Label(L("JUGAR EN EL QUEST"), systemImage: "play.fill") }.buttonStyle(BigButton())
                    } else {
                        Button { m.run(L("Abriendo Steam"), "abrir_steam.sh") } label: { Label(L("ABRIR STEAM"), systemImage: "arrow.right") }
                            .buttonStyle(BigButton()).disabled(m.busyTask != nil)
                    }
                    stat(open ? L("Abierto") : estado(v), L("Estado"))
                    stat(v.map { L($0.runtimeNombre) } ?? "…", L("Sistema de VR"))
                    if size > 0 { stat(bytesText(size), L("Tamaño")) }
                    Spacer()
                }
                .padding(.horizontal, 26).padding(.top, 4)

                HStack(alignment: .top, spacing: 14) {
                    VStack(alignment: .leading, spacing: 8) {
                        Text(L("Sistema de VR")).font(.system(size: 13, weight: .semibold))
                        Picker(L("Sistema de VR"), selection: Binding(get: { v?.perfil.pedido ?? "auto" }, set: { m.setVR(game.id, "runtime", $0) })) {
                            ForEach(Self.runtimes, id: \.id) { o in Text(L(o.name)).tag(o.id) }
                        }
                        .pickerStyle(.segmented).labelsHidden().disabled(v == nil)
                        Text(L("Si un juego anda mal con OpenComposite, probá SteamVR real, que suele ser lo más estable; si SteamVR real va lento o no abre, probá OpenComposite."))
                            .font(.caption).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
                    }
                    .padding(14).frame(maxWidth: .infinity, alignment: .topLeading)
                    .background(RoundedRectangle(cornerRadius: 10).fill(Theme.card))
                    .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.line))
                    VStack(alignment: .leading, spacing: 8) {
                        Text(L("Calidad")).font(.system(size: 13, weight: .semibold))
                        Text(m.adv[game.installdir] != nil ? L("Este juego tiene ajustes propios.") : L("Usa los ajustes de «Todos los juegos»."))
                            .font(.caption).foregroundStyle(Theme.text2)
                        Button(L("Calidad y rendimiento…")) { m.loadAdv(); gameAdv = game }.buttonStyle(Pill())
                    }
                    .padding(14).frame(width: 240, alignment: .topLeading)
                    .background(RoundedRectangle(cornerRadius: 10).fill(Theme.card))
                    .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.line))
                }
                .padding(.horizontal, 26).padding(.vertical, 18)
            }
        }
        .background(Theme.bg)
        .onAppear {
            let acf = m.steamapps.appendingPathComponent("appmanifest_\(game.id).acf")
            size = Int64(parseACF(acf)["SizeOnDisk"] ?? "") ?? 0
        }
    }

    private func estado(_ v: VRJuego?) -> String {
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
    }
}

// MARK: - Barra de descargas (la de Steam: celeste, abajo)
struct DownloadsBar: View {
    @EnvironmentObject var m: AppModel
    var body: some View {
        if let d = m.downloads.first {
            let k = d.total > 0 ? Double(d.done) / Double(d.total) : 0
            HStack(spacing: 12) {
                Text(d.paused ? L("En pausa") : L("Descargando")).font(.system(size: 13, weight: .bold))
                Text(d.name).font(.callout).lineLimit(1)
                if m.downloads.count > 1 { Text(L("y %d más", m.downloads.count - 1)).font(.caption).foregroundStyle(Theme.text2) }
                GeometryReader { geo in
                    ZStack(alignment: .leading) {
                        Capsule().fill(Color.white.opacity(0.1))
                        Capsule().fill(LinearGradient(colors: [Color(red: 71 / 255, green: 191 / 255, blue: 1), Color(red: 26 / 255, green: 68 / 255, blue: 194 / 255)], startPoint: .leading, endPoint: .trailing))
                            .frame(width: geo.size.width * k)
                            .animation(.easeOut(duration: 0.6), value: k)
                    }
                }
                .frame(height: 6)
                Text("\(Int(k * 100)) % · \(bytesText(d.done)) / \(bytesText(d.total))").font(.caption.monospacedDigit()).foregroundStyle(Theme.text2)
            }
            .padding(.horizontal, 16).frame(height: 46)
            .background(LinearGradient(colors: [Color(red: 42 / 255, green: 71 / 255, blue: 94 / 255).opacity(0.85), Color(red: 27 / 255, green: 40 / 255, blue: 56 / 255).opacity(0.85)], startPoint: .leading, endPoint: .trailing))
            .overlay(Rectangle().fill(Theme.line).frame(height: 1), alignment: .top)
            .transition(.move(edge: .bottom))
        }
    }
}

// MARK: - Tienda (la búsqueda de la tienda de Steam; instalar se lo pide al Steam de VRmin)
struct StoreItem: Identifiable, Decodable {
    struct Price: Decodable { let currency: String?; let final: Int? }
    let id: Int; let name: String; let tiny_image: String?; let price: Price?
}

struct StoreView: View {
    @EnvironmentObject var m: AppModel
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
                VStack(spacing: 10) {
                    ForEach(results) { r in row(r) }
                }
            }
        }
        .padding(18)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .safeAreaInset(edge: .bottom, spacing: 0) { DownloadsBar() }
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
                    .help(L("Steam muestra su ventana de instalación: si el juego no es tuyo, te ofrece comprarlo"))
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
        }
    }
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
