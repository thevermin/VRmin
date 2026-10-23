#!/bin/bash
# «Algo anda mal»: junta en un .zip del Escritorio lo que hace falta para ver qué pasó, y lo muestra en Finder.
#   reporte.sh [archivo con el registro de VRmin Host]
# No incluye claves ni la lista de Quest emparejados.
source "$(dirname "$0")/entorno.sh"
STAMP=$(date +"%Y-%m-%d %H.%M.%S")
D="$P/logs/reporte"
rm -rf "$D"; mkdir -p "$D/juegos" "$D/perfiles"
C="$WINEPREFIX/drive_c"
[ -n "$1" ] && cp "$1" "$D/vrmin_host.log" 2>/dev/null
# (el del servidor puede ser enorme: alcanza con el final)
tail -c 3000000 "$P/logs/servidor.log" > "$D/servidor.log" 2>/dev/null
tail -c 2000000 "$P/logs/steam.log" > "$D/steam.log" 2>/dev/null
cp "$P/logs/vigia.log" "$D/" 2>/dev/null
cp "$P/logs/juegos/"*.log "$D/juegos/" 2>/dev/null
cp "$C/questvrbridge/questvrbridge_log.txt" "$D/runtime_openxr.log" 2>/dev/null
cp "$P/perfiles_juegos.json" "$D/" 2>/dev/null
cp "$P/perfiles/"*.json "$D/perfiles/" 2>/dev/null
cp -R "$P/perfiles/detectado" "$D/perfiles/" 2>/dev/null
{
  echo "VRmin · $STAMP"
  sw_vers 2>/dev/null
  echo "Mac: $(sysctl -n hw.model 2>/dev/null) · $(sysctl -n machdep.cpu.brand_string 2>/dev/null) · $(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1073741824 )) GB"
  df -h "$HOME" 2>/dev/null | tail -1
  echo "VRmin Host: $(defaults read "$(dirname "$RES")/../Info" CFBundleShortVersionString 2>/dev/null)"
  pgrep -fl "steam.exe|wine|QuestRenderer|server.py" 2>/dev/null | cut -c1-200
} > "$D/sistema.txt"
OUT="$HOME/Desktop/VRmin reporte $STAMP.zip"
(cd "$D" && /usr/bin/zip -qr "$OUT" .)
if [ -f "$OUT" ]; then
  say "Reporte listo en el Escritorio: $(basename "$OUT"). Mandalo por correo o adjuntalo donde pidas ayuda." \
      "Report ready on the Desktop: $(basename "$OUT"). Email it or attach it wherever you ask for help."
  open -R "$OUT"
else
  say "No se pudo armar el reporte. Los registros están en: $D" "Couldn't build the report. The logs are in: $D"
  open "$D"
fi
