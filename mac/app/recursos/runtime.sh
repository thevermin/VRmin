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
    id="$(/usr/bin/python3 -c "import sys,uuid; print('{0.0.1.00000000}.{%s}' % str(uuid.UUID(bytes_le=bytes.fromhex(sys.argv[1]))).upper())" "$hex")"
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
  # OpenComposite arranca siempre con una sesión D3D11 provisoria, y el D3D11 de 32 bits de Wine (con OpenGL) no la
  # puede crear en una Mac: para hl2.exe (Half-Life 2: VR Mod) se le pide a Wine que lo haga con Vulkan
  # (y sin el hilo aparte de wined3d, «csmt»: en 32 bits no lo puede crear y el dispositivo falla)
  if [ ! -f "$Q/.hl2vk2" ]; then
    "$WINE" reg add 'HKCU\Software\Wine\AppDefaults\hl2.exe\Direct3D' /v renderer /t REG_SZ /d vulkan /f >/dev/null 2>&1 \
      && "$WINE" reg add 'HKCU\Software\Wine\AppDefaults\hl2.exe\Direct3D' /v csmt /t REG_DWORD /d 0 /f >/dev/null 2>&1 && touch "$Q/.hl2vk2"
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
  if /usr/bin/python3 - "$WINEPREFIX/system.reg" "$id" "$Q/luid.reg" <<'PY'
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
# (el driver da el área de juego), sin las paredes del área de juego (el Quest tiene las suyas), sin SteamVR Home y sin
# el menú de SteamVR ni su ventana (en la Mac se cerraban y abrían todo el tiempo y la ponían muy lenta).
# allowDisplayLockedMode: dentro de Wine, SteamVR cree que la sesión de Windows está bloqueada y en vez de dibujar
# llenaba todo de un color verde azulado
poner_driver_steamvr() {
  local d sha
  for d in "$RES/steamvr/driver_vrmin" "$RES/../../steamvr/driver_vrmin"; do [ -f "$d/bin/win64/driver_vrmin.dll" ] && break; done
  [ -f "$d/bin/win64/driver_vrmin.dll" ] || { say "Falta el driver de VRmin para SteamVR" "VRmin's SteamVR driver is missing"; return 1; }
  sha="$(shasum -a 256 "$d/bin/win64/driver_vrmin.dll" | cut -d' ' -f1)"
  [ "$(cat "$Q/driver_vrmin/.version" 2>/dev/null)" = "$sha" ] && return 0
  rm -rf "$Q/driver_vrmin" && cp -R "$d" "$Q/driver_vrmin" && echo "$sha" > "$Q/driver_vrmin/.version"
}
ajustes_steamvr() {
  mkdir -p "$Q/openvr/config"
  cat > "$Q/openvr/config/steamvr.vrsettings" <<'JSON'
{
   "driver_null" : { "enable" : false },
   "dashboard" : { "enableDashboard" : false, "showDesktop" : false },
   "collisionBounds" : { "CollisionBoundsColorGammaA" : 0, "CollisionBoundsFadeDistance" : 0 },
   "steamvr" : {
      "requireHmd" : true,
      "forcedDriver" : "vrmin",
      "activateMultipleDrivers" : false,
      "enableHomeApp" : false,
      "showMirrorView" : false,
      "startMonitorFromAppLaunch" : false,
      "startDashboardFromAppLaunch" : false,
      "startOverlayAppsFromDashboard" : false,
      "allowDisplayLockedMode" : true
   }
}
JSON
}

# SteamVR real, abierto antes que el juego y con el visor y los controles de VRmin ya agregados: si el juego lo abre él
# mismo, se conecta antes de que cargue el driver, arma sus controles sin dispositivos y se queda sin controles
abrir_steamvr() {
  if pgrep -f 'vrserver.exe' >/dev/null 2>&1 && grep -q "Right controller activated" "$Q/driver_vrmin.log" 2>/dev/null; then return 0; fi
  rm -f "$Q/driver_vrmin.log"
  say "Abriendo SteamVR…" "Opening SteamVR…"
  WINEDEBUG=-all "$WINE" "$STEAMVR_WIN\\bin\\win64\\vrserver.exe" >/dev/null 2>&1 < /dev/null &
  local i
  for i in $(seq 1 60); do
    grep -q "Right controller activated" "$Q/driver_vrmin.log" 2>/dev/null && { say "SteamVR listo" "SteamVR ready"; return 0; }
    sleep 0.5
  done
  say "SteamVR tardó en abrir; abro el juego igual" "SteamVR took long to open; opening the game anyway"
}

# Cierra SteamVR cuando el juego se cierra (si no, sigue dibujando y calentando la Mac): espera a que aparezca algún
# programa de la carpeta del juego (hasta 5 minutos) y, cuando ya no queda ninguno durante 30 segundos, cierra SteamVR.
# (con una pausa así, un juego que abre otro, como The Lab con Robot Repair, no lo cierra)
cerrar_steamvr_con_el_juego() {
  local carpeta="$1"
  [ -n "$carpeta" ] || return 0
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
      if [ $sin -ge 30 ]; then
        for p in vrmonitor vrcompositor vrdashboard vrwebhelper vrserver vrstartup; do pkill -f "$p.exe" 2>/dev/null; done
        break
      fi
    done
  ) > /dev/null 2>&1 < /dev/null &
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
      [ "$antes" = "steamvr" ] || openxr_activo "$STEAMVR_WIN\\steamxr_win64.json"
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
