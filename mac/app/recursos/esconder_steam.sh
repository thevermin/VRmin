#!/bin/bash
# Con la Biblioteca de VRmin, vuelve a esconder la ventana de Steam si aparece en los próximos segundos (al cerrarse un
# juego o SteamVR, Steam la muestra). Lo usa el renderizador cuando ya no queda ningún juego conectado.
source "$(dirname "$0")/entorno.sh"
source "$RES/runtime.sh"
esconder_steam
