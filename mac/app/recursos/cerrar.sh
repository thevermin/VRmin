#!/bin/bash
# Cierra Steam y todo lo que corre dentro del Windows de QuestVRBridge.
# Aunque Steam esté trabado: primero se le pide a Wine que cierre todo (con límite de tiempo) y después se
# terminan a la fuerza los procesos que hayan quedado (solo los de este Windows, no el Steam de Mac).
source "$(dirname "$0")/entorno.sh"
# SteamVR primero y por las buenas (ver cerrar_steamvr_bien en runtime.sh): si no, la vez siguiente arranca en modo seguro
if pgrep -f 'vrserver.exe' >/dev/null 2>&1; then source "$RES/runtime.sh"; cerrar_steamvr; fi
if [ -x "$WINESERVER" ]; then
  "$WINESERVER" -k 2>/dev/null &
  K=$!
  for i in $(seq 1 10); do kill -0 $K 2>/dev/null || break; sleep 0.5; done
  kill -9 $K 2>/dev/null
fi
# Procesos de Windows (su nombre empieza con «C:\»), los del motor (Wine) y los «start.exe /exec C:\…» con que Wine
# abre programas (pueden quedar vivos solos, y hacían parecer que Steam seguía abierto)
quedan() {
  ps -axo pid=,command= | awk -v d="$WINEDIR" -v me=$$ '
    { pid = $1; cmd = substr($0, index($0, $2)) }
    pid != me && (index(cmd, d) > 0 || cmd ~ /^[A-Za-z]:\\/ || cmd ~ /^start\.exe \/exec [A-Za-z]:\\/) { print pid }'
}
P1=$(quedan)
if [ -n "$P1" ]; then
  kill $P1 2>/dev/null
  sleep 2
  P2=$(quedan)
  [ -n "$P2" ] && kill -9 $P2 2>/dev/null
fi
say "Cerrado." "Closed."
