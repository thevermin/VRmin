#!/bin/bash
# Abre el SteamVR real en el visor, sin juego, como al ponerse un Valve Index: su sala y su menú (con la biblioteca de
# Steam, desde donde se instalan y se abren los juegos). Queda abierto hasta cerrarlo (los juegos que se abren desde su
# menú lo usan y, al cerrarse, se vuelve a su sala).
#   abrir_steamvr.sh            lo abre (o, si ya está abierto, lo deja así)
#   abrir_steamvr.sh --cerrar   lo cierra
source "$(dirname "$0")/entorno.sh"
source "$RES/runtime.sh"
cd "$P"
if [ "$1" = "--cerrar" ]; then
  cerrar_steamvr
  say "SteamVR cerrado" "SteamVR closed"
  exit 0
fi
motor_listo || { say "Primero instalá el motor." "Install the engine first."; exit 1; }
[ -f "$STEAMLIB/common/SteamVR/bin/win64/vrstartup.exe" ] || {
  say "SteamVR no está instalado en el Steam de VRmin: buscalo en la biblioteca de Steam (en Herramientas) e instalalo." \
      "SteamVR isn't installed in VRmin's Steam: find it in Steam's library (under Tools) and install it."; exit 1; }
# Steam cerrado: se abre primero (SteamVR toma de él tu biblioteca) y se espera a que cargue, como al elegir un juego
# en el Quest. Tarda alrededor de un minuto
if ! steam_abierto; then
  bash "$RES/abrir_steam.sh" || exit 1
  for i in $(seq 1 75); do steam_abierto && break; sleep 2; done
  steam_abierto || { say "Steam no terminó de abrir: probá de nuevo en un momento." "Steam didn't finish opening: try again in a moment."; exit 1; }
  for i in $(seq 1 45); do pgrep -f 'steamwebhelper' >/dev/null && break; sleep 2; done
  say "Steam abierto, esperando que termine de cargar…" "Steam is open, waiting for it to finish loading…"
  sleep 25
fi

# Los juegos que se abran desde el menú de SteamVR lo usan a él (OpenVR y OpenXR → SteamVR, con el driver de VRmin)
preparar_runtime steamvr || exit 1
ajustes_dll
gpu_luid_dxmt
microfono_wine
# (si quedó abierto por un juego, el vigía de ese juego lo cerraría cuando el juego se cierre: ahora queda abierto)
v="$(cat "$Q/.vigia_steamvr" 2>/dev/null)"; [ -n "$v" ] && kill "$v" 2>/dev/null; rm -f "$Q/.vigia_steamvr"
ya=0; pgrep -f 'vrserver\.exe' >/dev/null && ya=1
# (el registro de vrcompositor junta todas las veces que se abrió: solo cuenta lo que se escriba desde ahora)
LOGC="$Q/openvr/logs/vrcompositor.txt"
antes=$(stat -f %z "$LOGC" 2>/dev/null || echo 0)
nuevo() { local s; s=$(stat -f %z "$LOGC" 2>/dev/null || echo 0)
  if [ "$s" -lt "$antes" ]; then cat "$LOGC"; else tail -c +$((antes + 1)) "$LOGC"; fi; }
abrir_steamvr
[ $ya = 1 ] && exit 0
# ¿Llega la imagen de SteamVR? Su pantalla (vrcompositor) necesita el DXMT de VRmin con búferes y texturas compartidos
# (shared-buffer-handle, keyed-mutex…): con uno anterior se cierra al arrancar, una y otra vez, y en el visor no se ve
# nada. Entonces se cierra SteamVR y se avisa por qué
for i in $(seq 1 40); do
  sleep 1
  grep -q 'Frames: sending to VRmin Host' "$Q/driver_vrmin.log" 2>/dev/null && exit 0
  if nuevo 2>/dev/null | grep -q 'CreateSharedFrameInfoConstantBuffer'; then
    disown -a 2>/dev/null   # (sin el «Terminated» de bash por el vrstartup que abrió abrir_steamvr)
    cerrar_steamvr
    say "La pantalla de SteamVR no arrancó: necesita un motor de DirectX (DXMT) más nuevo que el instalado, que llega con una versión nueva de VRmin. SteamVR cerrado." \
        "SteamVR's display couldn't start: it needs a newer DirectX engine (DXMT) than the one installed, which comes with a newer VRmin version. SteamVR closed."
    exit 1
  fi
done
