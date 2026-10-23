#!/bin/bash
# «Solo Modo Centinela»: con el visor apoyado y conectado por cable (modo desarrollador), abre VRmin en el Quest
# directo en el Modo Centinela, sin tocar el visor. Argumento: el nombre de esta Mac (como la ve el Quest).
source "$(dirname "$0")/entorno.sh"
# todo lo que pasa queda también en logs/vigia.log (para revisar errores)
exec > >(tee "$P/logs/vigia.log") 2>&1
echo "== $(date '+%d/%m %H:%M:%S') · Mac: $1"
ADB="$P/herramientas/platform-tools/adb"
[ -x "$ADB" ] || { say "Faltan las herramientas del Modo Centinela: tocá «Instalar» en VRmin Host" "The Watch Mode tools are missing: click “Install” in VRmin Host"; exit 1; }
"$ADB" start-server >/dev/null 2>&1
ST="$("$ADB" devices | awk 'NR>1 && $2!="" {print $2; exit}')"
case "$ST" in
  device) ;;
  unauthorized) say "Ponete el visor una vez y tocá «Permitir» en el aviso de depuración USB" "Put on the headset once and tap “Allow” on the USB debugging prompt"; exit 1 ;;
  *) say "No veo el Quest: conectalo por cable USB-C" "I can't see the Quest: connect it with the USB-C cable"; exit 1 ;;
esac
say "Despertando el visor…" "Waking the headset…"
"$ADB" shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
# que no se apague sin tenerlo puesto (el Modo Centinela lo vuelve a la normalidad al terminar)
"$ADB" shell am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1
sleep 1
say "Abriendo VRmin en el Modo Centinela…" "Opening VRmin in Watch Mode…"
# (adb shell junta todo en una línea para el visor: el nombre de la Mac va entre comillas, con sus espacios)
Q="$(printf "%s" "$1" | sed "s/'/'\\\\''/g")"
OUT="$("$ADB" shell am start -n com.vrmin.app/org.questvrbridge.client.VrActivity --es mac "'$Q'" --ez vigia true 2>&1)"
echo "$OUT"
if echo "$OUT" | grep -q "^Error"; then
  say "No se pudo abrir VRmin en el visor (arriba está lo que contestó el visor)" "Couldn't open VRmin on the headset (see the headset's answer above)"; exit 1
fi
say "Listo: dejá el visor apoyado mirándote, a la altura del pecho, y abrí un juego con «Jugar»." \
    "Done: leave the headset resting facing you at chest height, and open a game with “Play”."
exit 0
