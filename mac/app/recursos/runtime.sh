# VRmin Host · elegir con qué runtime de VR habla el Windows de VRmin (lo usa jugar.sh con "source")
#
# Los juegos encuentran su runtime así, y eso es lo que se cambia acá:
#   OpenXR  → el registro de Windows: HKLM\Software\Khronos\OpenXR\1\ActiveRuntime (un .json)
#   OpenVR  → el archivo openvrpaths.vrpath (en AppData\Local\openvr): la carpeta del "runtime" y los drivers
#
# Modos:
#   openxr   OpenXR → nuestro runtime.            OpenVR → OpenComposite → nuestro runtime
#   openvr   igual que openxr (el juego usa OpenVR, y OpenComposite lo pasa a OpenXR)
#   steamvr  OpenXR y OpenVR → el SteamVR real de Valve, con el driver de VRmin (experimental)
#   plano    sin runtime: el juego no encuentra visor y (si sabe) abre en una pantalla plana
#
# Se acuerda del último modo aplicado (prefijo/drive_c/questvrbridge/.runtime) para no reescribir el registro si no cambió.

Q="$WINEPREFIX/drive_c/questvrbridge"
VRPATH_DIR="$WINEPREFIX/drive_c/users/$USER/AppData/Local/openvr"
STEAMVR_WIN='C:\Program Files (x86)\Steam\steamapps\common\SteamVR'

# escribir_vrpath CARPETA_RUNTIME [DRIVER_EXTERNO]   (rutas de Windows)
escribir_vrpath() {
  local rt="${1//\\/\\\\}" drv="${2//\\/\\\\}" ext="null"
  [ -n "$2" ] && ext="[ \"$drv\" ]"
  mkdir -p "$VRPATH_DIR"
  cat > "$VRPATH_DIR/openvrpaths.vrpath" <<JSON
{
	"config" : [ "C:\\\\questvrbridge\\\\openvr\\\\config" ],
	"external_drivers" : $ext,
	"jsonid" : "vrpathreg",
	"log" : [ "C:\\\\questvrbridge\\\\openvr\\\\logs" ],
	"runtime" : [ "$rt" ],
	"version" : 1
}
JSON
}

# openxr_activo RUTA_JSON [RUTA_JSON_32] (rutas de Windows; la segunda, para juegos de 32 bits)
# Micrófono de los juegos: el que se eligió en VRmin Host (archivo «microfono»: el UID de CoreAudio del micrófono,
# «VRminMic_UID» para el del visor; vacío = el de macOS). Wine lo toma como el micrófono predeterminado de Windows
# (DefaultInput / DefaultVoiceInput de winecoreaudio, con el número de dispositivo que Wine le da a ese UID).
microfono_wine() {
  local uid; uid="$(cat "$P/microfono" 2>/dev/null)"
  [ "$uid" = "$(cat "$Q/.microfono" 2>/dev/null)" ] && [ -f "$Q/.microfono" ] && return 0
  local base='HKCU\Software\Wine\Drivers\winecoreaudio.drv'
  if [ -z "$uid" ]; then
    "$WINE" reg delete "$base" /v DefaultInput /f >/dev/null 2>&1
    "$WINE" reg delete "$base" /v DefaultVoiceInput /f >/dev/null 2>&1
  else
    local hex id
    hex="$("$WINE" reg query "$base\\devices\\1,$uid" /v guid 2>/dev/null | tr -d '\r' | awk '/REG_BINARY/ {print $3}')"
    if [ ${#hex} -ne 32 ]; then   # (Wine todavía no lo vio: se le da un número fijo, el mismo que va a usar Wine)
      hex="$(printf '%s' "$uid" | md5 -q 2>/dev/null || printf '%s' "$uid" | md5sum | cut -c1-32)"
      "$WINE" reg add "$base\\devices\\1,$uid" /v guid /t REG_BINARY /d "$hex" /f >/dev/null 2>&1
    fi
    # bytes del GUID → {0.0.1.00000000}.{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} (los tres primeros campos, al revés)
    id="$("$PY" -c "import sys,uuid; print('{0.0.1.00000000}.{%s}' % str(uuid.UUID(bytes_le=bytes.fromhex(sys.argv[1]))).upper())" "$hex")"
    "$WINE" reg add "$base" /v DefaultInput /t REG_SZ /d "$id" /f >/dev/null 2>&1
    "$WINE" reg add "$base" /v DefaultVoiceInput /t REG_SZ /d "$id" /f >/dev/null 2>&1
  fi
  printf '%s' "$uid" > "$Q/.microfono"
  if [ -n "$uid" ]; then say "Micrófono de los juegos: $uid" "Games microphone: $uid"; else say "Micrófono de los juegos: el de macOS" "Games microphone: macOS default"; fi
}

openxr_activo() {
  "$WINE" reg add 'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ /d "$1" /f >/dev/null 2>&1
  [ -n "$2" ] && "$WINE" reg add 'HKLM\Software\Wow6432Node\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ /d "$2" /f >/dev/null 2>&1
  return 0
}

# OpenComposite: el que viene con VRmin Host (compilado por VRmin desde el código de GameNative, ver
# opencomposite/README.md; GPLv3). Se copia al motor cuando cambia.
poner_oc() { # archivo-de-VRmin nombre-en-el-motor marca
  local f="$RES/opencomposite/$1" sha
  sha="$(shasum -a 256 "$f" 2>/dev/null | cut -d' ' -f1)"
  [ -n "$sha" ] || { echo "Falta $1 en VRmin Host"; return 0; }
  [ "$(cat "$Q/opencomposite/$3" 2>/dev/null)" = "$sha" ] && return 0
  mkdir -p "$Q/opencomposite/bin"
  cp "$f" "$Q/opencomposite/bin/$2" && echo "$sha" > "$Q/opencomposite/$3" && echo "OpenComposite instalado ($2)"
  return 0
}

# Juegos de 32 bits (Half-Life 2: VR Mod): nuestro runtime de 32 bits registrado y OpenComposite de 32 bits
# (vrclient.dll, al lado del de 64). Una sola vez.
asegurar_32bits() {
  [ -f "$Q/questvrbridge_openxr32.dll" ] || return 0
  if [ ! -f "$Q/.runtime32" ]; then
    "$WINE" reg add 'HKLM\Software\Wow6432Node\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ /d 'C:\questvrbridge\questvrbridge_openxr32.json' /f >/dev/null 2>&1 \
      && touch "$Q/.runtime32"
  fi
  poner_oc vrclient.dll vrclient.dll .version32
}

# Juegos de 64 bits (con las interfaces de OpenVR nuevas que piden los juegos de 2026, como IVRSystem_026 de
# No Man's Sky)
asegurar_64bits() { poner_oc vrclient_x64.dll vrclient_x64.dll .version64; }

# Copia directa (texturas compartidas de DXMT): DXMT reconoce la GPU por su número de Metal (registryID, dado vuelta),
# y Wine le había puesto otro número propio (LUID). Si no coinciden, DXMT no puede compartir texturas. Se le pone a
# Wine el mismo número (en el registro de Windows; solo si es distinto).
gpu_luid_dxmt() {
  local id
  id=$(/usr/sbin/ioreg -r -c IOAccelerator -d 1 -w0 2>/dev/null | grep -o 'id 0x[0-9a-fA-F]*' | head -1 | cut -c4-)
  [ -n "$id" ] || { echo "Copia directa: no pude leer el número de la GPU"; return 0; }
  if "$PY" - "$WINEPREFIX/system.reg" "$id" "$Q/luid.reg" <<'PY'
import re, struct, sys
reg, idhex, out = sys.argv[1], sys.argv[2], sys.argv[3]
quiero = ",".join("%02x" % b for b in struct.pack(">Q", int(idhex, 16)))   # LUID = registryID dado vuelta (como DXMT y MoltenVK)
try:
    txt = open(reg, encoding="utf-8", errors="replace").read()
except OSError:
    sys.exit(1)
claves = []
for m in re.finditer(r'^\[(System\\\\ControlSet001\\\\Enum\\\\PCI\\\\[^\]]*\\\\Properties\\\\\{60B193CB-5276-4D0F-96FC-F173ABAD3EC6\}\\\\0002)\][^\n]*\n(?:#[^\n]*\n)*@=hex\(ffff0009\):([0-9a-f,]+)', txt, re.M | re.I):
    if m.group(2).lower() != quiero:
        claves.append(m.group(1).replace("\\\\", "\\"))
if not claves:
    sys.exit(1)
with open(out, "w", encoding="utf-8") as f:
    f.write("REGEDIT4\r\n")
    for k in claves:
        f.write("\r\n[HKEY_LOCAL_MACHINE\\%s]\r\n@=hex(ffff0009):%s\r\n" % (k, quiero))
PY
  then
    "$WINE" regedit /S 'C:\questvrbridge\luid.reg' >/dev/null 2>&1
    echo "Copia directa: número de la GPU de Wine igualado al de Metal ($id)"
  fi
}

# SteamVR real: el driver de VRmin (el que viene con VRmin Host, copiado al motor cuando cambia) y los ajustes de
# SteamVR en C:\questvrbridge\openvr\config (solo los usa VRmin): solo el visor de VRmin, sin pedir armar la sala
# (el driver da el área de juego), sin las paredes del área de juego (el Quest tiene las suyas) y sin SteamVR Home.
# El menú de SteamVR (botón de menú del control izquierdo), sin su ventana en la Mac. Sus paneles son páginas web que
# Chromium dibuja en la placa de video y SteamVR le pasa a su compositor (necesita el DXMT de VRmin con
# reshare-opened-textures, keyed-mutex, dxgi-usage y offscreen-swapchain). Dibujados sin la placa (forceCEFMode cpu)
# necesitan OpenGL, que vrwebhelper no puede crear dentro de Wine. Steam no se conecta a SteamVR (ajustes_dll, en
# entorno.sh: dentro de Wine se colgaba). useNewDesktop false: el menú mostraba el escritorio de Windows con la forma de
# Windows 10 (Graphics Capture), que Wine no tiene, y se cerraba y volvía a abrir todo el tiempo. allowDisplayLockedMode:
# dentro de Wine, SteamVR cree que la sesión de Windows está bloqueada y en vez de dibujar llenaba todo de un color
# verde azulado. cefMaxFrameRate 30: los paneles del menú se dibujan a 30 cuadros por segundo como máximo (120 por
# defecto), para dejarle la placa de video al juego
poner_driver_steamvr() {
  local d sha
  for d in "$RES/steamvr/driver_vrmin" "$RES/../../steamvr/driver_vrmin"; do [ -f "$d/bin/win64/driver_vrmin.dll" ] && break; done
  [ -f "$d/bin/win64/driver_vrmin.dll" ] || { say "Falta el driver de VRmin para SteamVR" "VRmin's SteamVR driver is missing"; return 1; }
  sha="$(shasum -a 256 "$d/bin/win64/driver_vrmin.dll" | cut -d' ' -f1)"
  [ "$(cat "$Q/driver_vrmin/.version" 2>/dev/null)" = "$sha" ] && return 0
  rm -rf "$Q/driver_vrmin" && cp -R "$d" "$Q/driver_vrmin" && echo "$sha" > "$Q/driver_vrmin/.version"
}
ajustes_steamvr() {
  # (pruebas anteriores para que Steam no se conecte a SteamVR; ahora lo hace ajustes_dll, en entorno.sh, para todo)
  if [ -f "$Q/.steam_sin_vrclient" ]; then
    "$WINE" reg delete 'HKCU\Software\Wine\AppDefaults\steam.exe\DllOverrides' /v vrclient /f >/dev/null 2>&1
    rm -f "$Q/.steam_sin_vrclient"
  fi
  rm -f "$Q/.steam_sin_d3d11"
  mkdir -p "$Q/openvr/config"
  # Lo nuestro se escribe siempre; lo demás que guarda SteamVR (como la resolución, en general o por juego) se conserva.
  # La resolución arranca en 25 % (SteamVR, en automático, elige casi el doble: no baja de lo de los primeros visores)
  "$PY" - "$Q/openvr/config/steamvr.vrsettings" <<'PY'
import json, sys
f = sys.argv[1]
try:
    s = json.load(open(f))
except Exception:
    s = {}
fijo = {
    "driver_null": {"enable": False},
    # (drivers de otros visores: SteamVR los carga igual aunque use solo el de VRmin, y el de Lighthouse le agrega una
    #  ventana oculta para emparejar controles de Valve, con su propio proceso de navegador de ~400 MB)
    "driver_lighthouse": {"enable": False}, "driver_oculus": {"enable": False},
    "driver_oculus_legacy": {"enable": False}, "driver_vrlink": {"enable": False},
    "dashboard": {"useNewDesktop": False, "showDesktop": False, "cefMaxFrameRate": 30},
    "collisionBounds": {"CollisionBoundsColorGammaA": 0, "CollisionBoundsFadeDistance": 0},
    "steamvr": {"requireHmd": True, "forcedDriver": "vrmin", "activateMultipleDrivers": False, "enableHomeApp": False,
                "showMirrorView": False, "startMonitorFromAppLaunch": False, "startOverlayAppsFromDashboard": False,
                "allowDisplayLockedMode": True},
}
# Si SteamVR se cerró de golpe, la vez siguiente bloquea los drivers («modo seguro»: «Debido a un cierre inesperado,
# algunos complementos se bloquearon») y su botón para volver a activarlos abre una ventana que en VRmin no se usa:
# sin el de VRmin no hay visor. Se vuelve a activar siempre.
s.setdefault("driver_vrmin", {}).update({"enable": True, "blocked_by_safe_mode": False})
# (de versiones anteriores de VRmin)
s.get("dashboard", {}).pop("enableDashboard", None)
s.get("dashboard", {}).pop("allowDashboardAutoLaunchWithSteamVRHome", None)
s.get("steamvr", {}).pop("forceCEFMode", None)
s.get("steamvr", {}).pop("allowDashboardAutoLaunchWithSteamVRHome", None)
s.pop("VRWebHelper", None)
for sec, v in fijo.items():
    s.setdefault(sec, {}).update(v)
for k, v in {"supersampleManualOverride": True, "supersampleScale": 0.25}.items():
    s["steamvr"].setdefault(k, v)
json.dump(s, open(f, "w"), indent=3)
PY
  # La ventana de ajustes de SteamVR en el escritorio (que en VRmin no se usa) se crea al abrirla y no al arrancar,
  # oculta: es otro proceso de navegador de ~400 MB. Se revisa cada vez porque las actualizaciones de SteamVR lo vuelven
  # a poner
  "$PY" - "$STEAMLIB/common/SteamVR/resources/webhelperoverlays.json" <<'PY'
import json, sys
f = sys.argv[1]
try:
    s = json.load(open(f))
except Exception:
    sys.exit(0)
w = s.get("settings_desktop", {})
if w.get("preload"):
    w["preload"] = False
    json.dump(s, open(f, "w"), indent="\t")
PY
}

# SteamVR real, abierto antes que el juego y con el visor y los controles de VRmin ya agregados: si lo abre el juego,
# se conecta antes de que cargue el driver y se queda sin controles (OpenVR), o no encuentra el visor (OpenXR).
# Se abre con vrstartup (su ventana se conecta y hace cargar los drivers; vrserver solo no carga ninguno hasta que algún
# programa se le conecta). Si ya está abierto con los controles de VRmin, el juego nuevo lo usa.
abrir_steamvr() {
  if pgrep -f 'vrserver.exe' >/dev/null 2>&1 && grep -q "Right controller activated" "$Q/driver_vrmin.log" 2>/dev/null; then
    say "SteamVR ya está abierto" "SteamVR is already open"; return 0
  fi
  cerrar_steamvr   # (uno a medio abrir o sin el driver de VRmin)
  rm -f "$Q/driver_vrmin.log"
  # (el menú de SteamVR está listo cuando su página crea sus paneles: lo dice su registro, que empieza de nuevo)
  local menu="$Q/openvr/logs/vrwebhelper_systemui.txt"
  rm -f "$menu"
  say "Abriendo SteamVR…" "Opening SteamVR…"
  WINEDEBUG=-all "$WINE" "$STEAMVR_WIN\\bin\\win64\\vrstartup.exe" >/dev/null 2>&1 < /dev/null &
  local i
  for i in $(seq 1 120); do
    grep -q "Right controller activated" "$Q/driver_vrmin.log" 2>/dev/null && break
    sleep 0.5
  done
  grep -q "Right controller activated" "$Q/driver_vrmin.log" 2>/dev/null \
    || { say "SteamVR tardó en abrir; abro el juego igual" "SteamVR took long to open; opening the game anyway"; return 0; }
  # El juego se abre cuando el menú ya cargó: cargando a la vez, el menú (páginas web que sirve SteamVR) tardaba casi
  # un minuto en aparecer y el juego ya había empezado
  for i in $(seq 1 120); do
    grep -q 'Created: system.HeadsetView' "$menu" 2>/dev/null && break
    sleep 0.5
  done
  say "SteamVR listo" "SteamVR ready"
}

# Cierra SteamVR como al tocar la X de su ventana (vrmonitor.exe), y espera a que termine. Cerrarlo de golpe le
# parece una falla: la vez siguiente arranca en modo seguro y avisa «se ha desactivado debido a un fallo en la última
# ejecución». Solo si en 15 segundos no se cerró, a la fuerza.
cerrar_steamvr_bien() {
  pgrep -f 'vrserver.exe' >/dev/null 2>&1 || return 0
  WINEDEBUG=-all "$WINE" taskkill /IM vrmonitor.exe >/dev/null 2>&1 < /dev/null
  local i; for i in $(seq 1 30); do pgrep -f 'vrserver.exe' >/dev/null 2>&1 || return 0; sleep 0.5; done
  local p; for p in vrmonitor vrcompositor vrdashboard vrwebhelper vrserver vrstartup; do pkill -f "$p.exe" 2>/dev/null; done
  sleep 2
}

# Cierra SteamVR (y deja de vigilar el juego que lo usaba)
cerrar_steamvr() {
  local v; v="$(cat "$Q/.vigia_steamvr" 2>/dev/null)"
  [ -n "$v" ] && kill "$v" 2>/dev/null; rm -f "$Q/.vigia_steamvr"
  cerrar_steamvr_bien
}

# Con la Biblioteca de VRmin (sin «steam_ventana»), esconde la ventana de Steam si aparece en los próximos 30 segundos
# (Steam la muestra un rato después de cerrarse el juego o SteamVR)
# (después de «Mostrar Steam» no hace nada hasta «Ocultar Steam»: ver ocultar_steam.sh)
esconder_steam() {
  [ -f "$P/steam_ventana" ] && return 0
  local s; for s in 2 3 5 5 5 10; do sleep $s; bash "$RES/ocultar_steam.sh" > /dev/null 2>&1; done
}

# Cierra SteamVR cuando el juego se cierra (si no, sigue dibujando y calentando la Mac): espera a que aparezca algún
# programa de la carpeta del juego (hasta 5 minutos) y, cuando ya no queda ninguno durante 30 segundos, cierra SteamVR.
# (con una pausa así, un juego que abre otro, como The Lab con Robot Repair, no lo cierra)
cerrar_steamvr_con_el_juego() {
  local carpeta="$1"
  [ -n "$carpeta" ] || return 0
  local v; v="$(cat "$Q/.vigia_steamvr" 2>/dev/null)"
  [ -n "$v" ] && kill "$v" 2>/dev/null   # (el que vigilaba el juego anterior: SteamVR ahora es de este)
  (
    export FILTRO="steamapps\\common\\$carpeta" FILTRO2="steamapps/common/$carpeta"
    abierto() { ps -axo command= | awk 'BEGIN { f = tolower(ENVIRON["FILTRO"]); g = tolower(ENVIRON["FILTRO2"]) } index(tolower($0), f) || index(tolower($0), g) { n++ } END { exit !n }'; }
    visto=0; desde=$(date +%s); sin=0
    while pgrep -f 'vrserver.exe' >/dev/null 2>&1; do
      sleep 5
      if abierto; then visto=1; sin=0
      elif [ $visto = 1 ]; then sin=$((sin + 5))
      elif [ $(( $(date +%s) - desde )) -gt 300 ]; then sin=999
      fi
      # (al cerrarse el juego, y al cerrarse SteamVR, Steam muestra su ventana: con la Biblioteca de VRmin se vuelve
      #  a esconder)
      [ $visto = 1 ] && [ $sin = 5 ] && esconder_steam &
      if [ $sin -ge 30 ]; then cerrar_steamvr_bien; esconder_steam; break; fi
    done
    rm -f "$Q/.vigia_steamvr"
  ) > /dev/null 2>&1 < /dev/null &
  echo $! > "$Q/.vigia_steamvr"
  disown 2>/dev/null || true
}

preparar_runtime() {
  local modo="$1" antes
  antes="$(cat "$Q/.runtime" 2>/dev/null)"
  mkdir -p "$Q/openvr/config" "$Q/openvr/logs"
  case "$modo" in
    openxr|openvr)
      escribir_vrpath 'C:\questvrbridge\opencomposite'
      [ "$antes" = "openxr" ] || { openxr_activo 'C:\questvrbridge\questvrbridge_openxr.json' 'C:\questvrbridge\questvrbridge_openxr32.json'; rm -f "$Q/.runtime32"; }
      asegurar_64bits
      asegurar_32bits
      echo "openxr" > "$Q/.runtime"
      echo "Runtime: OpenXR → VRmin · OpenVR → OpenComposite → VRmin"
      ;;
    steamvr)
      poner_driver_steamvr || return 1
      ajustes_steamvr
      escribir_vrpath "$STEAMVR_WIN" 'C:\questvrbridge\driver_vrmin'
      # (OpenXR de 64 y de 32 bits → SteamVR: con el de 32 todavía en el nuestro, SteamVR avisa «SteamVR isn't set as the
      #  default OpenXR runtime». .steamxr32: los Windows que ya estaban en este modo con solo el de 64)
      if [ "$antes" != "steamvr" ] || [ ! -f "$Q/.steamxr32" ]; then
        openxr_activo "$STEAMVR_WIN\\steamxr_win64.json" "$STEAMVR_WIN\\steamxr_win32.json" && touch "$Q/.steamxr32"
      fi
      echo "steamvr" > "$Q/.runtime"
      say "Runtime: SteamVR real con el driver de VRmin (experimental)" "Runtime: real SteamVR with the VRmin driver (experimental)"
      ;;
    plano)
      mkdir -p "$Q/sin_vr"
      escribir_vrpath 'C:\questvrbridge\sin_vr'
      [ "$antes" = "plano" ] || { openxr_activo 'C:\questvrbridge\sin_vr\no_hay.json' 'C:\questvrbridge\sin_vr\no_hay.json'; rm -f "$Q/.runtime32"; }
      echo "plano" > "$Q/.runtime"
      say "Runtime: ninguno (el juego no ve el visor y abre en pantalla plana si puede)" "Runtime: none (the game doesn't see the headset and opens flat if it can)"
      ;;
    *)
      say "Modo de runtime desconocido: $modo" "Unknown runtime mode: $modo"; return 1 ;;
  esac
}
