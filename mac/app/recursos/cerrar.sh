#!/bin/bash
# Cierra Steam y todo lo que corre dentro del Windows de QuestVRBridge.
# Aunque Steam esté trabado: primero se le pide a Wine que cierre todo (con límite de tiempo) y después se
# terminan a la fuerza los procesos que hayan quedado (solo los de este Windows, no el Steam de Mac).
source "$(dirname "$0")/entorno.sh"
if [ -x "$WINESERVER" ]; then
  "$WINESERVER" -k 2>/dev/null &
  K=$!
  for i in $(seq 1 10); do kill -0 $K 2>/dev/null || break; sleep 0.5; done
  kill -9 $K 2>/dev/null
fi
# Procesos de Windows (su nombre empieza con «C:\») y los del motor (Wine)
quedan() {
  ps -axo pid=,command= | awk -v d="$WINEDIR" -v me=$$ '
    { pid = $1; cmd = substr($0, index($0, $2)) }
    pid != me && (index(cmd, d) > 0 || cmd ~ /^[A-Za-z]:\\/) { print pid }'
}
P1=$(quedan)
if [ -n "$P1" ]; then
  kill $P1 2>/dev/null
  sleep 2
  P2=$(quedan)
  [ -n "$P2" ] && kill -9 $P2 2>/dev/null
fi
say "Cerrado." "Closed."
