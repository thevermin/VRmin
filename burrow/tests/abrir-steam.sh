#!/bin/bash
# Abre el Steam nativo de Linux ARM64 bajo Burrow (en la Mac), con una pantalla virtual que se ve con Compartir
# Pantalla: vnc://127.0.0.1:5901 (contraseña «vrmin»). Se cierra con Control-C.
#   abrir-steam.sh <carpeta de steam-arm64.sh>
set -euo pipefail
cd "$(dirname "$0")"
S=$(cd "$1" && pwd)
echo "Abriendo Steam bajo Burrow. En unos segundos: Finder › Ir › Conectarse al servidor › vnc://127.0.0.1:5901 (contraseña: vrmin)"
# (los programas de Steam, ejecutables: si la carpeta se copió o se bajó de nuevo, pueden haber perdido el permiso)
python3 -I - "$S/Steam" <<'EOF'
import os, sys
for d, _, fs in os.walk(sys.argv[1]):
    for f in fs:
        p = os.path.join(d, f)
        if os.path.islink(p): continue
        with open(p, 'rb') as h: cab = h.read(4)
        if cab == b'\x7fELF' or cab[:2] == b'#!': os.chmod(p, 0o755)
EOF
# (como en Linux: ~/.steam/root es la carpeta de Steam; ahí busca sus letras)
ln -sfn ../../Steam "$S/home/.steam/root"
# (con tu usuario de la Mac: los archivos de Steam son tuyos, y Burrow usa otro si no se le dice; Steam lo busca en
#  /etc/passwd)
grep -q ":$(id -u):" steam-root/etc/passwd || echo "steam:x:$(id -u):$(id -g)::$S/home:/bin/sh" >> steam-root/etc/passwd
abrir() {
  ./elfuse "$@" --user "$(id -u):$(id -g)" --sysroot "$PWD/steam-root" --env HOME="$S/home" --env LANG=C.UTF-8 \
    --env LD_PRELOAD=/usr/lib/burrow/libburrow_drm.so /bin/dash /usr/bin/burrow-steam "$S/Steam"
}
# (BURROW_VERBOSE=1: cada llamada al sistema de cada programa, solo en steam.log, para ver dónde se traba algo; se
#  cierra sola a los 3 minutos)
if [ -n "${BURROW_VERBOSE:-}" ]; then
  echo "(registrando todo en steam.log: se cierra sola en 3 minutos)"
  ( sleep 180; pkill -f elfuse ) &
  abrir --verbose > steam.log 2>&1 || true
  echo "Listo: $(du -h steam.log | cut -f1) de registro"
else
  abrir 2>&1 | tee steam.log
fi
