#!/bin/bash
# Arma «VRmin Linux <versión>.tar.gz»: VRmin Host con FFmpeg adentro (estático: solo los codificadores que usa,
# H.264/HEVC por VA-API y NVENC y H.264 por CPU con x264), el runtime de OpenXR y OpenComposite.
# Lo usa «VRmin para Linux» (.github/workflows/linux.yml) en Ubuntu 22.04, para que ande en distribuciones de 2022 en adelante.
# Hace falta lo de host/build.sh, runtime/build.sh y opencomposite/build.sh, más nasm, libva-dev y libx264-dev.
#   ./paquete.sh [carpeta]  →  <carpeta>/VRmin Linux <versión>.tar.gz (por defecto, host/build)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mkdir -p "${1:-$here/host/build}" && cd "${1:-$here/host/build}" && pwd)"
deps="$here/host/.deps"
ff="$deps/ffmpeg-static"
v=$(grep -o 'VRMIN_LINUX_VERSION "[^"]*' "$here/host/host.h" | sed 's/.*"//')

# FFmpeg estático (una vez)
if [ ! -f "$ff/lib/pkgconfig/libavcodec.pc" ]; then
  mkdir -p "$deps"
  [ -d "$deps/nv-codec-headers" ] || git clone -q --depth 1 --branch n12.0.16.1 https://github.com/FFmpeg/nv-codec-headers.git "$deps/nv-codec-headers"
  make -s -C "$deps/nv-codec-headers" install PREFIX="$ff" >/dev/null
  [ -d "$deps/FFmpeg" ] || git clone -q --depth 1 --branch n7.1 https://github.com/FFmpeg/FFmpeg.git "$deps/FFmpeg"
  (cd "$deps/FFmpeg" && PKG_CONFIG_PATH="$ff/lib/pkgconfig" ./configure --prefix="$ff" --enable-static --disable-shared --enable-pic \
     --disable-programs --disable-doc --disable-everything --disable-avformat --disable-avdevice --disable-avfilter \
     --disable-swscale --disable-swresample --disable-xlib --disable-libxcb --disable-sdl2 \
     --enable-gpl --enable-libx264 --enable-vaapi --enable-ffnvcodec --enable-nvenc \
     --enable-encoder=libx264,h264_vaapi,hevc_vaapi,h264_nvenc,hevc_nvenc >/dev/null &&
   make -s -j"$(nproc)" >/dev/null && make -s install >/dev/null)
fi

# VRmin Host, el runtime y OpenComposite
VRMIN_FFMPEG_STATIC=1 PKG_CONFIG_PATH="$ff/lib/pkgconfig" bash "$here/host/build.sh"
pkg="$out/VRmin Linux"
rm -rf "$pkg"; mkdir -p "$pkg"
cp "$here/host/build/vrmin-host" "$pkg/"
bash "$here/runtime/build.sh" "$pkg"
bash "$here/opencomposite/build.sh" "$pkg"
cp "$here/../LICENSE" "$pkg/LICENSE.txt"
cat > "$pkg/README.txt" <<EOF
VRmin for Linux $v (beta)

Open it with:  ./vrmin-host
Then open VRmin on the Quest (same Wi-Fi). VR games from Steam (with Proton) use VRmin while VRmin Host is open;
when you close it, your VR settings go back to how they were.

It needs (most desktops already have them): WebKitGTK 4.1, PulseAudio or PipeWire, X11 (on Wayland the desktop
view isn't available yet) and, for the video, a GPU with VA-API (AMD, Intel) or NVENC (NVIDIA, driver 520 or newer).
On Ubuntu/Debian:  sudo apt install libwebkit2gtk-4.1-0 libpulse0 libva2 libva-drm2 libva-x11-2
Log: ~/.local/share/VRmin/VRmin.log
EOF
(cd "$out" && tar -czf "VRmin Linux $v.tar.gz" "VRmin Linux")
echo "$out/VRmin Linux $v.tar.gz"
