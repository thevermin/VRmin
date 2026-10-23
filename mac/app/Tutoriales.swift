// VRmin Host · tutoriales: guías cortas con un ícono por paso. La bienvenida aparece sola la primera vez (y el del
// Modo Centinela al tocar «Empezar»), con «No mostrar este otra vez» y «No mostrar más tutoriales»; todos están en
// la Ayuda (botón «?» de arriba). Los textos van en los dos idiomas acá mismo.
import SwiftUI

/// Texto en el idioma elegido (español o inglés)
private func T(_ es: String, _ en: String) -> String { idioma() == "es" ? es : en }

struct HostTut: Identifiable {
    let id: String
    let icon: String
    let es: String, en: String
    let steps: [(String, String, String)]   // (ícono, español, inglés)
    var title: String { T(es, en) }
}

let HOST_TUTS: [HostTut] = [
    HostTut(id: "bienvenida", icon: "hand.wave", es: "Bienvenido a VRmin Host", en: "Welcome to VRmin Host", steps: [
        ("macbook", "VRmin Host es la parte de la Mac: le pasa al Quest tu pantalla, tus ventanas y los juegos VR de Steam.", "VRmin Host is the Mac side: it sends your screen, your windows and Steam VR games to the Quest."),
        ("gearshape.2", "Para los juegos, instalá una vez el «Motor de juegos de Windows» y «Steam para Windows» (tarjeta «Para los juegos VR»).", "For games, install the “Windows game engine” and “Steam for Windows” once (the “For VR games” card)."),
        ("visionpro", "En el Quest, abrí VRmin y elegí esta Mac. No hace falta encender nada acá: se enciende sola.", "On the Quest, open VRmin and pick this Mac. Nothing to turn on here: it starts by itself."),
        ("lock.shield", "La primera vez aparece acá un código de 6 números: escribilo en el Quest.", "The first time, a 6-digit code shows up here: type it on the Quest."),
        ("questionmark.circle", "Todas las guías están en el botón «?» de arriba.", "Every guide is under the “?” button at the top."),
    ]),
    HostTut(id: "juegos", icon: "gamecontroller", es: "Jugar un juego VR", en: "Playing a VR game", steps: [
        ("square.grid.2x2", "Los juegos VR que instales en Steam aparecen en «Juegos». Tocá «Jugar» (o elegilo desde el Quest).", "VR games you install in Steam show up in “Games”. Click “Play” (or pick it from the Quest)."),
        ("hourglass", "La primera vez un juego puede tardar en abrir: la Mac lo está preparando.", "The first launch can take a while: the Mac is getting the game ready."),
        ("xmark.circle", "Si un juego se traba, «Forzar cierre» sobre su imagen lo cierra.", "If a game hangs, “Force quit” on its picture closes it."),
        ("arrow.clockwise", "Si Steam se traba, aparece «Reiniciar Steam».", "If Steam hangs, “Restart Steam” appears."),
    ]),
    HostTut(id: "conexion", icon: "cable.connector", es: "Wi-Fi o cable", en: "Wi-Fi or cable", steps: [
        ("wifi", "Por Wi-Fi: el Quest y la Mac en la misma red (mejor si la Mac va por cable al router).", "Over Wi-Fi: the Quest and the Mac on the same network (best with the Mac wired to the router)."),
        ("cable.connector", "Por cable USB-C: más estable y con menos demora. VRmin lo usa solo cuando está conectado.", "Over USB-C cable: steadier, with less delay. VRmin uses it on its own when it's plugged in."),
        ("switch.2", "«Solo por cable USB-C» obliga a usar el cable (sin cable no se conecta).", "“USB-C cable only” forces the cable (no cable, no connection)."),
    ]),
    HostTut(id: "centinela", icon: "eye", es: "El Modo Centinela", en: "Sentinel Mode", steps: [
        ("eye", "Jugá juegos VR sin ponerte el visor: queda apoyado mirándote, sigue tus controles y el juego se ve en una ventana de la Mac.", "Play VR games without wearing the headset: it sits facing you, tracks your controllers, and the game shows in a Mac window."),
        ("hammer", "Una sola vez: activá el modo desarrollador del Quest (el «?» de la fila del Modo Centinela explica cómo) y tocá «Instalar».", "Once: turn on the Quest's developer mode (the “?” on the Sentinel Mode row explains how) and click “Install”."),
        ("cable.connector", "Conectá el visor por cable USB-C. Así su pantalla no se apaga aunque no lo tengas puesto.", "Connect the headset by USB-C cable. That keeps its display on even when you're not wearing it."),
        ("display", "Ponelo alto y mirándote: encima del monitor es ideal para jugar sentado.", "Put it up high, facing you: on top of the monitor is ideal for playing seated."),
        ("camera", "En la ventana del Modo Centinela podés prender la cámara de la Mac para que el juego siga tu cabeza.", "In the Sentinel Mode window you can turn on the Mac's camera so the game follows your head."),
        ("escape", "Para salir: cerrá esa ventana o apretá Esc.", "To exit: close that window or press Esc."),
    ]),
    HostTut(id: "grabar", icon: "record.circle", es: "Grabar un video", en: "Recording a video", steps: [
        ("record.circle", "«Grabar» en la tarjeta «Grabación» (o en los ajustes rápidos del visor). Otro toque la termina.", "“Record” on the “Recording” card (or in the headset's quick settings). Click again to stop."),
        ("cable.connector", "Con el visor por cable (y las herramientas del Modo Centinela) también se graba lo que ves en el visor.", "With the headset on the cable (and the Sentinel Mode tools) what you see in the headset is recorded too."),
        ("folder", "Los videos quedan en Películas › VRmin. Van sin sonido.", "Videos go to Movies › VRmin. They have no sound."),
    ]),
    HostTut(id: "problemas", icon: "stethoscope", es: "Si algo no anda", en: "If something isn't working", steps: [
        ("list.bullet.rectangle", "El registro (abajo de todo) cuenta qué está pasando: copialo si pedís ayuda.", "The log (at the very bottom) shows what's going on: copy it if you ask for help."),
        ("record.circle", "Si el escritorio no se ve en el Quest: Ajustes del Sistema › Privacidad y seguridad › Grabación de pantalla › VRmin Host.", "If the desktop doesn't show on the Quest: System Settings › Privacy & Security › Screen Recording › VRmin Host."),
        ("speaker.wave.2", "Sin sonido en el visor: revisá el sonido en los ajustes (el botón de los controles, arriba).", "No sound in the headset: check audio in settings (the sliders button at the top)."),
        ("power", "Si nada responde: «Apagar» y «Encender ahora» en «Conexión».", "If nothing responds: “Turn off” and “Turn on now” under “Connection”."),
    ]),
]

enum Tutoriales {
    static func enabled(_ id: String) -> Bool {
        let d = UserDefaults.standard
        return !d.bool(forKey: "tutOff") && !d.bool(forKey: "tutNo_" + id)
    }
    static func find(_ id: String) -> HostTut? { HOST_TUTS.first { $0.id == id } }
}

/// Lo que se muestra en la hoja de tutoriales: uno solo (que apareció solo, o con «Empezar») o la Ayuda
struct TutPresent: Identifiable {
    let id = UUID()
    var tut: HostTut?                 // nil: la Ayuda
    var auto = false
    var start: (String, () -> Void)? = nil
}

struct TutorialSheet: View {
    @State var p: TutPresent
    @State private var open: HostTut? = nil   // en la Ayuda: el que se está viendo
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Group {
            if let t = p.tut { tutView(t, auto: p.auto, start: p.start, back: nil) }
            else if let t = open { tutView(t, auto: false, start: nil, back: { open = nil }) }
            else { helpList }
        }
        .padding(24).frame(width: 560)
        .background(Theme.bg)
        .foregroundStyle(Theme.text)
        .tint(Theme.amber)
    }

    private func titleRow(_ icon: String, _ title: String, _ sub: String?) -> some View {
        HStack(spacing: 14) {
            Image(systemName: icon).font(.system(size: 22, weight: .semibold)).foregroundStyle(Theme.amberDark)
                .frame(width: 46, height: 46)
                .background(RoundedRectangle(cornerRadius: 11, style: .continuous).fill(Theme.amber))
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.system(size: 21, weight: .bold))
                if let sub { Text(sub).font(.callout).foregroundStyle(Theme.text2) }
            }
        }
    }

    private func tutView(_ t: HostTut, auto: Bool, start: (String, () -> Void)?, back: (() -> Void)?) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            titleRow(t.icon, t.title, nil)
            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    ForEach(Array(t.steps.enumerated()), id: \.offset) { _, s in
                        HStack(alignment: .center, spacing: 12) {
                            Image(systemName: s.0).font(.system(size: 16)).foregroundStyle(Theme.text)
                                .frame(width: 36, height: 36)
                                .background(RoundedRectangle(cornerRadius: 9, style: .continuous).fill(Color.white.opacity(0.10)))
                            Text(T(s.1, s.2)).font(.system(size: 14)).fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
            }
            .frame(maxHeight: 360)
            HStack(alignment: .bottom) {
                if auto {
                    VStack(alignment: .leading, spacing: 4) {
                        Button(T("No mostrar este otra vez", "Don't show this one again")) {
                            UserDefaults.standard.set(true, forKey: "tutNo_" + t.id); dismiss()
                        }.buttonStyle(.plain).font(.caption).foregroundStyle(Theme.text2)
                        Button(T("No mostrar más tutoriales", "Don't show any more tutorials")) {
                            UserDefaults.standard.set(true, forKey: "tutOff"); dismiss()
                        }.buttonStyle(.plain).font(.caption).foregroundStyle(Theme.text2)
                    }
                }
                Spacer()
                if let back {
                    Button { back() } label: { Label(T("Volver a la Ayuda", "Back to Help"), systemImage: "chevron.left") }.buttonStyle(Pill(primary: true))
                } else if let start {
                    Button(T("Ahora no", "Not now")) { dismiss() }.buttonStyle(Pill())
                    Button(start.0) { dismiss(); start.1() }.buttonStyle(Pill(primary: true))
                } else {
                    Button(T("Entendido", "Got it")) { dismiss() }.buttonStyle(Pill(primary: true))
                }
            }
        }
    }

    @State private var off = UserDefaults.standard.bool(forKey: "tutOff")
    private var helpList: some View {
        VStack(alignment: .leading, spacing: 14) {
            titleRow("questionmark.circle", T("Ayuda", "Help"), T("Guías cortas de VRmin Host", "Short guides for VRmin Host"))
            ScrollView {
                VStack(spacing: 6) {
                    ForEach(HOST_TUTS) { t in
                        Button { open = t } label: {
                            HStack(spacing: 12) {
                                Image(systemName: t.icon).frame(width: 30)
                                Text(t.title).font(.system(size: 14, weight: .semibold))
                                Spacer()
                                Image(systemName: "chevron.right").foregroundStyle(Theme.text2)
                            }
                            .padding(.horizontal, 12).padding(.vertical, 10)
                            .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Color.white.opacity(0.06)))
                            .contentShape(Rectangle())
                        }.buttonStyle(.plain)
                    }
                }
            }
            .frame(maxHeight: 380)
            Toggle(T("Mostrar tutoriales solos (la primera vez)", "Show tutorials on their own (the first time)"), isOn: Binding(
                get: { !off },
                set: { on in
                    off = !on
                    UserDefaults.standard.set(!on, forKey: "tutOff")
                    if on { for t in HOST_TUTS { UserDefaults.standard.removeObject(forKey: "tutNo_" + t.id) } }   // vuelven todos
                })).toggleStyle(.switch).font(.callout)
            HStack { Spacer(); Button(T("Cerrar", "Close")) { dismiss() }.buttonStyle(Pill(primary: true)) }
        }
    }
}
