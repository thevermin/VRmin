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
WINE="$WINEDIR/bin/wine"
WINESERVER="$WINEDIR/bin/wineserver"
FW="$MOTOR/Frameworks"
MARCA_MOTOR="gcenx-11.8+dxmt-notpop1"

export WINEPREFIX="$P/prefijo"
export DYLD_FALLBACK_LIBRARY_PATH="$WINEDIR/lib:$FW:$FW/GStreamer.framework/Versions/1.0/lib:/usr/lib"
export GST_PLUGIN_PATH="$FW/GStreamer.framework/Versions/1.0/lib/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH="$GST_PLUGIN_PATH"
export WINEMSYNC=1
# (MoltenVK, para juegos Vulkan: traduce los shaders en varios núcleos a la vez, menos tirones al aparecer cosas nuevas)
export MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION=1
# La capa de Steam dentro de los juegos (gameoverlayrenderer) traba juegos y a Steam en Wine: desactivada
export WINEDLLOVERRIDES="dxgi,d3d11,d3d10core=b;winemenubuilder.exe=d;gameoverlayrenderer,gameoverlayrenderer64=d"
# Juegos Unity: Mono lee mal el último error de Windows en hilos nuevos (archivos que no existen "existen").
# Este arreglo se carga en Wine y lo corrige sin tocar los juegos (ver arreglo_errores.c)
[ -f "$RES/arreglo_errores.dylib" ] && export DYLD_INSERT_LIBRARIES="$RES/arreglo_errores.dylib"
STEAMDIR="$WINEPREFIX/drive_c/Program Files (x86)/Steam"
STEAMLIB="$STEAMDIR/steamapps"

motor_listo() { [ -x "$WINE" ] && [ -f "$WINEDIR/lib/wine/x86_64-unix/winemetal.so" ] && [ -d "$FW" ] \
  && [ "$(cat "$WINEPREFIX/.wine_usado" 2>/dev/null)" = "$MARCA_MOTOR" ]; }
steam_abierto() { pgrep -if "steam\.exe" >/dev/null; }
