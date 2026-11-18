#!/bin/bash
# El Linux mínimo para Half-Life: Alyx ARM64 (en Ubuntu ARM64): el de la prueba de la GPU (Mesa Venus, el cargador de
# Vulkan) más las bibliotecas que el juego pide y no trae (lo que en Steam da su «Steam Linux Runtime»), con todas las
# que esas necesitan. Los programas del juego no van acá: Burrow los abre desde la carpeta del juego en la Mac.
#   sysroot-alyx.sh <carpeta de gpu-root> <carpeta nueva>
set -euo pipefail
gpu=$1 root=$2
sudo apt-get install -y -qq libstdc++6 libx11-6 libx11-xcb1 libxcb1 libuuid1 libssl3t64 libogg0 libglib2.0-0t64 \
  libfribidi0 libfreetype6 libfontconfig1 libharfbuzz0b libpango-1.0-0 libpng16-16t64 libbz2-1.0 libbrotli1 libexpat1 libudev1 libzstd1 libdrm2 libwayland-client0 > /dev/null
rm -rf "$root"; cp -a "$gpu" "$root"
copy() { local f; for f in "$@"; do [ -e "$f" ] || continue; mkdir -p "$root$(dirname "$f")"; cp -L "$f" "$root$f"; done; }
# (los nombres que piden las bibliotecas del juego: ver burrow/tests/alyx.sh)
pide="libstdc++.so.6 libgcc_s.so.1 libX11.so.6 libX11-xcb.so.1 libxcb.so.1 libuuid.so.1 libssl.so.3 libcrypto.so.3
  libogg.so.0 libglib-2.0.so.0 libgobject-2.0.so.0 libgio-2.0.so.0 libfribidi.so.0 libpng16.so.16 libbz2.so.1.0
  libbrotlidec.so.1 libexpat.so.1 libudev.so.1 libz.so.1 libzstd.so.1 libdrm.so.2 libwayland-client.so.0 libdl.so.2
  librt.so.1 libpthread.so.0 libm.so.6 libc.so.6
  libfreetype.so.6 libfontconfig.so.1 libharfbuzz.so.0 libpango-1.0.so.0"   # (estas el juego las trae con otro nombre)
todo=""
for n in $pide; do
  f=$(ldconfig -p | awk -v n="$n" '$1 == n && /AArch64/ { print $NF; exit }')
  [ -n "$f" ] || { echo "::error::Falta $n"; exit 1; }
  todo="$todo $f"
  # (con el nombre que piden, aunque el archivo se llame distinto)
  mkdir -p "$root$(dirname "$f")"; cp -L "$f" "$root$(dirname "$f")/$n"
done
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
echo "$root: $(find "$root" -type f | wc -l) archivos, $(du -sh "$root" | cut -f1)"
