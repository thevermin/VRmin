#!/bin/bash
# Compila VRmin Host para Windows (desde Linux o macOS con mingw-w64) y arma la carpeta para el .zip
set -e
cd "$(dirname "$0")"
OUT=${1:-build}
mkdir -p "$OUT/runtime"
x86_64-w64-mingw32-windres host.rc -O coff -o "$OUT/host.res"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wno-unknown-pragmas -municode -mwindows -static \
    common.cpp quest.cpp messages.cpp game.cpp convert.cpp encoder.cpp desktop.cpp audio.cpp mic.cpp report.cpp games.cpp setup.cpp main.cpp "$OUT/host.res" \
    -o "$OUT/VRmin Host.exe" \
    -lws2_32 -liphlpapi -lbcrypt -lmfplat -lmfuuid -lole32 -loleaut32 -luuid -ld3d11 -ldxgi -lshlwapi -lshell32 \
    -luser32 -lgdi32 -lwinmm -lstrmiids
# El runtime de OpenXR es el mismo que usa la Mac (dentro de Wine)
cp ../../mac/openxr/questvrbridge_openxr.dll ../../mac/openxr/questvrbridge_openxr.json \
   ../../mac/openxr/questvrbridge_openxr32.dll ../../mac/openxr/questvrbridge_openxr32.json "$OUT/runtime/"
# OpenComposite: el que compila VRmin (mac/opencomposite)
mkdir -p "$OUT/OpenComposite/bin"
cp ../../mac/opencomposite/vrclient_x64.dll ../../mac/opencomposite/vrclient.dll "$OUT/OpenComposite/bin/"
cp ../../mac/opencomposite/README.md ../../mac/opencomposite/LICENSE-OpenComposite.txt "$OUT/OpenComposite/"
rm -f "$OUT/host.res"
cp README.txt "$OUT/"
echo "Listo: $OUT"
