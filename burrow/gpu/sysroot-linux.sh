#!/bin/bash
# The Linux side of Burrow's GPU test (on Ubuntu ARM64): a minimal Linux in <folder> with Mesa's Venus Vulkan driver,
# the Vulkan loader and vulkaninfo/vkcube, with every library they need.
#   sysroot-linux.sh <folder>
set -euo pipefail
root=$1
sudo apt-get install -y -qq mesa-vulkan-drivers vulkan-tools libvulkan1 libdrm-dev > /dev/null
mkdir -p "$root"
copy() { local f; for f in "$@"; do [ -e "$f" ] || continue; mkdir -p "$root$(dirname "$f")"; cp -L "$f" "$root$f"; done; }
venus=$(ls /usr/lib/aarch64-linux-gnu/libvulkan_virtio.so)
icd=$(grep -l libvulkan_virtio /usr/share/vulkan/icd.d/*.json)
copy /usr/bin/vulkaninfo /usr/bin/vkcube "$venus" $icd /lib/ld-linux-aarch64.so.1
# Every library they load, and the libraries those load
todo="/usr/bin/vulkaninfo /usr/bin/vkcube $venus /usr/lib/aarch64-linux-gnu/libvulkan.so.1"
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
# libdrm's GPU list, answered by Burrow (see drm-shim.c): loaded with LD_PRELOAD
mkdir -p "$root/usr/lib/burrow"
gcc -O2 -shared -fPIC -o "$root/usr/lib/burrow/libburrow_drm.so" "$(dirname "$0")/drm-shim.c" $(pkg-config --cflags libdrm)
mkdir -p "$root/dev/dri" "$root/tmp"
echo "$root: $(find "$root" -type f | wc -l) files, $(du -sh "$root" | cut -f1)"
ls "$root/usr/share/vulkan/icd.d"
