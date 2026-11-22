#!/bin/bash
# Esconde la ventana de Steam (Steam sigue abierto de fondo, para la sesión, las descargas y los juegos).
# La de iniciar sesión no se toca. Ver steam/ocultar_steam.c
source "$(dirname "$0")/entorno.sh"
steam_abierto || exit 0
# («Mostrar Steam» la dejó a la vista: hasta «Ocultar Steam», nada la esconde solo)
[ -f "$P/.steam_mostrada" ] && exit 0
WINEDEBUG=-all "$WINE" "$RES/steam/ocultar_steam.exe" >/dev/null 2>&1 < /dev/null
say "Ventana de Steam escondida (Steam sigue abierto de fondo)." "Steam's window hidden (Steam stays open in the background)."
