#!/bin/bash
# OpenComposite para Linux (vrclient.so nativo): el mismo código que usa VRmin en Windows (GameNative/opencomposite, el
# commit de mac/opencomposite/COMMIT) con los cambios de VRmin (mac/opencomposite/*.patch y los de Linux,
# linux/opencomposite/*.patch). Con Proton, los juegos de SteamVR cargan este vrclient.so (por el vrclient de Proton).
# Hace falta: git, cmake, python3, g++ y los archivos de desarrollo de Vulkan, X11 y OpenGL (en Ubuntu/Debian,
# «sudo apt install git cmake python3 g++ libvulkan-dev libx11-dev libgl-dev»).
#   ./build.sh [carpeta]  →  <carpeta>/OpenComposite/bin/linux64/vrclient.so (por defecto, ../host/build: al lado de
#   vrmin-host, de donde VRmin Host lo instala)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
out="$(mkdir -p "${1:-$repo/linux/host/build}" && cd "${1:-$repo/linux/host/build}" && pwd)"
work="$here/build"
commit="$(tr -d '[:space:]' < "$repo/mac/opencomposite/COMMIT")"
mkdir -p "$work"

# El código, en el commit justo (y sin cambios de antes: se vuelven a aplicar todos)
if [ ! -d "$work/oc/.git" ]; then
  git clone -q https://github.com/GameNative/opencomposite.git "$work/oc"
fi
git -C "$work/oc" fetch -q origin "$commit" 2>/dev/null || true
git -C "$work/oc" checkout -q -f "$commit"
git -C "$work/oc" clean -q -fdx
git -C "$work/oc" submodule update -q --init --recursive
shopt -s nullglob
for p in "$repo"/mac/opencomposite/*.patch "$here"/*.patch; do
  echo "== $(basename "$p")"
  git -C "$work/oc" apply --whitespace=nowarn "$p"
done

cmake -S "$work/oc" -B "$work/cmake" -DCMAKE_BUILD_TYPE=Release -DOC_VERSION="${commit:0:8}-gamenative-vrmin" >/dev/null
cmake --build "$work/cmake" --target OCOVR --parallel "$(nproc)"
mkdir -p "$out/OpenComposite/bin/linux64"
cp "$work/cmake/bin/linux64/vrclient.so" "$out/OpenComposite/bin/linux64/vrclient.so"
cp "$work/cmake/bin/version.txt" "$out/OpenComposite/bin/version.txt"   # (así Steam no reemplaza este runtime)
cp "$work/oc/LICENSE.txt" "$out/OpenComposite/LICENSE-OpenComposite.txt"
echo "$out/OpenComposite/bin/linux64/vrclient.so"
