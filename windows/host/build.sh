#!/bin/bash
# Compila VRmin Host para Windows (desde Linux o macOS con mingw-w64) y arma la carpeta para el .zip
set -e
cd "$(dirname "$0")"
OUT=${1:-build}
mkdir -p "$OUT/runtime"
# La ventana usa WebView2 (el navegador de Windows) por medio de webview (MIT) y las cabeceras del SDK de WebView2
# (BSD): se bajan una vez a .deps, en versiones fijas
DEPS=.deps
if [ ! -f "$DEPS/webview/core/include/webview/webview.h" ]; then
    rm -rf "$DEPS/webview" && mkdir -p "$DEPS"
    git clone -q --depth 1 --branch 0.12.0 https://github.com/webview/webview.git "$DEPS/webview"
fi
if [ ! -f "$DEPS/wv2/build/native/include/WebView2.h" ]; then
    rm -rf "$DEPS/wv2" && mkdir -p "$DEPS/wv2"
    curl -fsSL -o "$DEPS/wv2.nupkg" "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40"
    echo "ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74  $DEPS/wv2.nupkg" | sha256sum -c - >/dev/null
    (cd "$DEPS/wv2" && unzip -q ../wv2.nupkg 'build/native/include/*' LICENSE.txt)
fi
x86_64-w64-mingw32-windres host.rc -O coff -o "$OUT/host.res"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wno-unknown-pragmas -municode -mwindows -static \
    -I"$DEPS/webview/core/include" -I"$DEPS/wv2/build/native/include" -Icompat \
    common.cpp quest.cpp messages.cpp game.cpp convert.cpp encoder.cpp desktop.cpp audio.cpp mic.cpp report.cpp windows.cpp cable.cpp sentinel.cpp update.cpp record.cpp games.cpp setup.cpp ui.cpp main.cpp "$OUT/host.res" \
    -o "$OUT/VRmin Host.exe" \
    -lws2_32 -liphlpapi -lbcrypt -lmfplat -lmfuuid -lole32 -loleaut32 -luuid -ld3d11 -ldxgi -lshlwapi -lshell32 \
    -luser32 -lgdi32 -lwinmm -lstrmiids -ladvapi32 -lversion -ldwmapi -ld2d1 -ldwrite -lurlmon -lwinhttp -lmfreadwrite
# El runtime de OpenXR es el mismo que usa la Mac (dentro de Wine)
cp ../../mac/openxr/questvrbridge_openxr.dll ../../mac/openxr/questvrbridge_openxr.json \
   ../../mac/openxr/questvrbridge_openxr32.dll ../../mac/openxr/questvrbridge_openxr32.json "$OUT/runtime/"
# OpenComposite: el que compila VRmin (mac/opencomposite)
mkdir -p "$OUT/OpenComposite/bin"
cp ../../mac/opencomposite/vrclient_x64.dll ../../mac/opencomposite/vrclient.dll "$OUT/OpenComposite/bin/"
cp ../../mac/opencomposite/README.md ../../mac/opencomposite/LICENSE-OpenComposite.txt "$OUT/OpenComposite/"
rm -f "$OUT/host.res"
cp README.txt "$OUT/"
mkdir -p "$OUT/licenses"
cp "$DEPS/webview/LICENSE" "$OUT/licenses/webview.txt"
cp "$DEPS/wv2/LICENSE.txt" "$OUT/licenses/WebView2-SDK.txt"
echo "Listo: $OUT"
