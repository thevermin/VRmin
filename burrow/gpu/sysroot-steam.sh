#!/bin/bash
# El Linux mínimo para el Steam nativo de Linux ARM64 (en Ubuntu ARM64): el de Half-Life: Alyx más lo que pide el
# cliente de Steam (GTK2, NSS, CEF...) y una pantalla virtual con VNC (Xvfb + x11vnc) para verlo desde la Mac. Steam no
# va acá: lo baja burrow/tests/steam-arm64.sh de los servidores de Valve.
#   sysroot-steam.sh <carpeta de alyx-root> <carpeta nueva>
set -euo pipefail
trap 'echo "::error::sysroot-steam.sh: falló la línea $LINENO"' ERR
alyx=$1 root=$2
sudo apt-get install -y -qq dash xvfb x11vnc x11-xkb-utils xkb-data fonts-dejavu-core fontconfig-config ca-certificates \
  libgl1 libegl1 libice6 libsm6 libxcomposite1 libxdamage1 libxinerama1 libxtst6 libatk1.0-0t64 libatk-bridge2.0-0t64 \
  libatspi2.0-0t64 libcairo2 libcups2t64 libgdk-pixbuf-2.0-0 libgtk2.0-0t64 libibus-1.0-5 libnghttp2-14 libnm0 \
  libnspr4 libnss3 libopenal1 libpipewire-0.3-0t64 libssh2-1t64 libva2 libvdpau1 libpulse0 > /dev/null
rm -rf "$root"; cp -a "$alyx" "$root"
copy() { local f; for f in "$@"; do [ -e "$f" ] || continue; mkdir -p "$root$(dirname "$f")"; cp -L "$f" "$root$f"; done; }
# (lo que pide el cliente: steamui.so, steamwebhelper y libcef.so)
pide="libGL.so.1 libEGL.so.1 libICE.so.6 libSM.so.6 libXcomposite.so.1 libXdamage.so.1 libXinerama.so.1 libXtst.so.6
  libXrender.so.1 libatk-1.0.so.0 libatk-bridge-2.0.so.0 libatspi.so.0 libcairo.so.2 libcups.so.2 libgdk_pixbuf-2.0.so.0
  libgtk-x11-2.0.so.0 libgdk-x11-2.0.so.0 libibus-1.0.so.5 libnghttp2.so.14 libnm.so.0 libnspr4.so libnss3.so
  libnssutil3.so libsmime3.so libopenal.so.1 libpipewire-0.3.so.0 libssh2.so.1 libva.so.2 libvdpau.so.1 libpulse.so.0"
todo="/usr/bin/Xvfb /usr/bin/x11vnc /usr/bin/xkbcomp /bin/dash /usr/bin/sleep"
for n in $pide; do
  f=$(ldconfig -p | awk -v n="$n" '$1 == n && /AArch64/ { print $NF; exit }')
  [ -n "$f" ] || { echo "::error::Falta $n"; exit 1; }
  todo="$todo $f"
done
# (NSS abre sus módulos por su cuenta)
todo="$todo $(find /usr/lib/aarch64-linux-gnu -name 'libsoftokn3.so' -o -name 'libfreebl*.so' -o -name 'libnssckbi.so' \
  -o -name 'libnssdbm3.so' | tr '\n' ' ')"
done_=""
while [ -n "$todo" ]; do
  next=""
  for f in $todo; do
    case " $done_ " in *" $f "*) continue;; esac
    done_="$done_ $f"
    copy "$f"
    for l in $(ldd "$f" 2>/dev/null | awk '/=> \//{print $3}'); do next="$next $l"; done
  done
  todo=$next
done
# Los datos que usan: teclados de X, letras, certificados
for d in /usr/share/X11/xkb /usr/share/fonts/truetype/dejavu /etc/fonts /usr/share/X11/locale; do
  mkdir -p "$root$(dirname "$d")"; cp -a "$d" "$root$d"
done
mkdir -p "$root/etc/ssl/certs" "$root/bin" "$root/tmp"
cp -L /etc/ssl/certs/ca-certificates.crt "$root/etc/ssl/certs/"
ln -sf /bin/dash "$root/bin/sh"
printf 'root:x:0:0::/root:/bin/sh\nsteam:x:501:20::/tmp:/bin/sh\n' > "$root/etc/passwd"
printf 'root:x:0:\nstaff:x:20:\n' > "$root/etc/group"
# Lo que corre adentro: la pantalla virtual, el VNC y Steam
install -m 755 "$(dirname "$0")/../tests/steam-dentro.sh" "$root/usr/bin/burrow-steam"
echo "$root: $(find "$root" -type f | wc -l) archivos, $(du -sh "$root" | cut -f1)"
