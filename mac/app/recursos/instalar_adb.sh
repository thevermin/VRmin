#!/bin/bash
# Modo Centinela: instala las herramientas de Android (adb, de Google) para que el visor no apague la pantalla
# cuando no lo tenés puesto. Necesita el modo desarrollador del Quest y el cable USB-C.
source "$(dirname "$0")/entorno.sh"
DEST="$P/herramientas"
URL="https://dl.google.com/android/repository/platform-tools-latest-darwin.zip"
mkdir -p "$DEST"
say "Bajando las herramientas de Android de Google (≈15 MB)…" "Downloading Google's Android platform tools (≈15 MB)…"
TMP="$(mktemp -d)"
if ! curl -fL --retry 3 -o "$TMP/pt.zip" "$URL"; then
  say "No se pudieron bajar. ¿Hay internet?" "Couldn't download them. Is the Mac online?"; rm -rf "$TMP"; exit 1
fi
rm -rf "$DEST/platform-tools"
unzip -q "$TMP/pt.zip" -d "$DEST" || { say "El archivo bajado está dañado" "The downloaded file is damaged"; rm -rf "$TMP"; exit 1; }
rm -rf "$TMP"
ADB="$DEST/platform-tools/adb"
xattr -dr com.apple.quarantine "$DEST/platform-tools" 2>/dev/null
[ -x "$ADB" ] || { say "Falta adb después de instalar" "adb is missing after installing"; exit 1; }
"$ADB" start-server >/dev/null 2>&1
say "Listo: $("$ADB" version | head -1)" "Done: $("$ADB" version | head -1)"
ESTADO="$("$ADB" devices | awk 'NR>1 && $2!="" {print $2; exit}')"
case "$ESTADO" in
  device) say "El Quest ya está autorizado: el Modo Centinela va a mantener la pantalla encendida mientras jugás." \
              "The Quest is already authorized: Watch Mode will keep its display on while you play." ;;
  unauthorized) say "Ponete el visor y tocá «Permitir» en el aviso de depuración USB (marcá «Permitir siempre desde esta computadora»)." \
                    "Put on the headset and tap “Allow” on the USB debugging prompt (check “Always allow from this computer”)." ;;
  *) say "Conectá el Quest con el cable USB-C (con el modo desarrollador activado) y aceptá el aviso en el visor." \
         "Connect the Quest with the USB-C cable (with developer mode on) and accept the prompt in the headset." ;;
esac
exit 0
