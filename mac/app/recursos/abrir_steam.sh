#!/bin/bash
# Abre Steam (Windows). Queda abierto en segundo plano.
source "$(dirname "$0")/entorno.sh"
cd "$P"
export WINEDEBUG=-all,err+module
motor_listo || { say "Hay una versión nueva del motor: tocá «Instalar» en «Motor de juegos de Windows» (Steam y los juegos se conservan)." "There's a new engine version: press “Install” on “Windows games engine” (Steam and games are kept)."; exit 1; }
[ -f "$STEAMDIR/steam.exe" ] || { say "Primero instalá Steam." "Install Steam first."; exit 1; }
if steam_abierto; then
  # Ya abierto: traer su ventana adelante
  WINEDEBUG=-all "$WINE" "C:\\Program Files (x86)\\Steam\\steam.exe" "steam://open/main" >/dev/null 2>&1 < /dev/null &
  disown
  say "Steam ya estaba abierto: mostrando su ventana." "Steam was already open: showing its window."
  exit 0
fi
# Ventana negra de Steam en Wine 11: delante de su navegador (steamwebhelper.exe) va un intermediario que le
# agrega --disable-gpu --single-process (de ramiabih/play-windows-steam-on-mac, MIT). El original queda como
# steamwebhelper_real.exe. Se revisa cada vez porque las actualizaciones de Steam lo reemplazan.
W="$RES/steam/steamwebhelper.exe"
for d in "$STEAMDIR/bin/cef/"cef.win*; do
  [ -f "$d/steamwebhelper.exe" ] || continue
  cmp -s "$W" "$d/steamwebhelper.exe" && continue
  [ "$(stat -f %z "$d/steamwebhelper.exe")" -gt 500000 ] && cp "$d/steamwebhelper.exe" "$d/steamwebhelper_real.exe"
  [ -f "$d/steamwebhelper_real.exe" ] && cp "$W" "$d/steamwebhelper.exe" && say "Arreglo de la ventana de Steam aplicado ($(basename "$d"))" "Steam window fix applied ($(basename "$d"))"
done
say "Abriendo Steam (tarda ~1 minuto en aparecer; la primera vez se actualiza y pide iniciar sesión)" "Opening Steam (takes ~1 minute to appear; the first time it updates and asks you to sign in)"
# Ojo: sin "nohup" ni "env": son programas protegidos de macOS y borran DYLD_FALLBACK_LIBRARY_PATH,
# que Wine necesita para encontrar sus bibliotecas.
# Menos carga para Steam (su interfaz es un navegador que en Wine dibuja sin GPU): sin amigos ni chat
EXTRA="-nofriendsui -nochatui"
# -noverifyfiles: si Steam verifica sus archivos al arrancar, vuelve a poner su steamwebhelper.exe y la ventana
# queda negra otra vez. -cef-single-process: el navegador de Steam en un solo proceso (igual que el arreglo).
EXTRA="$EXTRA -noverifyfiles -cef-single-process"
"$WINE" "C:\\Program Files (x86)\\Steam\\steam.exe" -cef-disable-gpu -cef-disable-gpu-compositing -no-cef-sandbox $EXTRA \
  > "$P/logs/steam.log" 2>&1 < /dev/null &
disown
sleep 2
