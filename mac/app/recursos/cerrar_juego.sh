#!/bin/bash
# Cierra solo el juego abierto (no Steam): primero le pide que se cierre y, si no responde, lo termina.
#   cerrar_juego.sh            cualquier juego abierto
#   cerrar_juego.sh <carpeta>  solo ese juego (su carpeta en steamapps/common)
source "$(dirname "$0")/entorno.sh"
# (por Steam la ruta es de Windows, con «\»; abierto directo es de la Mac, con «/»)
export FILTRO="steamapps\\common\\${1:-}" FILTRO2="steamapps/common/${1:-}"   # (awk lo lee de ENVIRON: sin procesar las barras)
juego() { ps -axo pid=,command= | awk 'BEGIN { f = ENVIRON["FILTRO"]; g = ENVIRON["FILTRO2"] } { cmd = tolower(substr($0, index($0, $2))) } index(cmd, tolower(f)) || index(cmd, tolower(g)) { print $1 }'; }
P1=$(juego)
[ -z "$P1" ] && { say "No hay ningún juego abierto." "No game is open."; exit 0; }
# El nombre del programa (…\Juego.exe) para pedirle a Windows que lo cierre como corresponde
ps -axo command= | awk 'BEGIN { f = ENVIRON["FILTRO"]; g = ENVIRON["FILTRO2"] } index(tolower($0), tolower(f)) || index(tolower($0), tolower(g))' | grep -o '[^\\/]*\.exe' | sort -u | while IFS= read -r exe; do
  WINEDEBUG=-all "$WINE" taskkill /IM "$exe" >/dev/null 2>&1 &
done
for i in $(seq 1 8); do sleep 0.5; [ -z "$(juego)" ] && break; done
P2=$(juego)
[ -n "$P2" ] && kill $P2 2>/dev/null && sleep 2
P3=$(juego)
[ -n "$P3" ] && kill -9 $P3 2>/dev/null
say "Juego cerrado." "Game closed."
