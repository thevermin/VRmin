#!/bin/bash
# Cierra solo el juego abierto (no Steam): primero le pide que se cierre y, si no responde, lo termina.
#   cerrar_juego.sh            cualquier juego abierto (SteamVR no cuenta; si está abierto solo, sin juego, se cierra él)
#   cerrar_juego.sh <carpeta>  solo ese juego (su carpeta en steamapps/common)
source "$(dirname "$0")/entorno.sh"
# (por Steam la ruta es de Windows, con «\»; abierto directo es de la Mac, con «/»)
export FILTRO="steamapps\\common\\${1:-}" FILTRO2="steamapps/common/${1:-}"   # (awk lo lee de ENVIRON: sin procesar las barras)
# Sin carpeta, SteamVR (también en steamapps/common) no es un juego: con un juego abierto desde su menú se cierra el
# juego y se vuelve a la sala de SteamVR, como en un Valve Index
export SIN=""; [ -z "${1:-}" ] && SIN="steamvr"
juegos() { ps -axo pid=,command= | awk 'BEGIN { f = tolower(ENVIRON["FILTRO"]); g = tolower(ENVIRON["FILTRO2"]); s = ENVIRON["SIN"] }
  { cmd = tolower(substr($0, index($0, $2))) }
  (index(cmd, f) || index(cmd, g)) && !(s != "" && (index(cmd, f s "\\") || index(cmd, g s "/"))) { print $1 "\t" cmd }'; }
juego() { juegos | cut -f1; }
P1=$(juego)
if [ -z "$P1" ]; then
  # Sin juego, pero con SteamVR abierto solo (abrir_steamvr.sh): «Cerrar» desde el visor lo cierra a él
  if [ -z "${1:-}" ] && pgrep -f 'vrserver\.exe' >/dev/null; then
    source "$RES/runtime.sh"
    cerrar_steamvr
    ( esconder_steam ) > /dev/null 2>&1 < /dev/null &
    say "SteamVR cerrado." "SteamVR closed."
    exit 0
  fi
  say "No hay ningún juego abierto." "No game is open."; exit 0
fi
# El nombre del programa (…\Juego.exe) para pedirle a Windows que lo cierre como corresponde
juegos | cut -f2- | grep -o '[^\\/]*\.exe' | sort -u | while IFS= read -r exe; do
  WINEDEBUG=-all "$WINE" taskkill /IM "$exe" >/dev/null 2>&1 &
done
for i in $(seq 1 8); do sleep 0.5; [ -z "$(juego)" ] && break; done
P2=$(juego)
[ -n "$P2" ] && kill $P2 2>/dev/null && sleep 2
P3=$(juego)
[ -n "$P3" ] && kill -9 $P3 2>/dev/null
say "Juego cerrado." "Game closed."
