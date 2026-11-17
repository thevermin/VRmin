#!/bin/bash
# Le pide al Steam de VRmin que instale un juego (desde la Tienda de VRmin Host): instalar_juego.sh <appid> [nombre].
# Con «desinstalar» al final, que lo desinstale (Steam pregunta antes).
# Lo abre primero si está cerrado. Steam muestra su ventana de instalación en la Mac (y, si el juego no es tuyo, su
# página para comprarlo); el juego aparece en la Biblioteca apenas empieza a bajar.
source "$(dirname "$0")/entorno.sh"
cd "$P"
ID="$1"; NOMBRE="${2:-$1}"; QUE="${3:-install}"; [ "$QUE" = desinstalar ] && QUE=uninstall
[[ "$ID" =~ ^[0-9]+$ ]] || { say "Falta el número del juego." "The game's number is missing."; exit 1; }
if ! steam_abierto; then
  bash "$RES/abrir_steam.sh" || exit 1
  for i in $(seq 1 75); do steam_abierto && break; sleep 2; done
  steam_abierto || { say "Steam no terminó de abrir: probá de nuevo en un momento." "Steam didn't finish opening: try again in a moment."; exit 1; }
  say "Steam abierto, esperando que termine de cargar…" "Steam is open, waiting for it to finish loading…"
  sleep 25
fi
WINEDEBUG=-all "$WINE" "C:\\Program Files (x86)\\Steam\\steam.exe" "steam://$QUE/$ID" >/dev/null 2>&1 < /dev/null &
disown
if [ "$QUE" = uninstall ]; then
  say "Steam pregunta en la Mac si desinstalar $NOMBRE." "Steam asks on the Mac whether to uninstall $NOMBRE."
else
  say "Steam muestra la instalación de $NOMBRE en la Mac: tocá «Instalar» ahí." \
      "Steam shows $NOMBRE's installation on the Mac: click “Install” there."
fi
