#!/bin/bash
# Abre un juego VR:  jugar.sh APPID [argumentos extra]
# 1) decide el runtime y los argumentos del juego (vrjuegos.py: tu elección > lo conocido > lo detectado)
# 2) deja el Windows de VRmin apuntando a ese runtime (runtime.sh)
# 3) lo abre por Steam (lo normal) o directo (para probar con variables de entorno y diagnóstico)
# Todo queda en logs/juegos/APPID.log
source "$(dirname "$0")/entorno.sh"
source "$RES/runtime.sh"
cd "$P"
APPID="$1"; shift
[ -n "$APPID" ] || { say "Falta el número del juego" "Missing the game number"; exit 1; }
motor_listo || { say "Primero instalá el motor." "Install the engine first."; exit 1; }
steam_abierto || { say "Steam no está abierto. Abrilo y esperá a ver tu biblioteca." "Steam isn't open. Open it and wait until you see your library."; exit 1; }

PERFIL="$(/usr/bin/python3 "$RES/vrjuegos.py" resolver "$APPID")" || exit 1
eval "$PERFIL"

mkdir -p "$P/logs/juegos"
LOG="$P/logs/juegos/$APPID.log"
{
  echo
  echo "======== $(date '+%Y-%m-%d %H:%M:%S') · $NOMBRE ($APPID)"
  say "Detectado: $DETECTADO" "Detected: $DETECTADO"
  [ -n "$SUGERENCIAS" ] && say "Sugerencias: $SUGERENCIAS" "Suggestions: $SUGERENCIAS"
  say "Runtime: $RUNTIME_NOMBRE (elegido: $PEDIDO) · abrir: $LANZAR" "Runtime: $RUNTIME_NOMBRE (chosen: $PEDIDO) · launch: $LANZAR"
  [ -n "$AVISO" ] && say "AVISO: $AVISO" "WARNING: $AVISO"
  [ ${#ARGS[@]} -gt 0 ] && say "Argumentos: ${ARGS[*]}" "Arguments: ${ARGS[*]}"
} | tee -a "$LOG"

# Ajustes vacíos (0 bytes): pasa si el juego se cerró a la fuerza mientras los guardaba, y algunos juegos (Beat Saber)
# quedan en negro al leerlos. Se apartan (no se borran) para que el juego cree unos nuevos.
find "$WINEPREFIX/drive_c/users/"*/AppData/LocalLow -maxdepth 3 -type f \( -name "settings.ini" -o -name "settings.cfg" -o -name "*.settings" \) -size 0 2>/dev/null |
  while read -r f; do mv "$f" "$f.vacio.$(date +%s)" && say "Ajustes vacíos apartados: $f" "Empty settings moved aside: $f" | tee -a "$LOG"; done

# Siempre la última versión de nuestro runtime
mkdir -p "$WINEPREFIX/drive_c/questvrbridge"
cp "$RES/openxr/questvrbridge_openxr.dll" "$RES/openxr/questvrbridge_openxr.json" "$WINEPREFIX/drive_c/questvrbridge/"
[ -f "$RES/openxr/questvrbridge_openxr32.dll" ] && cp "$RES/openxr/questvrbridge_openxr32.dll" "$RES/openxr/questvrbridge_openxr32.json" "$WINEPREFIX/drive_c/questvrbridge/"
preparar_runtime "$RUNTIME" 2>&1 | tee -a "$LOG"
# (OpenComposite: el grip de Touch como botón donde el juego lo usa para agarrar; OpenComposite busca esos controles en
#  la carpeta desde donde se abre el juego)
[ "$RUNTIME" = "openvr" ] && /usr/bin/python3 "$RES/vrjuegos.py" controles_oc "$CARPETA" "$CARPETA/$(dirname "${EXE:-.}")" 2>&1 | tee -a "$LOG"
gpu_luid_dxmt 2>&1 | tee -a "$LOG"

if [ "$LANZAR" = "directo" ] && [ -n "$EXE" ] && [ -f "$CARPETA/$EXE" ]; then
  # Directo: el juego es hijo de este proceso, así que recibe las variables de entorno y el diagnóstico.
  # (Steam tiene que estar abierto igual; el juego se conecta a él.)
  say "Abriendo directo: $EXE" "Opening directly: $EXE" | tee -a "$LOG"
  [ -f "$CARPETA/$(dirname "$EXE")/steam_appid.txt" ] || echo "$APPID" > "$CARPETA/$(dirname "$EXE")/steam_appid.txt"
  if [ "$DIAGNOSTICO" = "1" ]; then
    export WINEDEBUG=err+all,warn+module,+loaddll
    export XR_LOADER_DEBUG=all
    say "Diagnóstico: WINEDEBUG=$WINEDEBUG XR_LOADER_DEBUG=all" "Diagnostics: WINEDEBUG=$WINEDEBUG XR_LOADER_DEBUG=all" | tee -a "$LOG"
  else
    export WINEDEBUG=-all,err+module,err+loaddll
  fi
  export DXMT_LOG_PATH="$P/logs"
  for e in "${ENVS[@]}"; do export "$e"; echo "Variable: $e" >> "$LOG"; done
  cd "$CARPETA/$(dirname "$EXE")"
  "$WINE" "$CARPETA/$EXE" "${ARGS[@]}" "$@" >> "$LOG" 2>&1 < /dev/null &
  disown
else
  [ "$LANZAR" = "directo" ] && say "No encontré el ejecutable ($EXE): lo abro por Steam" "Couldn't find the executable ($EXE): opening it through Steam" | tee -a "$LOG"
  [ ${#ENVS[@]} -gt 0 ] && say "(las variables de entorno solo se aplican al abrir directo)" "(environment variables only apply when opening directly)" | tee -a "$LOG"
  say "Pidiendo a Steam que abra el juego (si pregunta cómo abrirlo, elegí SteamVR / realidad virtual)" "Asking Steam to open the game (if it asks how to launch it, choose SteamVR / virtual reality)" | tee -a "$LOG"
  export WINEDEBUG=-all
  "$WINE" "C:\\Program Files (x86)\\Steam\\steam.exe" -applaunch "$APPID" "${ARGS[@]}" "$@" >> "$P/logs/steam.log" 2>&1 &
fi
# (para el modo automático: si no llega ninguna imagen, la próxima vez se prueba el otro runtime)
/usr/bin/python3 "$RES/vrjuegos.py" intento "$APPID" "$RUNTIME" > /dev/null 2>&1
sleep 3
say "Registros: logs/juegos/$APPID.log · runtime: prefijo/drive_c/questvrbridge/questvrbridge_log.txt" "Logs: logs/juegos/$APPID.log · runtime: prefijo/drive_c/questvrbridge/questvrbridge_log.txt"
