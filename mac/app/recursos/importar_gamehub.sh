#!/bin/bash
# Trae un juego ya descargado en GameHub a la biblioteca de Steam de QuestVRBridge
# (copia instantánea de APFS: no ocupa espacio extra). Uso: importar_gamehub.sh APPID
set -e
source "$(dirname "$0")/entorno.sh"
cd "$P"
APPID="$1"
SRC="$HOME/GameHub/steamapps"
ACF="$SRC/appmanifest_$APPID.acf"
[ -f "$ACF" ] || { say "No encontré ese juego en GameHub" "Couldn't find that game in GameHub"; exit 1; }
[ -f "$STEAMDIR/steam.exe" ] || { say "Primero instalá Steam." "Install Steam first."; exit 1; }
steam_abierto && { say "Cerrá Steam primero (botón \"Cerrar todo\") para que vea el juego nuevo al abrirlo." "Close Steam first (\"Close\" button) so it sees the new game when it opens."; exit 1; }
DIR=$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)".*/\1/p' "$ACF" | head -1)
[ -n "$DIR" ] && [ -d "$SRC/common/$DIR" ] || { say "Faltan los archivos del juego en GameHub" "The game's files are missing in GameHub"; exit 1; }
DST="$STEAMLIB/common/$DIR"
mkdir -p "$STEAMLIB/common"
if [ ! -d "$DST" ]; then
  say "Copiando \"$DIR\" (copia instantánea)..." "Copying \"$DIR\" (instant copy)..."
  cp -cR "$SRC/common/$DIR" "$DST" 2>/dev/null || cp -R "$SRC/common/$DIR" "$DST"
fi
# Si en GameHub habíamos modificado algo (guardando el original), se restaura el original
find "$DST" -name '*.original' -print0 | while IFS= read -r -d '' f; do mv -f "$f" "${f%.original}"; say "Restaurado: ${f#$DST/}" "Restored: ${f#$DST/}"; done
find "$DST" \( -name 'questvrbridge_log.txt' -o -name 'unity_actual.log' -o -name 'unity_anterior.log' -o -name 'unity_crash_*.log' \) -delete
cp "$ACF" "$STEAMLIB/"
say "Listo: \"$DIR\" ya está en tu biblioteca de Steam de QuestVRBridge." "Done: \"$DIR\" is now in your VRmin Steam library."
