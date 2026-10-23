#!/bin/bash
# Instala Steam para Windows (oficial de Valve) dentro del Windows de QuestVRBridge.
set -e
source "$(dirname "$0")/entorno.sh"
cd "$P"
export WINEDEBUG=-all
motor_listo || { say "Primero instalá el motor." "Install the engine first."; exit 1; }
if [ -f "$STEAMDIR/steam.exe" ]; then say "Steam ya está instalado." "Steam is already installed."; exit 0; fi
say "==> Descargando el instalador oficial de Steam" "==> Downloading the official Steam installer"
curl -fL --progress-bar -o SteamSetup.exe "https://cdn.cloudflare.steamstatic.com/client/installer/SteamSetup.exe"
[ "$(head -c 2 SteamSetup.exe)" = "MZ" ] && [ "$(stat -f%z SteamSetup.exe)" -gt 1000000 ] || { say "La descarga no parece válida" "The download doesn't look valid"; exit 1; }
say "==> Instalando Steam" "==> Installing Steam"
"$WINE" SteamSetup.exe /S > "$P/logs/steam_instalacion.log" 2>&1 || true
"$WINESERVER" -w
rm -f SteamSetup.exe
[ -f "$STEAMDIR/steam.exe" ] && say "Steam instalado. La primera vez que lo abras se actualiza solo (varios minutos)." "Steam installed. The first time you open it, it updates itself (several minutes)." \
  || { say "No encontré steam.exe después de instalar (mirá logs/steam_instalacion.log)" "Couldn't find steam.exe after installing (see logs/steam_instalacion.log)"; exit 1; }
