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
openxr_activo() {
  "$WINE" reg add 'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ /d "$1" /f >/dev/null 2>&1
  [ -n "$2" ] && "$WINE" reg add 'HKLM\Software\Wow6432Node\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ /d "$2" /f >/dev/null 2>&1
  return 0
}

# Juegos de 32 bits (Half-Life 2: VR Mod): nuestro runtime de 32 bits registrado y OpenComposite de 32 bits
# (vrclient.dll, al lado del de 64). Una sola vez; OpenComposite se baja si falta.
OC_URL_32="https://znix.xyz/OpenComposite/download.php?arch=x86&branch=openxr"
asegurar_32bits() {
  [ -f "$Q/questvrbridge_openxr32.dll" ] || return 0
  if [ ! -f "$Q/.runtime32" ]; then
    "$WINE" reg add 'HKLM\Software\Wow6432Node\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ /d 'C:\questvrbridge\questvrbridge_openxr32.json' /f >/dev/null 2>&1 \
      && touch "$Q/.runtime32"
  fi
  local oc="$P/descargas/opencomposite_x86.dll"
  if [ ! -f "$Q/opencomposite/bin/vrclient.dll" ]; then
    mkdir -p "$P/descargas" "$Q/opencomposite/bin"
    file -b "$oc" 2>/dev/null | grep -q 'PE32 .*DLL' || curl -fsL -o "$oc" "$OC_URL_32" || true
    file -b "$oc" 2>/dev/null | grep -q 'PE32 .*DLL' && cp "$oc" "$Q/opencomposite/bin/vrclient.dll" \
      && echo "OpenComposite de 32 bits instalado"
  fi
  return 0
}

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

preparar_runtime() {
  local modo="$1" antes
  antes="$(cat "$Q/.runtime" 2>/dev/null)"
  mkdir -p "$Q/openvr/config" "$Q/openvr/logs"
  case "$modo" in
    openxr|openvr)
      escribir_vrpath 'C:\questvrbridge\opencomposite'
      [ "$antes" = "openxr" ] || { openxr_activo 'C:\questvrbridge\questvrbridge_openxr.json' 'C:\questvrbridge\questvrbridge_openxr32.json'; rm -f "$Q/.runtime32"; }
      asegurar_32bits
      echo "openxr" > "$Q/.runtime"
      echo "Runtime: OpenXR → VRmin · OpenVR → OpenComposite → VRmin"
      ;;
    steamvr)
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
