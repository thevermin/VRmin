#!/bin/bash
# Arma mac/test/gpu_name.inc con los tipos KMT y gpu_name() del runtime, y compila d3dshare.exe (mingw)
set -e
F=mac/openxr/questvrbridge_openxr.c
{ awk '/^typedef UINT KMT_HANDLE;/,/^typedef LONG \(WINAPI \*kmt_fn\)/' $F
  awk '/^static int gpu_name\(/,/^}$/' $F; } > mac/test/gpu_name.inc
x86_64-w64-mingw32-gcc -O2 -Imac/test mac/test/d3dshare_main.c -o "${1:-d3dshare.exe}" -ld3d11 -ldxguid -luuid
