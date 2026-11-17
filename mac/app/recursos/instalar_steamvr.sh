#!/bin/bash
# Le pide al Steam de VRmin que instale SteamVR (lo abre primero si está cerrado). Steam muestra su ventana de
# instalación en la Mac; SteamVR queda en la biblioteca, en Herramientas.
source "$(dirname "$0")/entorno.sh"
cd "$P"
[ -f "$STEAMLIB/common/SteamVR/bin/win64/vrstartup.exe" ] && { say "SteamVR ya está instalado." "SteamVR is already installed."; exit 0; }
if ! steam_abierto; then
  bash "$RES/abrir_steam.sh" || exit 1
  for i in $(seq 1 75); do steam_abierto && break; sleep 2; done
  steam_abierto || { say "Steam no terminó de abrir: probá de nuevo en un momento." "Steam didn't finish opening: try again in a moment."; exit 1; }
  say "Steam abierto, esperando que termine de cargar…" "Steam is open, waiting for it to finish loading…"
  sleep 25
fi
WINEDEBUG=-all "$WINE" "C:\\Program Files (x86)\\Steam\\steam.exe" "steam://install/250820" >/dev/null 2>&1 < /dev/null &
disown
say "Steam muestra la instalación de SteamVR en la Mac: tocá «Instalar» ahí (baja unos 5 GB)." \
    "Steam shows SteamVR's installation on the Mac: click “Install” there (about 5 GB)."
