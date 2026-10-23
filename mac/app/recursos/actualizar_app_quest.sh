#!/bin/bash
# Instala (o actualiza) VRmin en el visor por el cable USB-C, con adb.
# Uso: actualizar_app_quest.sh <ruta de adb> <url del .apk>
source "$(dirname "$0")/entorno.sh"
unset DYLD_INSERT_LIBRARIES
ADB="$1"; URL="$2"
T="$(mktemp -d)"
say "Bajando la app del Quest…" "Downloading the Quest app…"
curl -fL --retry 3 -o "$T/VRmin.apk" "$URL" || { say "No se pudo bajar. ¿Hay internet?" "Couldn't download it. Is the Mac online?"; rm -rf "$T"; exit 1; }
say "Instalando en el visor (no lo desconectes)…" "Installing on the headset (don't unplug it)…"
OUT="$("$ADB" install -r "$T/VRmin.apk" 2>&1)"
rm -rf "$T"
echo "$OUT" | tail -2
case "$OUT" in
  *Success*) say "Listo: abrí VRmin en el visor (Biblioteca › Orígenes desconocidos)." "Done: open VRmin in the headset (Library › Unknown sources)."; exit 0 ;;
  *INSTALL_FAILED_UPDATE_INCOMPATIBLE*)
    say "La VRmin del visor tiene otra firma. Desinstalala en el visor (Biblioteca › ⋯ › Desinstalar) y tocá «Instalar» de nuevo." \
        "The VRmin on the headset has a different signature. Uninstall it in the headset (Library › ⋯ › Uninstall) and tap “Install” again."; exit 1 ;;
  *) say "No se pudo instalar (mirá el mensaje de arriba)." "Couldn't install it (see the message above)."; exit 1 ;;
esac
