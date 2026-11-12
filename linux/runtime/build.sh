#!/bin/bash
# Runtime de OpenXR de VRmin para Linux (libvrmin_openxr.so, solo Vulkan): el mismo mac/openxr/questvrbridge_openxr.c
# que en Windows, con lo de Windows hecho con lo de Linux (win32_linux.h). Lo usan los juegos con Proton (wineopenxr y
# OpenComposite). Hace falta gcc y git (para bajar las cabeceras de OpenXR y Vulkan, las mismas versiones que en Windows;
# o se le pasan con OPENXR_INCLUDE=... VULKAN_INCLUDE=...).
#   ./build.sh [carpeta]  →  <carpeta>/runtime/libvrmin_openxr.so y vrmin_openxr.json (por defecto, ../host/build: al
#   lado de vrmin-host, de donde VRmin Host lo instala)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
out="$(mkdir -p "${1:-$repo/linux/host/build}" && cd "${1:-$repo/linux/host/build}" && pwd)/runtime"
deps="$here/build"
xr="${OPENXR_INCLUDE:-$deps/OpenXR-SDK/include}"
vk="${VULKAN_INCLUDE:-$deps/Vulkan-Headers/include}"
if [ ! -f "$xr/openxr/openxr.h" ]; then
  git clone -q --depth 1 --branch release-1.1.43 https://github.com/KhronosGroup/OpenXR-SDK.git "$deps/OpenXR-SDK"
fi
if [ ! -f "$vk/vulkan/vulkan.h" ]; then
  git clone -q --depth 1 --branch v1.3.296 https://github.com/KhronosGroup/Vulkan-Headers.git "$deps/Vulkan-Headers"
fi
mkdir -p "$out"
# (-z nodelete: el loader de OpenXR descarga el runtime al cerrar la instancia, y sus hilos siguen andando)
gcc -O2 -Wall -fPIC -shared -fvisibility=hidden -o "$out/libvrmin_openxr.so" "$repo/mac/openxr/questvrbridge_openxr.c" \
  -I"$here" -I"$xr" -I"$vk" -pthread -ldl -lm -Wl,-soname,libvrmin_openxr.so -Wl,-z,nodelete -Wl,-z,defs
cp "$here/vrmin_openxr.json" "$out/vrmin_openxr.json"
echo "$out/libvrmin_openxr.so"
