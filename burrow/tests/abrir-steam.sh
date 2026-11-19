#!/bin/bash
# Abre el Steam nativo de Linux ARM64 bajo Burrow (en la Mac), con una pantalla virtual que se ve con Compartir
# Pantalla: vnc://localhost:5901. Se cierra con Control-C.
#   abrir-steam.sh <carpeta de steam-arm64.sh>
set -euo pipefail
cd "$(dirname "$0")"
S=$(cd "$1" && pwd)
echo "Abriendo Steam bajo Burrow. En unos segundos: Finder › Ir › Conectarse al servidor › vnc://localhost:5901"
# (con tu usuario de la Mac: los archivos de Steam son tuyos, y Burrow usa otro si no se le dice; Steam lo busca en
#  /etc/passwd)
grep -q ":$(id -u):" steam-root/etc/passwd || echo "steam:x:$(id -u):$(id -g)::$S/home:/bin/sh" >> steam-root/etc/passwd
./elfuse --user "$(id -u):$(id -g)" --sysroot "$PWD/steam-root" --env HOME="$S/home" --env LD_PRELOAD=/usr/lib/burrow/libburrow_drm.so \
  /bin/dash /usr/bin/burrow-steam "$S/Steam" 2>&1 | tee steam.log
