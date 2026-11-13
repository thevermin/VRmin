#!/bin/bash
# Runs Burrow's tests on this Mac (from the «burrow-prueba» package): each line «ok …», and a summary at the end
cd "$(dirname "$0")"
mkdir -p root/tmp
E=./elfuse
r=0
corre() { local name=$1 esperado=$2; shift 2; echo "== $name"; "$@" > "$name.txt" 2>&1; cat "$name.txt"
          if [ -n "$esperado" ] && ! diff -q "$esperado" "$name.txt" > /dev/null; then echo "!! $name: different from Linux"; r=1; fi; }
export BURROW_TEST=madriguera
corre estatico prueba-static.esperado $E ./prueba-static uno dos
corre dinamico prueba-dynamic.esperado env -u TMPDIR $E --sysroot root root/prueba uno dos
corre hilos hilos.esperado $E --sysroot root root/hilos
corre busybox "" $E ./busybox sh -c 'echo hola desde busybox; ls / | head -3; uname -a; seq 3 | wc -l'
echo
[ $r = 0 ] && echo "BURROW: TODO BIEN" || echo "BURROW: ALGO FALLÓ"
