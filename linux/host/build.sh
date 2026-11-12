#!/bin/bash
# Compila VRmin Host para Linux (g++ 11 o más nuevo y FFmpeg 6 o más nuevo, con sus archivos de desarrollo:
# en Ubuntu/Debian, «sudo apt install g++ libavcodec-dev libavutil-dev libx11-dev libxext-dev libxtst-dev libxrandr-dev libxcomposite-dev»): ./build.sh  →  build/vrmin-host
# La ventana usa WebKitGTK («sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev», o la 4.0); sin eso, queda sin ventana.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
# Ventana: webview (MIT), la misma versión que en Windows; se baja una vez a .deps y se revisa que sea esa
UI=()
WK=$(pkg-config --exists webkit2gtk-4.1 && echo webkit2gtk-4.1 || { pkg-config --exists webkit2gtk-4.0 && echo webkit2gtk-4.0; } || true)
if [ -n "$WK" ] && pkg-config --exists gtk+-3.0; then
    DEPS=.deps
    if [ ! -f "$DEPS/webview/core/include/webview/webview.h" ]; then
        rm -rf "$DEPS/webview" && mkdir -p "$DEPS"
        git clone -q --depth 1 --branch 0.12.0 https://github.com/webview/webview.git "$DEPS/webview"
    fi
    (cd "$DEPS/webview" && sha256sum --quiet -c -) <<'SUMS'
b1ff6e11431d031e5f7917e5301f4bcdf3910a2763a0720abfd291994eee7f7b  core/include/webview/webview.h
22c4c38a605ae5362bdbd795b4358f3599c7a7e58d4824c4ed55e3be192386e0  LICENSE
SUMS
    UI=(-DVRMIN_UI -I"$DEPS/webview/core/include" $(pkg-config --cflags --libs gtk+-3.0 "$WK"))
else
    echo "Sin WebKitGTK: VRmin Host queda sin ventana (No WebKitGTK: building without a window)" >&2
fi
g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -pthread -o build/vrmin-host \
  main.cpp common.cpp quest.cpp game.cpp messages.cpp convert.cpp encoder.cpp games.cpp vrsetup.cpp pendiente.cpp ui.cpp \
  desktop.cpp windows.cpp \
  $(pkg-config --cflags --libs libavcodec libavutil) \
  $(pkg-config --cflags --libs x11 xext xtst xrandr xcomposite) ${UI[@]+"${UI[@]}"}
echo "build/vrmin-host"
