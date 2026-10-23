#!/bin/bash
# Actualiza VRmin Host: baja la versión nueva, revisa que la firma sea la de VRmin y la pone en lugar de esta
# apenas esta se cierra (después la abre).
# Uso: actualizar_host.sh <url del .zip> <ruta de «VRmin Host.app»> <pid de la app>
source "$(dirname "$0")/entorno.sh"
unset DYLD_INSERT_LIBRARIES
URL="$1"; APP="$2"; PID="$3"
PAGINA="https://github.com/thevermin/VRmin/releases/latest"
if [ ! -w "$(dirname "$APP")" ] || [[ "$APP" == */AppTranslocation/* ]]; then
  say "No puedo reemplazar la app donde está. Movela a Aplicaciones, o bajá la nueva de $PAGINA" \
      "I can't replace the app where it is. Move it to Applications, or download the new one from $PAGINA"
  exit 1
fi
T="$(mktemp -d)"
falla() { say "$1" "$2"; rm -rf "$T"; exit 1; }
say "Bajando la versión nueva…" "Downloading the new version…"
curl -fL --retry 3 -o "$T/n.zip" "$URL" || falla "No se pudo bajar. ¿Hay internet?" "Couldn't download it. Is the Mac online?"
ditto -x -k "$T/n.zip" "$T/x" || falla "El archivo bajado está dañado" "The downloaded file is damaged"
NUEVA="$(ls -d "$T/x"/*.app 2>/dev/null | head -1)"
[ -d "$NUEVA" ] || falla "El archivo bajado no tiene la app" "The downloaded file doesn't contain the app"
# La firma: válida, de VRmin y (si esta app tiene el certificado de VRmin) con el mismo certificado
codesign --verify --strict "$NUEVA" 2>/dev/null || falla "La app bajada no tiene una firma válida" "The downloaded app doesn't have a valid signature"
[ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$NUEVA/Contents/Info.plist" 2>/dev/null)" = "local.questvrbridge.app" ] \
  || falla "La app bajada no es VRmin Host" "The downloaded app isn't VRmin Host"
REQ="$(codesign -d -r- "$APP" 2>&1 | sed -n 's/^designated => //p')"
if [[ "$REQ" == *certificate* ]]; then
  codesign --verify -R="=$REQ" "$NUEVA" 2>/dev/null || falla "La app bajada no está firmada por VRmin" "The downloaded app isn't signed by VRmin"
fi
say "Listo: VRmin Host se cierra y se abre la versión nueva." "Done: VRmin Host closes and the new version opens."
# Cuando esta app se cierra: la nueva en su lugar (si falla, vuelve la de antes) y se abre
nohup /bin/bash -c '
  while kill -0 "$1" 2>/dev/null; do sleep 0.3; done
  rm -rf "$2.vieja"
  if mv "$2" "$2.vieja"; then
    if mv "$3" "$2"; then rm -rf "$2.vieja"; else mv "$2.vieja" "$2"; fi
  fi
  rm -rf "$4"
  open "$2"' _ "$PID" "$APP" "$NUEVA" "$T" >/dev/null 2>&1 &
disown
exit 0
