# QuestVRBridge · entorno común (lo usan los demás scripts con "source")
# RES  = archivos de la app (solo lectura, dentro de «VRmin Host.app»)
# P    = datos del usuario (motor, Windows, registros). Lo define la app con QVB_DATA.
RES="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
P="${QVB_DATA:-$HOME/Library/Application Support/QuestVRBridge}"
# Mensajes en el idioma de la app (VRMIN_LANG=es → español; si no, inglés): say "español" "english"
say() { if [ "$VRMIN_LANG" = es ]; then echo "$1"; else echo "${2:-$1}"; fi; }
mkdir -p "$P/logs"
MOTOR="$P/motor"
WINEDIR="$MOTOR/wswine.bundle"
WINE_REAL="$WINEDIR/bin/wine"
WINE="$WINE_REAL"
WINESERVER="$WINEDIR/bin/wineserver"
FW="$MOTOR/Frameworks"
MARCA_MOTOR="gcenx-11.8+dxmt-vrmin1"

export WINEPREFIX="$P/prefijo"
export DYLD_FALLBACK_LIBRARY_PATH="$WINEDIR/lib:$FW:$FW/GStreamer.framework/Versions/1.0/lib:/usr/lib"
export GST_PLUGIN_PATH="$FW/GStreamer.framework/Versions/1.0/lib/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH="$GST_PLUGIN_PATH"
export WINEMSYNC=1
# (MoltenVK, para juegos Vulkan: traduce los shaders en varios núcleos a la vez, menos tirones al aparecer cosas nuevas)
export MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION=1
# La capa de Steam dentro de los juegos (gameoverlayrenderer) traba juegos y a Steam en Wine: desactivada.
# d3d11 incorporada (DXMT) va en los ajustes de Wine y no acá (ajustes_dll): las variables de entorno les ganan a los
# ajustes de cada programa, y Steam necesita uno propio.
if [ -f "$WINEPREFIX/.dll_d3d11" ]; then
  export WINEDLLOVERRIDES="dxgi,d3d10core=b;winemenubuilder.exe=d;gameoverlayrenderer,gameoverlayrenderer64=d"
else
  export WINEDLLOVERRIDES="dxgi,d3d11,d3d10core=b;winemenubuilder.exe=d;gameoverlayrenderer,gameoverlayrenderer64=d"
fi
# Juegos Unity: Mono lee mal el último error de Windows en hilos nuevos (archivos que no existen "existen").
# Este arreglo se carga en Wine y lo corrige sin tocar los juegos (ver arreglo_errores.c). Solo en Wine (por wine.sh):
# puesto para todo, macOS no abría sus propios programas de arm64e (curl, python…) en algunas Mac y el motor no se instalaba
if [ -f "$RES/arreglo_errores.dylib" ] && [ -x "$RES/wine.sh" ]; then
  export WINE_REAL QVB_ARREGLO="$RES/arreglo_errores.dylib"
  WINE="$RES/wine.sh"
fi
STEAMDIR="$WINEPREFIX/drive_c/Program Files (x86)/Steam"
STEAMLIB="$STEAMDIR/steamapps"

motor_listo() { [ -x "$WINE_REAL" ] && [ -f "$WINEDIR/lib/wine/x86_64-unix/winemetal.so" ] && [ -d "$FW" ] \
  && [ "$(cat "$WINEPREFIX/.wine_usado" 2>/dev/null)" = "$MARCA_MOTOR" ]; }
steam_abierto() { pgrep -if "steam\.exe" >/dev/null; }
# Ajustes de DLLs de Wine (una vez por Windows de VRmin): d3d11 incorporada (DXMT) para todos los programas, Steam
# incluido. Steam (de 32 bits) carga vrclient.dll con SteamVR abierto (para sus paneles en el visor); antes eso lo
# trababa dentro de Wine y se le sacaba d3d11 (sin ella no puede cargar vrclient.dll), pero entonces avisaba
# «Shared lib not found (102)» cada vez que buscaba SteamVR. Con el DXMT de VRmin de ahora se le devuelve.
# Vale desde la próxima vez que se abre Steam.
ajustes_dll() {
  [ -f "$WINEPREFIX/system.reg" ] || return 0
  if [ ! -f "$WINEPREFIX/.dll_d3d11" ]; then
    "$WINE" reg add 'HKCU\Software\Wine\DllOverrides' /v d3d11 /t REG_SZ /d builtin /f >/dev/null 2>&1 \
      && touch "$WINEPREFIX/.dll_d3d11" \
      && export WINEDLLOVERRIDES="dxgi,d3d10core=b;winemenubuilder.exe=d;gameoverlayrenderer,gameoverlayrenderer64=d"
  fi
  if [ ! -f "$WINEPREFIX/.steam_con_d3d11" ]; then
    "$WINE" reg delete 'HKCU\Software\Wine\AppDefaults\steam.exe\DllOverrides' /v d3d11 /f >/dev/null 2>&1
    touch "$WINEPREFIX/.steam_con_d3d11"
  fi
  return 0
}
