#!/bin/bash
# Compila VRmin Host para Linux (g++ 11 o más nuevo): ./build.sh  →  build/vrmin-host
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -pthread -o build/vrmin-host \
  main.cpp common.cpp quest.cpp game.cpp messages.cpp convert.cpp pendiente.cpp
echo "build/vrmin-host"
