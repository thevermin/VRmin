#!/bin/bash
# Compila VRmin Host para Linux (g++ 11 o más nuevo y FFmpeg 6 o más nuevo, con sus archivos de desarrollo:
# en Ubuntu/Debian, «sudo apt install g++ libavcodec-dev libavutil-dev libx11-dev libxext-dev libxtst-dev libxrandr-dev libxcomposite-dev»): ./build.sh  →  build/vrmin-host
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -pthread -o build/vrmin-host \
  main.cpp common.cpp quest.cpp game.cpp messages.cpp convert.cpp encoder.cpp games.cpp vrsetup.cpp pendiente.cpp \
  desktop.cpp windows.cpp \
  $(pkg-config --cflags --libs libavcodec libavutil) \
  $(pkg-config --cflags --libs x11 xext xtst xrandr xcomposite)
echo "build/vrmin-host"
