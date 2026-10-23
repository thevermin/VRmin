#!/bin/bash
# Junta en una carpeta todo lo que sirve para ver por qué un juego no anda, y la abre:
#   registros_juego.sh APPID
# - el registro de VRmin para ese juego (runtime elegido, avisos, salida del juego si se abrió directo)
# - el registro de nuestro runtime OpenXR y los de OpenComposite / SteamVR
# - los registros del juego de las últimas 3 horas (Unity: Player.log · Unreal: Saved/Logs)
# - la detección y el perfil del juego
source "$(dirname "$0")/entorno.sh"
APPID="$1"
[ -n "$APPID" ] || { say "Falta el número del juego" "Missing the game number"; exit 1; }
D="$P/logs/juegos/$APPID-registros"
rm -rf "$D"; mkdir -p "$D"
C="$WINEPREFIX/drive_c"
cp "$P/logs/juegos/$APPID.log" "$D/vrmin.log" 2>/dev/null
cp "$C/questvrbridge/questvrbridge_log.txt" "$D/runtime_openxr.log" 2>/dev/null
cp "$C/questvrbridge/questvrbridge_log_anterior.txt" "$D/runtime_openxr_anterior.log" 2>/dev/null
cp "$P/logs/steam.log" "$D/steam.log" 2>/dev/null
cp "$P/perfiles/$APPID.json" "$D/perfil.json" 2>/dev/null
/usr/bin/python3 "$RES/vrjuegos.py" detectar "$APPID" > "$D/deteccion.json" 2>&1
# Registros recientes (180 minutos) de OpenComposite, SteamVR y del juego
n=0
while IFS= read -r -d '' f; do
  n=$((n + 1))
  [ $n -gt 40 ] && break
  cp "$f" "$D/$(printf '%02d' $n)_$(basename "$f")"
done < <(find "$C/questvrbridge/openvr" "$C/users" "$C/Program Files (x86)/Steam/logs" "$C/Program Files (x86)/Steam/steamapps/common" \
          -maxdepth 7 -type f \( -iname '*.log' -o -iname 'Player*.log' -o -iname '*.txt' -path '*Logs*' \) -mmin -180 -size -20M -print0 2>/dev/null)
say "Registros juntados en: $D" "Logs collected in: $D"
open "$D"
