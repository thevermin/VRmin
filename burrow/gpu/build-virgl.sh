#!/bin/bash
# The Mac side of Burrow's GPU: virglrenderer with Venus (Vulkan commands from Linux, run on the Mac with MoltenVK →
# Metal). It's the macOS fork used by libkrun/krunkit (gitlab.freedesktop.org/slp/virglrenderer, MIT), at the tag in
# VIRGL_TAG, with gpu/patches applied. → build/virgl/lib/libvirglrenderer*.dylib
# Needs Homebrew: meson ninja pkg-config libepoxy molten-vk
set -euo pipefail
cd "$(dirname "$0")/.."
tag=$(cat gpu/VIRGL_TAG)
src=.deps/virglrenderer
if [ "$(git -C "$src" describe --tags 2>/dev/null)" != "$tag" ]; then
  rm -rf "$src"; mkdir -p .deps
  git clone -q --depth 1 --branch "$tag" https://gitlab.freedesktop.org/slp/virglrenderer.git "$src"
fi
# Burrow's changes to it (gpu/patches)
git -C "$src" checkout -q . && git -C "$src" clean -qfd
for p in gpu/patches/*.patch; do [ -f "$p" ] && git -C "$src" apply "$PWD/$p"; done
out="$PWD/build/virgl"
rm -rf "$src/build"
export LIBRARY_PATH="$(brew --prefix)/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"   # (it links -lMoltenVK)
meson setup "$src/build" "$src" --prefix="$out" --buildtype=release -Dvenus=true -Drender-server=false -Dtests=false > /dev/null
meson compile -C "$src/build" > /dev/null
meson install -C "$src/build" > /dev/null
ls "$out/lib"/libvirglrenderer*.dylib
