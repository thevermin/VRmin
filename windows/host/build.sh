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
# 3D con IA: ONNX Runtime con DirectML (MIT) y DirectML (Microsoft, redistribuible), de NuGet; miniz (MIT) para
# comprimir la profundidad. También en versiones fijas.
if [ ! -f "$DEPS/ort/build/native/include/onnxruntime_c_api.h" ]; then
    rm -rf "$DEPS/ort" && mkdir -p "$DEPS/ort"
    curl -fsSL -o "$DEPS/ort.nupkg" "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1"
    echo "6763468507b7cfc777b1334b3e174c11a540ddacb7bd4354bc2e0ec89e56eec2  $DEPS/ort.nupkg" | sha256sum -c - >/dev/null
    (cd "$DEPS/ort" && unzip -q ../ort.nupkg 'build/native/include/*' 'runtimes/win-x64/native/onnxruntime.dll' LICENSE ThirdPartyNotices.txt)
    rm -f "$DEPS/ort.nupkg"
fi
if [ ! -f "$DEPS/dml/bin/x64-win/DirectML.dll" ]; then
    rm -rf "$DEPS/dml" && mkdir -p "$DEPS/dml"
    curl -fsSL -o "$DEPS/dml.nupkg" "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.2"
    echo "9f07482559087088a4dba4ae76eeeee1fad3f7077a92ccfbdb439c6bc2964c09  $DEPS/dml.nupkg" | sha256sum -c - >/dev/null
    (cd "$DEPS/dml" && unzip -q ../dml.nupkg 'bin/x64-win/DirectML.dll' LICENSE.txt ThirdPartyNotices.txt)
    rm -f "$DEPS/dml.nupkg"
fi
if [ ! -f "$DEPS/miniz/miniz.c" ]; then
    rm -rf "$DEPS/miniz" && mkdir -p "$DEPS/miniz"
    curl -fsSL -o "$DEPS/miniz.zip" "https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip"
    echo "ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5  $DEPS/miniz.zip" | sha256sum -c - >/dev/null
    (cd "$DEPS/miniz" && unzip -q ../miniz.zip miniz.c miniz.h LICENSE)
    rm -f "$DEPS/miniz.zip"
fi
x86_64-w64-mingw32-gcc -O2 -c "$DEPS/miniz/miniz.c" -o "$OUT/miniz.o"
x86_64-w64-mingw32-windres host.rc -O coff -o "$OUT/host.res"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wno-unknown-pragmas -municode -mwindows -static \
    -I"$DEPS/webview/core/include" -I"$DEPS/wv2/build/native/include" -I"$DEPS/ort/build/native/include" -I"$DEPS/miniz" -Icompat \
    common.cpp quest.cpp messages.cpp game.cpp convert.cpp encoder.cpp desktop.cpp audio.cpp mic.cpp report.cpp windows.cpp cable.cpp sentinel.cpp update.cpp record.cpp depth.cpp vmonitor.cpp games.cpp setup.cpp ui.cpp main.cpp "$OUT/host.res" "$OUT/miniz.o" \
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
rm -f "$OUT/host.res" "$OUT/miniz.o"
# 3D con IA: ONNX Runtime y DirectML (el modelo se baja la primera vez que se usa)
mkdir -p "$OUT/ai"
cp "$DEPS/ort/runtimes/win-x64/native/onnxruntime.dll" "$DEPS/dml/bin/x64-win/DirectML.dll" "$OUT/ai/"
cp README.txt "$OUT/"
mkdir -p "$OUT/licenses"
cp "$DEPS/webview/LICENSE" "$OUT/licenses/webview.txt"
cp "$DEPS/wv2/LICENSE.txt" "$OUT/licenses/WebView2-SDK.txt"
cp "$DEPS/ort/LICENSE" "$OUT/licenses/ONNX-Runtime.txt"
cp "$DEPS/ort/ThirdPartyNotices.txt" "$OUT/licenses/ONNX-Runtime-ThirdPartyNotices.txt"
cp "$DEPS/dml/LICENSE.txt" "$OUT/licenses/DirectML.txt"
cp "$DEPS/dml/ThirdPartyNotices.txt" "$OUT/licenses/DirectML-ThirdPartyNotices.txt"
cp "$DEPS/miniz/LICENSE" "$OUT/licenses/miniz.txt"
echo "Listo: $OUT"
