#!/bin/bash
# Instala el "motor" de QuestVRBridge en la carpeta de datos:
#  - Wine 11 (compilación para Mac de Gcenx, la que distribuye WineHQ) + bibliotecas de Sikarugir (GStreamer…)
#    (Wine 11: texturas compartidas entre programas, para la copia directa de DXMT. El Wine 11 de Sikarugir
#     no puede abrir programas nuevos: wineboot falla con «failed to start wineboot 1», probado en CI)
#  - DXMT (DirectX 11 -> Metal): la versión de notpop (github.com/notpop/dxmt, Wine 11 Metal view fix) con
#    mac/dxmt/msaa-8x-to-4x.patch, compilada por VRmin (.github/workflows/dxmt.yml) y publicada con VRmin 0.55. El
#    DXMT oficial v0.80 no puede crear la ventana de Metal con el Wine de Gcenx («Failed to create metal view»: los
#    juegos se cierran a los 5 segundos) y el más nuevo no dibuja algunos juegos (Aircar)
#  - Un Windows (prefijo) con nuestro runtime OpenXR registrado
#  - OpenComposite para juegos OpenVR/SteamVR (sin tocar archivos de los juegos)
set -e
source "$(dirname "$0")/entorno.sh"
cd "$P"
# Todo lo que pasa queda también en logs/instalar_motor.log (para revisar errores)
exec > >(tee -a "$P/logs/instalar_motor.log") 2>&1
echo "======== $(date '+%Y-%m-%d %H:%M:%S') · instalar_motor.sh"
trap 'c=$?; [ $c -ne 0 ] && echo "ERROR: terminó con código $c en la línea $LINENO"' EXIT
BASE="https://github.com/Sikarugir-App"
# wine-devel 11.8: exporta las funciones de la ventana de Mac que necesita DXMT (la 11.0 estable y la 11.18 no)
ENGINE="gcenx wine-devel 11.8"
ENGINE_URL="https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.8/wine-devel-11.8-osx64.tar.xz"
ENGINE_SHA="5c9f0984961ee6289f4f2c4fe751c27f02b6c28855154e3f99b7e2bd8108b620"
ENGINE_APP="Wine Devel.app"
TPL="Template-1.0.21"
TPL_URL="$BASE/Template/releases/download/v1.0/$TPL.tar.xz"
TPL_SHA="bbe996e4e4375318485953d0c7818b7b4b0a4dc1f13303bcc584f99f7602f78d"
DXMT_URL="https://github.com/thevermin/VRmin/releases/download/v0.55/dxmt-vrmin-1.tar.gz"
DXMT_SHA="c840fc9d736cd65a996ae20d0a5691de46696a5f62ab6c9cf868dd0e2c91e56d"
DXMT_DIR="$P/dxmt/vrmin-1"
export WINEDEBUG=-all

paso() { echo; say "==> $1" "==> ${2:-$1}"; }
bajar() { # url archivo sha
  curl -fL --progress-bar -o "$2" "$1"
  echo "$3  $2" | shasum -a 256 -c - || { rm -f "$2"; say "La descarga no coincide. Probá de nuevo." "The download doesn't match. Try again."; exit 1; }
}

paso "Comprobando el sistema" "Checking the system"
sw_vers -productVersion
MAJ=$(sw_vers -productVersion | cut -d. -f1)
[ "$MAJ" -ge 14 ] || { say "Hace falta macOS 14 (Sonoma) o más nuevo." "macOS 14 (Sonoma) or newer is required."; exit 1; }
# Rosetta 2 (el motor es de Intel): si falta, se instala. macOS pide la contraseña de la Mac en su propia ventana.
if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
  paso "Instalando Rosetta 2 (la Mac te va a pedir tu contraseña)" "Installing Rosetta 2 (your Mac will ask for your password)"
  PROMPT=$(say "VRmin necesita Rosetta 2 de Apple para los juegos de Windows." "VRmin needs Apple's Rosetta 2 to run Windows games.")
  osascript -e "do shell script \"/usr/sbin/softwareupdate --install-rosetta --agree-to-license\" with prompt \"$PROMPT\" with administrator privileges" || true
  arch -x86_64 /usr/bin/true 2>/dev/null || { say "Falta Rosetta 2. Instalala con:  softwareupdate --install-rosetta --agree-to-license" "Rosetta 2 is missing. Install it with:  softwareupdate --install-rosetta --agree-to-license"; exit 1; }
fi
say "macOS y Rosetta 2: OK · datos en: $P" "macOS and Rosetta 2: OK · data in: $P"

[ -x "$WINESERVER" ] && "$WINESERVER" -k 2>/dev/null || true
rm -rf "$P/wine-dxmt" "$P/Wine Devel.app"
# (un Windows de otro Wine 11 de Gcenx se conserva y se actualiza con wineboot -u: no hay que reinstalar Steam)
USADO="$(cat "$WINEPREFIX/.wine_usado" 2>/dev/null || true)"
if [ -d "$WINEPREFIX" ] && [ "$USADO" != "$MARCA_MOTOR" ] && [ "${USADO#gcenx-11}" = "$USADO" ]; then
  say "Borrando un Windows de una versión anterior del motor" "Removing a Windows from an older engine version"
  rm -rf "$WINEPREFIX"
fi

# Un motor de otra versión (por ejemplo Wine 10): se reemplaza. (Se mira el wine de verdad, no wine.sh: wine.sh siempre
# existe y, sin motor, se borraba «el anterior» y no se bajaba el nuevo)
if [ -x "$WINE_REAL" ] && ! grep -q "$ENGINE" "$WINEDIR/version" 2>/dev/null; then
  say "Reemplazando el motor anterior por Wine 11" "Replacing the previous engine with Wine 11"
  rm -rf "$WINEDIR"
fi
if [ ! -x "$WINE_REAL" ]; then
  paso "Descargando el motor Wine 11 (≈170 MB)" "Downloading the Wine 11 engine (≈170 MB)"
  mkdir -p "$MOTOR"
  bajar "$ENGINE_URL" motor.tar.xz "$ENGINE_SHA"
  rm -rf "$MOTOR/tmp" && mkdir -p "$MOTOR/tmp"
  tar -xJf motor.tar.xz -C "$MOTOR/tmp" && rm motor.tar.xz
  mv "$MOTOR/tmp/$ENGINE_APP/Contents/Resources/wine" "$WINEDIR" && rm -rf "$MOTOR/tmp"
  echo "$ENGINE" > "$WINEDIR/version"
fi
if [ ! -d "$FW" ]; then
  paso "Descargando las bibliotecas del motor (≈90 MB)" "Downloading the engine libraries (≈90 MB)"
  bajar "$TPL_URL" plantilla.tar.xz "$TPL_SHA"
  rm -rf "$MOTOR/tmp" && mkdir -p "$MOTOR/tmp"
  tar -xJf plantilla.tar.xz -C "$MOTOR/tmp" "$TPL.app/Contents/Frameworks"
  mv "$MOTOR/tmp/$TPL.app/Contents/Frameworks" "$FW"
  rm -rf "$MOTOR/tmp" plantilla.tar.xz
fi
xattr -dr com.apple.quarantine "$MOTOR" 2>/dev/null || true
echo "Wine: $("$WINE" --version)"

if [ ! -f "$DXMT_DIR/x86_64-unix/winemetal.so" ]; then
  paso "Descargando DXMT (≈20 MB)" "Downloading DXMT (≈20 MB)"
  bajar "$DXMT_URL" dxmt.tar.gz "$DXMT_SHA"
  mkdir -p "$DXMT_DIR" && tar -xzf dxmt.tar.gz -C "$DXMT_DIR" && rm dxmt.tar.gz
fi
# Espera a que terminen los programas de este Windows, hasta $1 segundos (por defecto 120): si alguno sigue abierto
# (Steam, por ejemplo), se cierra, así la instalación nunca queda esperando para siempre
esperar_wine() {
  "$WINESERVER" -w 2>/dev/null & local w=$!
  for _ in $(seq 1 "${1:-120}"); do kill -0 $w 2>/dev/null || return 0; sleep 1; done
  "$WINESERVER" -k 2>/dev/null; sleep 2; kill $w 2>/dev/null; return 0
}
# Lo que haya quedado abierto en este Windows con el motor de antes (Steam, un juego) se cierra antes de tocarlo
if [ -f "$WINEPREFIX/system.reg" ] && [ -x "$WINESERVER" ]; then
  "$WINESERVER" -k 2>/dev/null || true
  esperar_wine 15
fi
paso "Instalando DXMT dentro del motor" "Installing DXMT into the engine"
for f in d3d11.dll dxgi.dll d3d10core.dll winemetal.dll; do
  dst="$WINEDIR/lib/wine/x86_64-windows/$f"
  [ -f "$dst" ] && [ ! -f "$dst.wine-original" ] && cp "$dst" "$dst.wine-original"
  cp "$DXMT_DIR/x86_64-windows/$f" "$dst"
done
cp "$DXMT_DIR/x86_64-unix/winemetal.so" "$WINEDIR/lib/wine/x86_64-unix/winemetal.so"

if [ ! -f "$WINEPREFIX/system.reg" ]; then
  paso "Creando el Windows de QuestVRBridge (1-2 minutos)" "Creating VRmin's Windows (1-2 minutes)"
  R=0; WINEDEBUG=err+all,warn+module "$WINE" wineboot -i > "$P/logs/wineboot.log" 2>&1 || R=$?
  esperar_wine 180
  # Un Windows bien creado tiene un registro de varios MB: si quedó casi vacío, wineboot falló aunque no lo diga
  if [ $R -ne 0 ] || [ "$(stat -f %z "$WINEPREFIX/system.reg" 2>/dev/null || echo 0)" -lt 100000 ]; then
    say "Falló la creación del Windows (código $R). Detalle en logs/wineboot.log:" "Creating the Windows failed (code $R). Details in logs/wineboot.log:"
    tail -40 "$P/logs/wineboot.log"
    exit 1
  fi
  echo "$MARCA_MOTOR" > "$WINEPREFIX/.wine_usado"
elif [ "$(cat "$WINEPREFIX/.wine_usado" 2>/dev/null)" != "$MARCA_MOTOR" ]; then
  paso "Actualizando el Windows al motor nuevo (se conservan Steam y los juegos)" "Updating the Windows to the new engine (Steam and games are kept)"
  "$WINE" wineboot -u > "$P/logs/wineboot.log" 2>&1 || true
  esperar_wine 120
  echo "$MARCA_MOTOR" > "$WINEPREFIX/.wine_usado"
fi

paso "Registrando el runtime OpenXR de QuestVRBridge" "Registering VRmin's OpenXR runtime"
Q="$WINEPREFIX/drive_c/questvrbridge"
mkdir -p "$Q"
cp "$RES/openxr/questvrbridge_openxr.dll" "$RES/openxr/questvrbridge_openxr.json" "$Q/"
"$WINE" reg add 'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ /d 'C:\questvrbridge\questvrbridge_openxr.json' /f
# (y el de 32 bits, para juegos de 32 bits; jugar.sh pone OpenComposite de 32 bits la primera vez)
if [ -f "$RES/openxr/questvrbridge_openxr32.dll" ]; then
  cp "$RES/openxr/questvrbridge_openxr32.dll" "$RES/openxr/questvrbridge_openxr32.json" "$Q/"
  "$WINE" reg add 'HKLM\Software\Wow6432Node\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ /d 'C:\questvrbridge\questvrbridge_openxr32.json' /f && touch "$Q/.runtime32"
fi
esperar_wine 60

paso "OpenComposite (para juegos de SteamVR/OpenVR)" "OpenComposite (for SteamVR/OpenVR games)"
# (el que viene con VRmin Host, compilado por VRmin desde el código de GameNative: ver opencomposite/README.md)
OC="$RES/opencomposite/vrclient_x64.dll"
if [ -f "$OC" ]; then
  mkdir -p "$Q/opencomposite/bin"
  cp "$OC" "$Q/opencomposite/bin/vrclient_x64.dll"
  shasum -a 256 "$OC" | cut -d' ' -f1 > "$Q/opencomposite/.version64"
  U="$WINEPREFIX/drive_c/users/$USER/AppData/Local/openvr"
  mkdir -p "$U"
  cat > "$U/openvrpaths.vrpath" <<'JSON'
{
	"config" : [ "C:\\questvrbridge\\openvr\\config" ],
	"external_drivers" : null,
	"jsonid" : "vrpathreg",
	"log" : [ "C:\\questvrbridge\\openvr\\logs" ],
	"runtime" : [ "C:\\questvrbridge\\opencomposite" ],
	"version" : 1
}
JSON
  say "OpenComposite instalado (los juegos de SteamVR lo usan sin modificar sus archivos)" "OpenComposite installed (SteamVR games use it without changing their files)"
else
  say "AVISO: a VRmin Host le falta OpenComposite. Los juegos de OpenXR andan igual; los de SteamVR/OpenVR no." "WARNING: VRmin Host is missing OpenComposite. OpenXR games still work; SteamVR/OpenVR games don't."
fi

echo
say "Motor listo." "Engine ready."
