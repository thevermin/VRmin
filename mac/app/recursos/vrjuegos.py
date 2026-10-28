#!/usr/bin/env python3
"""
VRmin Host · juegos VR: detección, perfiles y elección del runtime de cada juego.

Cada juego de PC VR habla con el visor a través de una "API" de VR. Las que existen:
  - OpenXR   : la estándar. Va directo a nuestro runtime (C:\\questvrbridge\\questvrbridge_openxr.dll).
  - OpenVR   : la de SteamVR. La traduce OpenComposite a OpenXR (sin SteamVR real y sin tocar el juego).
  - SteamVR  : el SteamVR real de Valve con un driver nuestro (experimental: el driver todavía no existe).
  - Oculus   : el SDK de Oculus (LibOVR). Haría falta ReVive (todavía no está incluido).
  - Sin VR   : abrir el juego en una pantalla plana (útil para probar o para juegos con modo plano).

Este script decide qué usar en cada juego combinando, en este orden:
  1. lo que elegiste vos para ese juego      ($DATOS/perfiles/<appid>.json)
  2. lo conocido de ese juego                (perfiles.json, viene con la app)
  3. lo que se detecta mirando sus archivos  ($DATOS/perfiles/detectado/<appid>.json, en caché)

Uso:
  vrjuegos.py detectar APPID           detecta y muestra (JSON)
  vrjuegos.py resolver APPID           variables para bash (las usa jugar.sh)
  vrjuegos.py poner APPID CLAVE VALOR  guarda una elección (runtime, lanzar, estado, nota, args, exe, diagnostico)
  vrjuegos.py listar                   todos los juegos con su detección y perfil (JSON, lo usa la app)
  vrjuegos.py intento APPID RUNTIME    se abrió con ese runtime (todavía sin imagen)
  vrjuegos.py exito APPID              el juego mandó imagen: ese runtime anda

Funciona con el python3 que trae macOS (3.9): sin sintaxis nueva.
"""
import json
import os
import re
import shlex
import sys
import time
import urllib.request

RES = os.path.dirname(os.path.abspath(__file__))
DATOS = os.environ.get("QVB_DATA") or os.path.expanduser("~/Library/Application Support/QuestVRBridge")
PREFIJO = os.path.join(DATOS, "prefijo")
C = os.path.join(PREFIJO, "drive_c")
STEAMAPPS = os.path.join(C, "Program Files (x86)", "Steam", "steamapps")
PERFILES = os.path.join(DATOS, "perfiles")
DETECTADO = os.path.join(PERFILES, "detectado")

RUNTIMES = ["auto", "openxr", "openvr", "steamvr", "oculus", "plano"]
NOMBRES = {
    "auto": "Automático",
    "openxr": "OpenXR",
    "openvr": "OpenVR (OpenComposite)",
    "steamvr": "SteamVR real",
    "oculus": "Oculus (ReVive)",
    "plano": "Sin VR",
}
# Idioma de los mensajes: inglés (por defecto) o español; lo pasa VRmin Host en VRMIN_LANG
IDIOMA = "es" if os.environ.get("VRMIN_LANG") == "es" else "en"
EN = {
    "Automático": "Automatic",
    "OpenVR (OpenComposite)": "OpenVR (OpenComposite)",
    "SteamVR real": "Real SteamVR",
    "Sin VR": "No VR",
    "Puede usar %s y nuestro runtime OpenXR solo tiene DirectX 11: si no arranca, probá OpenVR o el argumento %s":
        "It may use %s and our OpenXR runtime only supports DirectX 11: if it doesn't start, try OpenVR or the %s argument",
    "Solo trae el SDK de Oculus: necesita ReVive (todavía no está incluido)":
        "It only ships the Oculus SDK: it needs ReVive (not included yet)",
    "OVRPlugin de Unity a veces también habla OpenXR: vale la pena probar «OpenXR»":
        "Unity's OVRPlugin sometimes speaks OpenXR too: worth trying “OpenXR”",
    "No encontré archivos de VR conocidos: puede estar dentro del motor. Se prueba con OpenVR primero":
        "No known VR files found: it may be built into the engine. Trying OpenVR first",
    "Oculus (ReVive) todavía no está incluido: se usa OpenVR": "Oculus (ReVive) isn't included yet: using OpenVR",
    "el driver de VRmin para SteamVR": "the VRmin driver for SteamVR",
    "SteamVR (instalalo desde Steam) y el driver de VRmin": "SteamVR (install it from Steam) and the VRmin driver",
    "SteamVR real: falta %s. Se usa OpenVR": "Real SteamVR: missing %s. Using OpenVR",
    "Falta OpenComposite: reinstalá el motor desde la app": "OpenComposite is missing: reinstall the engine from the app",
    "El juego %s no está instalado en el Steam de VRmin\n": "Game %s isn't installed in VRmin's Steam\n",
    "APIs: %s · motor: %s%s": "APIs: %s · engine: %s%s",
    "ninguna conocida": "none known",
    " · gráficos: ": " · graphics: ",
    "otro": "other",
    "Controles de Touch para OpenComposite: el grip agarra (%d botones arreglados)":
        "Touch controls for OpenComposite: the grip grabs (%d buttons fixed)",
    "La vez anterior no arrancó con %s: ahora pruebo %s": "Last time it didn't start with %s: now trying %s",
    "Gráficos: se pide DirectX 11 (%s), lo único que entiende nuestro runtime": "Graphics: asking for DirectX 11 (%s), the only one our runtime understands",
    "Se abre directo (sin el lanzador hlvr.bat) con Vulkan y algunos efectos bajados para que ande fluido":
        "Opens directly (without the hlvr.bat launcher) with Vulkan and a few effects lowered so it runs smoothly",
}


def T(es):
    """Texto en el idioma elegido (el código está en español)."""
    return es if IDIOMA == "es" else EN.get(es, es)


ESTADOS = ["sin probar", "funciona", "con problemas", "no funciona"]
NO_JUEGOS = {"228980", "250820", "1070560", "1391110", "1628350"}   # redistribuibles, SteamVR, Proton…

# Archivos que delatan qué usa un juego (en minúsculas)
SENALES = {
    "openxr": {"openxr_loader.dll", "unityopenxr.dll"},
    "openvr": {"openvr_api.dll", "xrsdkopenvr.dll"},
    "oculus": {"ovrplugin.dll", "libovrrt64_1.dll", "libovrrt32_1.dll", "oculusxrplugin.dll"},
}
EXE_DESCARTAR = re.compile(r"(crash|unins|setup|redist|vc_?redist|dxsetup|prereq|launcher_helper|easyanticheat|be?service|cefprocess|ue4prereq|ue5prereq)", re.I)


# ---------------------------------------------------------------- utilidades
def leer_json(ruta, defecto):
    try:
        with open(ruta, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return defecto


def guardar_json(ruta, datos):
    os.makedirs(os.path.dirname(ruta), exist_ok=True)
    tmp = ruta + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(datos, f, ensure_ascii=False, indent=2)
    os.replace(tmp, ruta)


def leer_acf(ruta):
    """appmanifest_*.acf de Steam: devuelve las claves de primer nivel del juego."""
    d = {}
    try:
        with open(ruta, encoding="utf-8", errors="replace") as f:
            for linea in f:
                m = re.match(r'^\s*"(\w+)"\s+"([^"]*)"', linea)
                if m and m.group(1) not in d:
                    d[m.group(1)] = m.group(2)
    except Exception:
        pass
    return d


def juegos_instalados():
    out = {}
    try:
        archivos = os.listdir(STEAMAPPS)
    except Exception:
        return out
    for f in archivos:
        if not (f.startswith("appmanifest_") and f.endswith(".acf")):
            continue
        d = leer_acf(os.path.join(STEAMAPPS, f))
        appid, carpeta = d.get("appid"), d.get("installdir")
        if not appid or not carpeta or appid in NO_JUEGOS:
            continue
        ruta = os.path.join(STEAMAPPS, "common", carpeta)
        if os.path.isdir(ruta):
            out[appid] = {"id": appid, "nombre": d.get("name", carpeta), "carpeta": ruta,
                          "actualizado": d.get("LastUpdated", d.get("buildid", ""))}
    return out


def a_windows(ruta_unix):
    """/…/prefijo/drive_c/Program Files/x → C:\\Program Files\\x"""
    rel = os.path.relpath(ruta_unix, C)
    return "C:\\" + rel.replace("/", "\\")


# ---------------------------------------------------------------- detección
def detectar(juego):
    raiz = juego["carpeta"]
    nombres, carpetas, exes = set(), set(), []
    vistos = 0
    base = raiz.rstrip("/").count("/")
    for dirpath, dirnames, filenames in os.walk(raiz):
        prof = dirpath.count("/") - base
        if prof >= 6:
            dirnames[:] = []
        for d in dirnames:
            carpetas.add(d.lower())
        for f in filenames:
            vistos += 1
            fl = f.lower()
            nombres.add(fl)
            if fl.endswith(".exe") and prof <= 4 and not EXE_DESCARTAR.search(fl):
                ruta = os.path.join(dirpath, f)
                try:
                    tam = os.path.getsize(ruta)
                except OSError:
                    tam = 0
                exes.append((ruta, tam, prof))
        if vistos > 60000:   # juegos enormes: alcanza con lo visto
            break

    apis = [api for api, senal in SENALES.items() if nombres & senal]
    motor = "otro"
    if "unityplayer.dll" in nombres:
        motor = "unity"
    elif any(n.endswith("-win64-shipping.exe") for n in nombres) or "engine" in carpetas and "binaries" in carpetas:
        motor = "unreal"
    graficos = []
    if "vulkan-1.dll" in nombres:
        graficos.append("vulkan")
    if "d3d12core.dll" in nombres or "d3d12" in carpetas:
        graficos.append("d3d12")

    # Ejecutable principal: el «-Shipping» de Unreal; si no, el más grande cerca de la raíz
    exe = ""
    if exes:
        ship = [e for e in exes if e[0].lower().endswith("-win64-shipping.exe")]
        elegido = ship[0] if ship else sorted(exes, key=lambda e: (e[2] > 1, -e[1]))[0]
        exe = os.path.relpath(elegido[0], raiz)

    # Recomendación: lo más directo que el juego entienda
    if "openxr" in apis:
        rec = "openxr"
    elif "openvr" in apis:
        rec = "openvr"
    elif "oculus" in apis:
        rec = "oculus"
    else:
        rec = "openvr"   # muchos juegos viejos de SteamVR traen OpenVR dentro de su motor

    sugerencias = []
    if rec == "openxr" and graficos:
        sugerencias.append(T("Puede usar %s y nuestro runtime OpenXR solo tiene DirectX 11: si no arranca, probá OpenVR o el argumento %s")
                           % (" / ".join(graficos), "-force-d3d11" if motor == "unity" else "-dx11"))
    if "oculus" in apis and len(apis) == 1:
        sugerencias.append(T("Solo trae el SDK de Oculus: necesita ReVive (todavía no está incluido)"))
    if "oculus" in apis and motor == "unity" and "openxr" not in apis:
        sugerencias.append(T("OVRPlugin de Unity a veces también habla OpenXR: vale la pena probar «OpenXR»"))
    if not apis:
        sugerencias.append(T("No encontré archivos de VR conocidos: puede estar dentro del motor. Se prueba con OpenVR primero"))

    return {"apis": apis, "motor": motor, "graficos": graficos, "exe": exe, "recomendado": rec,
            "sugerencias": sugerencias, "idioma": IDIOMA, "actualizado": juego.get("actualizado", ""), "fecha": int(time.time())}


def vr_en_steam(appid):
    """¿El juego es de VR? Lo dice la tienda de Steam (categorías «VR Support», «VR Supported», «VR Only»).
    None si no se pudo preguntar (sin internet): se vuelve a probar la próxima vez."""
    try:
        url = "https://store.steampowered.com/api/appdetails?appids=%s&filters=categories" % appid
        with urllib.request.urlopen(url, timeout=4) as r:
            info = json.loads(r.read().decode("utf-8")).get(appid, {})
        if not info.get("success"):
            return None
        cats = (info.get("data") or {}).get("categories", [])
        return any(c.get("id") in (31, 53, 54) or " VR" in " " + str(c.get("description", "")) for c in cats)
    except Exception:
        return None


def deteccion(juego, forzar=False):
    ruta = os.path.join(DETECTADO, juego["id"] + ".json")
    d = leer_json(ruta, None)
    if forzar or not d or d.get("actualizado") != juego.get("actualizado", "") or d.get("idioma") != IDIOMA:
        vr = d.get("vr") if d else None
        d = detectar(juego)
        if vr is not None:
            d["vr"] = vr
        guardar_json(ruta, d)
    if d.get("vr") is None:
        vr = vr_en_steam(juego["id"])
        if vr is not None:
            d["vr"] = vr
            guardar_json(ruta, d)
    return d


def es_vr(det):
    """VR según Steam, o si se encontraron archivos de VR (la tienda no marca como VR algunos programas, como Google Earth VR)"""
    return det.get("vr") is True or bool(det.get("apis"))


# ---------------------------------------------------------------- perfiles
def incluidos():
    return leer_json(os.path.join(RES, "perfiles.json"), {}).get("juegos", {})


def perfil_usuario(appid):
    return leer_json(os.path.join(PERFILES, appid + ".json"), {})


def driver_steamvr_listo():
    return os.path.isfile(os.path.join(C, "questvrbridge", "driver_vrmin", "bin", "win64", "driver_vrmin.dll"))


def steamvr_instalado():
    return os.path.isfile(os.path.join(STEAMAPPS, "common", "SteamVR", "bin", "win64", "vrserver.exe"))


def opencomposite_instalado():
    return os.path.isfile(os.path.join(C, "questvrbridge", "opencomposite", "bin", "vrclient_x64.dll"))


INTENTOS = os.path.join(PERFILES, "intentos.json")


def alternativa(det, runtime):
    """El otro camino que tiene sentido probar si este no arrancó (None si no hay otro)"""
    apis = det.get("apis", [])
    otro = {"openxr": "openvr", "openvr": "openxr"}.get(runtime)
    if not otro:
        return None
    # solo si el juego también trae esa API, o si no se encontró ninguna (puede estar dentro del motor)
    return otro if (otro in apis or not apis) else None


def resolver_juego(juego, det=None):
    appid = juego["id"]
    det = det or deteccion(juego)
    inc = incluidos().get(appid, {})
    usr = perfil_usuario(appid)

    pedido = usr.get("runtime") or "auto"
    if pedido not in RUNTIMES:
        pedido = "auto"
    runtime = pedido if pedido != "auto" else (inc.get("runtime") or det.get("recomendado") or "openvr")
    avisos = []
    # Automático: si la vez anterior no llegó ninguna imagen con este runtime, probar el otro; si uno ya anduvo, ese
    if pedido == "auto" and not inc.get("runtime"):
        prev = leer_json(INTENTOS, {}).get(appid)
        if prev and prev.get("runtime") in ("openxr", "openvr"):
            if prev.get("ok"):
                runtime = prev["runtime"]
            elif prev["runtime"] == runtime and alternativa(det, runtime):
                avisos.append(T("La vez anterior no arrancó con %s: ahora pruebo %s") % (NOMBRES[runtime], NOMBRES[alternativa(det, runtime)]))
                runtime = alternativa(det, runtime)
    # Lo que todavía no está: se usa lo más parecido, y se avisa
    if runtime == "oculus":
        avisos.append(T("Oculus (ReVive) todavía no está incluido: se usa OpenVR"))
        runtime = "openvr"
    if runtime == "steamvr" and not (driver_steamvr_listo() and steamvr_instalado()):
        falta = T("el driver de VRmin para SteamVR") if steamvr_instalado() else T("SteamVR (instalalo desde Steam) y el driver de VRmin")
        avisos.append(T("SteamVR real: falta %s. Se usa OpenVR") % falta)
        runtime = "openvr"
    if runtime == "openvr" and not opencomposite_instalado():
        avisos.append(T("Falta OpenComposite: reinstalá el motor desde la app"))

    args = list(inc.get("args", [])) + list(usr.get("args", []))
    if runtime == "plano":
        if det.get("motor") == "unity":
            args += ["-vrmode", "None"]
        elif det.get("motor") == "unreal":
            args += ["-nohmd"]
    elif usr.get("dx11", inc.get("dx11", True)):
        # Nuestro runtime (y OpenComposite encima de él) solo recibe imágenes de DirectX 11: a los motores que pueden
        # elegir se les pide DirectX 11 (si ya lo usan, no cambia nada). «dx11: false» en el perfil lo apaga.
        forzar = {"unity": "-force-d3d11", "unreal": "-dx11"}.get(det.get("motor"))
        if forzar and forzar not in args and not any(a.lower() in ("-dx12", "-d3d12", "-vulkan", "-force-vulkan", "-force-d3d12") for a in args):
            args.append(forzar)
            if det.get("graficos"):
                avisos.append(T("Gráficos: se pide DirectX 11 (%s), lo único que entiende nuestro runtime") % forzar)

    env = dict(inc.get("env", {}))
    env.update(usr.get("env", {}))
    lanzar = usr.get("lanzar") or inc.get("lanzar") or "steam"
    exe = usr.get("exe") or inc.get("exe") or det.get("exe", "")
    return {
        "id": appid, "nombre": juego["nombre"], "carpeta": juego["carpeta"],
        "pedido": pedido, "runtime": runtime, "avisos": avisos,
        "args": args, "env": env, "lanzar": lanzar if lanzar in ("steam", "directo") else "steam",
        "exe": exe, "diagnostico": bool(usr.get("diagnostico", False)),
        "estado": usr.get("estado", inc.get("estado", "sin probar")), "nota": usr.get("nota", T(inc.get("nota", ""))),
    }


# ---------------------------------------------------------------- comandos
def buscar(appid):
    j = juegos_instalados().get(appid)
    if not j:
        sys.stderr.write(T("El juego %s no está instalado en el Steam de VRmin\n") % appid)
        sys.exit(2)
    return j


def cmd_detectar(appid):
    print(json.dumps(deteccion(buscar(appid), forzar=True), ensure_ascii=False, indent=2))


def cmd_resolver(appid):
    j = buscar(appid)
    det = deteccion(j)
    r = resolver_juego(j, det)
    q = shlex.quote
    print("NOMBRE=%s" % q(r["nombre"]))
    print("CARPETA=%s" % q(r["carpeta"]))
    print("PEDIDO=%s" % q(r["pedido"]))
    print("RUNTIME=%s" % q(r["runtime"]))
    print("RUNTIME_NOMBRE=%s" % q(T(NOMBRES.get(r["runtime"], r["runtime"]))))
    print("AVISO=%s" % q(" · ".join(r["avisos"])))
    print("LANZAR=%s" % q(r["lanzar"]))
    print("EXE=%s" % q(r["exe"]))
    print("DIAGNOSTICO=%s" % ("1" if r["diagnostico"] else "0"))
    print("DETECTADO=%s" % q(T("APIs: %s · motor: %s%s") % (", ".join(det["apis"]) or T("ninguna conocida"), T(det["motor"]),
                                                           (T(" · gráficos: ") + ", ".join(det["graficos"])) if det["graficos"] else "")))
    print("SUGERENCIAS=%s" % q(" · ".join(det.get("sugerencias", []))))
    print("ARGS=(%s)" % " ".join(q(a) for a in r["args"]))
    print("ENVS=(%s)" % " ".join(q("%s=%s" % (k, v)) for k, v in r["env"].items()))


def cmd_poner(appid, clave, valor):
    usr = perfil_usuario(appid)
    if clave == "runtime":
        if valor not in RUNTIMES:
            sys.exit("Runtime desconocido: %s (opciones: %s)" % (valor, ", ".join(RUNTIMES)))
        usr["runtime"] = valor
    elif clave == "lanzar":
        if valor not in ("steam", "directo"):
            sys.exit("lanzar: steam o directo")
        usr["lanzar"] = valor
    elif clave == "estado":
        if valor not in ESTADOS:
            sys.exit("estado: " + ", ".join(ESTADOS))
        usr["estado"] = valor
    elif clave == "args":
        usr["args"] = json.loads(valor) if valor.strip().startswith("[") else shlex.split(valor)
    elif clave == "env":
        usr["env"] = json.loads(valor)
    elif clave in ("nota", "exe"):
        usr[clave] = valor
    elif clave in ("diagnostico", "dx11"):
        usr[clave] = valor in ("1", "true", "si", "sí")
    else:
        sys.exit("Clave desconocida: " + clave)
    usr["modificado"] = int(time.time())
    guardar_json(os.path.join(PERFILES, appid + ".json"), usr)
    print("ok")


def cmd_intento(appid, runtime):
    d = leer_json(INTENTOS, {})
    prev = d.get(appid, {})
    # (si ese runtime ya anduvo antes, sigue contando como bueno: un juego que se cerró rápido no lo cambia)
    d[appid] = {"runtime": runtime, "ok": bool(prev.get("ok") and prev.get("runtime") == runtime), "t": int(time.time())}
    guardar_json(INTENTOS, d)
    print("ok")


def cmd_exito(appid):
    d = leer_json(INTENTOS, {})
    if appid in d:
        d[appid]["ok"] = True
        guardar_json(INTENTOS, d)
    print("ok")


def cmd_listar():
    out = []
    for appid, j in sorted(juegos_instalados().items(), key=lambda kv: kv[1]["nombre"].lower()):
        det = deteccion(j)
        r = resolver_juego(j, det)
        out.append({"id": appid, "nombre": j["nombre"], "deteccion": det, "perfil": r, "vr": es_vr(det),
                    "runtimeNombre": T(NOMBRES.get(r["runtime"], r["runtime"]))})
    print(json.dumps({"juegos": out, "steamvr": steamvr_instalado(), "driverSteamvr": driver_steamvr_listo(),
                      "opencomposite": opencomposite_instalado()}, ensure_ascii=False))


def cmd_controles_oc(carpeta, *destinos):
    """OpenComposite no entiende el modo «grab» de los controles de Touch (agarrar con el grip): ese botón quedaba sin
    nada (en Half-Life 2: VR Mod no se podía agarrar). Se arma, a partir de los controles de Touch del propio juego, una
    copia con el grip como botón común, donde OpenComposite busca controles propios (carpeta del juego/OpenComposite).
    Si el usuario ya puso los suyos ahí (sin nuestra marca), no se tocan."""
    origen = None
    for raiz, dirs, archivos in os.walk(carpeta):
        if raiz[len(carpeta):].count(os.sep) >= 4:
            dirs[:] = []
            continue
        if "actions.json" in archivos:
            m = leer_json(os.path.join(raiz, "actions.json"), {})
            for b in m.get("default_bindings", []) if isinstance(m, dict) else []:
                if b.get("controller_type") == "oculus_touch" and b.get("binding_url"):
                    origen = os.path.join(raiz, b["binding_url"])
                    break
        if origen:
            break
    if not origen or not os.path.isfile(origen):
        return
    datos = leer_json(origen, None)
    if not isinstance(datos, dict):
        return
    cambios = 0
    for conjunto in (datos.get("bindings") or {}).values():
        for src in conjunto.get("sources", []):
            ins = src.get("inputs", {})
            # (el modo «grab», o un botón con la entrada «grab»: OpenComposite arma …/squeeze/grab, que no existe)
            if src.get("mode") == "grab" or "grab" in ins:
                src["mode"] = "button"
                if "grab" in ins:
                    ins["click"] = ins.pop("grab")
                ins.pop("held", None)
                cambios += 1
    if not cambios:
        return
    datos["vrmin"] = "armado por VRmin a partir de %s (agarrar con el grip)" % os.path.basename(origen)
    for d in set(os.path.normpath(x) for x in (destinos or (carpeta,))):
        ruta = os.path.join(d, "OpenComposite", "oculus_touch.json")
        actual = leer_json(ruta, None)
        if actual is not None and not (isinstance(actual, dict) and "vrmin" in actual):
            continue
        guardar_json(ruta, datos)
        print(T("Controles de Touch para OpenComposite: el grip agarra (%d botones arreglados)") % cambios)


def main(a):
    if len(a) >= 2 and a[0] == "detectar":
        cmd_detectar(a[1])
    elif len(a) >= 2 and a[0] == "resolver":
        cmd_resolver(a[1])
    elif len(a) >= 4 and a[0] == "poner":
        cmd_poner(a[1], a[2], a[3])
    elif a and a[0] == "listar":
        cmd_listar()
    elif len(a) >= 3 and a[0] == "intento":
        cmd_intento(a[1], a[2])
    elif len(a) >= 2 and a[0] == "exito":
        cmd_exito(a[1])
    elif len(a) >= 2 and a[0] == "controles_oc":
        cmd_controles_oc(*a[1:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
