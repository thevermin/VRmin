#!/bin/bash
# Instala el "motor" de QuestVRBridge en la carpeta de datos:
#  - Wine 11 (compilación para Mac de Gcenx, la que distribuye WineHQ) + bibliotecas de Sikarugir (GStreamer…)
#    (Wine 11: texturas compartidas entre programas, para la copia directa de DXMT. El Wine 11 de Sikarugir
#     no puede abrir programas nuevos: wineboot falla con «failed to start wineboot 1», probado en CI)
#  - DXMT (DirectX 11 -> Metal): la versión de notpop (github.com/notpop/dxmt, Wine 11 Metal view fix),
#    compilada por ramiabih/play-windows-steam-on-mac. El DXMT oficial v0.80 no puede crear la ventana de Metal
#    con el Wine de Gcenx («Failed to create metal view»: los juegos se cierran a los 5 segundos)
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
DXMT_URL="https://github.com/ramiabih/play-windows-steam-on-mac/releases/download/dxmt-fork-v1/dxmt-fork-macos-arm64.tar.gz"
DXMT_SHA="51043dd4cf585d52b1415c452052a0953dc01f9f9663ae0c3d6984907c25fab5"
DXMT_DIR="$P/dxmt/fork-v1"
OC_URL="https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr"
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
arch -x86_64 /usr/bin/true 2>/dev/null || { say "Falta Rosetta 2. Instalala con:  softwareupdate --install-rosetta --agree-to-license" "Rosetta 2 is missing. Install it with:  softwareupdate --install-rosetta --agree-to-license"; exit 1; }
say "macOS y Rosetta 2: OK · datos en: $P" "macOS and Rosetta 2: OK · data in: $P"

[ -x "$WINESERVER" ] && "$WINESERVER" -k 2>/dev/null || true
rm -rf "$P/wine-dxmt" "$P/Wine Devel.app"
# (un Windows de otro Wine 11 de Gcenx se conserva y se actualiza con wineboot -u: no hay que reinstalar Steam)
USADO="$(cat "$WINEPREFIX/.wine_usado" 2>/dev/null || true)"
if [ -d "$WINEPREFIX" ] && [ "$USADO" != "$MARCA_MOTOR" ] && [ "${USADO#gcenx-11}" = "$USADO" ]; then
  say "Borrando un Windows de una versión anterior del motor" "Removing a Windows from an older engine version"
  rm -rf "$WINEPREFIX"
fi

# Un motor de otra versión (por ejemplo Wine 10): se reemplaza
if [ -x "$WINE" ] && ! grep -q "$ENGINE" "$WINEDIR/version" 2>/dev/null; then
  say "Reemplazando el motor anterior por Wine 11" "Replacing the previous engine with Wine 11"
  rm -rf "$WINEDIR"
fi
if [ ! -x "$WINE" ]; then
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
  paso "Descargando DXMT (≈40 MB)" "Downloading DXMT (≈40 MB)"
  bajar "$DXMT_URL" dxmt.tar.gz "$DXMT_SHA"
  mkdir -p "$DXMT_DIR" && tar -xzf dxmt.tar.gz -C "$DXMT_DIR" && rm dxmt.tar.gz
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
  "$WINESERVER" -w || true
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
  "$WINESERVER" -w || true
  echo "$MARCA_MOTOR" > "$WINEPREFIX/.wine_usado"
fi

paso "Registrando el runtime OpenXR de QuestVRBridge" "Registering VRmin's OpenXR runtime"
Q="$WINEPREFIX/drive_c/questvrbridge"
mkdir -p "$Q"
cp "$RES/openxr/questvrbridge_openxr.dll" "$RES/openxr/questvrbridge_openxr.json" "$Q/"
"$WINE" reg add 'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ /d 'C:\questvrbridge\questvrbridge_openxr.json' /f
"$WINESERVER" -w

paso "OpenComposite (para juegos de SteamVR/OpenVR)" "OpenComposite (for SteamVR/OpenVR games)"
OC="$P/descargas/opencomposite_x64.dll"
mkdir -p "$P/descargas"
if ! file -b "$OC" 2>/dev/null | grep -qE 'PE32\+.*DLL'; then
  curl -fL --progress-bar -o "$OC" "$OC_URL" || true
fi
if file -b "$OC" 2>/dev/null | grep -qE 'PE32\+.*DLL'; then
  mkdir -p "$Q/opencomposite/bin"
  cp "$OC" "$Q/opencomposite/bin/vrclient_x64.dll"
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
  say "AVISO: no pude descargar OpenComposite. Los juegos de OpenXR andan igual; los de SteamVR/OpenVR no." "WARNING: couldn't download OpenComposite. OpenXR games still work; SteamVR/OpenVR games don't."
  rm -f "$OC"
fi

echo
say "Motor listo." "Engine ready."
