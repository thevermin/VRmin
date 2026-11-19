#!/bin/sh
# Adentro de Burrow: una pantalla virtual (Xvfb), un VNC para verla desde la Mac (Compartir Pantalla,
# vnc://localhost:5901) y el Steam nativo de Linux ARM64.
#   burrow-steam <carpeta de Steam> [opciones de Steam]
# (todo por TCP en la Mac, solo para esta Mac: la pantalla en el puerto 6000 y el VNC en el 5901)
STEAM=$1; shift
Xvfb :0 -screen 0 1280x800x24 -ac -listen tcp -nolisten unix -nolisten local > /tmp/xvfb.log 2>&1 &
sleep 3
export DISPLAY=127.0.0.1:0
x11vnc -display "$DISPLAY" -rfbport 5901 -localhost -forever -shared -nopw -quiet > /tmp/x11vnc.log 2>&1 &
export LD_LIBRARY_PATH="$STEAM/steamrtarm64:$STEAM/steamrtarm64/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export STEAM_RUNTIME=0
cd "$STEAM" && exec "$STEAM/steamrtarm64/steam" -noverifyfiles -nobootstrapupdate -skipinitialbootstrap -norepairfiles \
  -cef-disable-gpu -cef-disable-gpu-compositing "$@"
