#!/bin/bash
# Prueba otra versión de DXMT dentro del motor, y vuelve a la de siempre.
#   probar_dxmt.sh <dxmt.tar.gz>   instala esa (se guarda una copia de la actual la primera vez)
#   probar_dxmt.sh --volver        vuelve a la que había antes de probar
# Steam y los juegos tienen que estar cerrados.
set -e
source "$(dirname "$0")/entorno.sh"
DLLS="d3d11.dll dxgi.dll d3d10core.dll winemetal.dll"
W64="$WINEDIR/lib/wine/x86_64-windows" UNIX="$WINEDIR/lib/wine/x86_64-unix"
BAK="$P/dxmt/antes-de-probar"
[ -f "$UNIX/winemetal.so" ] || { say "El motor no está instalado." "The engine isn't installed."; exit 1; }
if pgrep -if "steam\.exe|wineserver" >/dev/null; then
  say "Primero cerrá Steam y los juegos (en VRmin Host: Steam › Cerrar)." "First close Steam and the games (in VRmin Host: Steam › Close)."
  exit 1
fi
if [ "$1" = "--volver" ]; then
  [ -f "$BAK/winemetal.so" ] || { say "No hay nada que volver: no se probó otra versión." "Nothing to undo: no other version was tried."; exit 0; }
  for f in $DLLS; do cp "$BAK/$f" "$W64/$f"; done
  cp "$BAK/winemetal.so" "$UNIX/winemetal.so"
  rm -rf "$BAK"
  say "Listo: volvió el DXMT de siempre." "Done: the usual DXMT is back."
  exit 0
fi
[ -f "$1" ] || { say "Uso: probar_dxmt.sh <dxmt.tar.gz> | --volver" "Usage: probar_dxmt.sh <dxmt.tar.gz> | --volver"; exit 1; }
T="$(mktemp -d)"
tar -xzf "$1" -C "$T"
for f in $DLLS; do [ -f "$T/x86_64-windows/$f" ] || { say "Al paquete le falta $f." "The package is missing $f."; rm -rf "$T"; exit 1; }; done
[ -f "$T/x86_64-unix/winemetal.so" ] || { say "Al paquete le falta winemetal.so." "The package is missing winemetal.so."; rm -rf "$T"; exit 1; }
if [ ! -f "$BAK/winemetal.so" ]; then
  mkdir -p "$BAK"
  for f in $DLLS; do cp "$W64/$f" "$BAK/$f"; done
  cp "$UNIX/winemetal.so" "$BAK/winemetal.so"
fi
for f in $DLLS; do cp "$T/x86_64-windows/$f" "$W64/$f"; done
cp "$T/x86_64-unix/winemetal.so" "$UNIX/winemetal.so"
cat "$T/VERSION.txt" 2>/dev/null || true
rm -rf "$T"
say "Listo: abrí un juego para probarlo. Para volver: probar_dxmt.sh --volver" "Done: open a game to try it. To undo: probar_dxmt.sh --volver"
