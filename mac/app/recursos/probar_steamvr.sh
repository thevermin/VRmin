#!/bin/bash
# Prueba si el SteamVR de Windows arranca dentro del Windows de VRmin, antes de hacerle un driver:
# lo abre con su visor de prueba («null»: dibuja en una ventana común) durante un rato y junta sus registros.
#   probar_steamvr.sh [segundos]   (por defecto 90)
# Hace falta SteamVR instalado en el Steam de VRmin y Steam abierto. Deja todo como estaba: los ajustes de SteamVR
# vuelven a los de antes y el próximo juego vuelve a elegir su runtime.
source "$(dirname "$0")/entorno.sh"
source "$RES/runtime.sh"
cd "$P"
SEG="${1:-90}"
STEAM="$WINEPREFIX/drive_c/Program Files (x86)/Steam"
SVR="$STEAM/steamapps/common/SteamVR"
OUT="$P/logs/steamvr_prueba"
[ -f "$SVR/bin/win64/vrserver.exe" ] || { say "SteamVR no está instalado en el Steam de VRmin: buscalo en la biblioteca de Steam e instalalo." "SteamVR isn't installed in VRmin's Steam: find it in Steam's library and install it."; exit 1; }
steam_abierto || { say "Steam no está abierto. Abrilo y esperá a ver tu biblioteca." "Steam isn't open. Open it and wait until you see your library."; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"

# OpenVR → SteamVR (sin drivers externos) y OpenXR → SteamVR
escribir_vrpath "$STEAMVR_WIN"
openxr_activo "$STEAMVR_WIN\\steamxr_win64.json" > /dev/null 2>&1
rm -f "$Q/.runtime"   # (el próximo juego vuelve a poner su runtime)

# Visor de prueba de SteamVR, sin SteamVR Home
# (openvrpaths.vrpath pone la configuración y los registros de SteamVR en C:\questvrbridge\openvr)
mkdir -p "$Q/openvr/config" "$Q/openvr/logs"
CFG="$Q/openvr/config/steamvr.vrsettings"
[ -f "$CFG" ] && cp "$CFG" "$OUT/steamvr.vrsettings.antes"
cat > "$CFG" <<'JSON'
{
   "driver_null" : {
      "enable" : true,
      "renderWidth" : 1440,
      "renderHeight" : 1600,
      "windowWidth" : 1440,
      "windowHeight" : 800,
      "displayFrequency" : 72
   },
   "steamvr" : {
      "requireHmd" : true,
      "forcedDriver" : "null",
      "activateMultipleDrivers" : true,
      "enableHomeApp" : false,
      "showMirrorView" : false,
      "startMonitorFromAppLaunch" : true
   }
}
JSON

say "Abriendo SteamVR ($SEG segundos)…" "Opening SteamVR ($SEG seconds)…"
export WINEDEBUG=err+all,warn+module
export DXMT_LOG_PATH="$OUT"
"$WINE" "$SVR/bin/win64/vrstartup.exe" > "$OUT/wine.log" 2>&1 &
sleep "$SEG"
pgrep -fl 'vrserver|vrcompositor|vrmonitor|vrwebhelper|vrdashboard' | cut -c1-160 > "$OUT/procesos.txt"
say "Procesos de SteamVR que quedaron abiertos:" "SteamVR processes still open:"; cat "$OUT/procesos.txt"

# Cerrar SteamVR y juntar sus registros
for p in vrmonitor vrcompositor vrdashboard vrwebhelper vrserver vrstartup; do pkill -f "$p.exe" 2>/dev/null; done
sleep 3
cp "$Q/openvr/logs/"*.txt "$STEAM/logs/"vr*.txt "$OUT/" 2>/dev/null
if [ -f "$OUT/steamvr.vrsettings.antes" ]; then cp "$OUT/steamvr.vrsettings.antes" "$CFG"; else rm -f "$CFG"; fi
tail -c 2000000 "$OUT/wine.log" > "$OUT/wine_final.log"
say "Listo. Registros en: $OUT" "Done. Logs in: $OUT"
grep -h -i 'compositor\|error\|failed\|fatal' "$OUT"/vrserver.txt "$OUT"/vrcompositor.txt 2>/dev/null | tail -20
