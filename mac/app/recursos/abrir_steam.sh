#!/bin/bash
# Abre Steam (Windows). Queda abierto en segundo plano. Con «ventana», muestra su ventana aunque se use la Biblioteca de VRmin.
source "$(dirname "$0")/entorno.sh"
cd "$P"
export WINEDEBUG=-all,err+module
motor_listo || { say "Hay una versión nueva del motor: tocá «Instalar» en «Motor de juegos de Windows» (Steam y los juegos se conservan)." "There's a new engine version: press “Install” on “Windows games engine” (Steam and games are kept)."; exit 1; }
[ -f "$STEAMDIR/steam.exe" ] || { say "Primero instalá Steam." "Install Steam first."; exit 1; }
if steam_abierto; then
  # ¿Steam se actualizó después de abrirse (la primera vez siempre) y su navegador nuevo quedó sin el arreglo de la
  # ventana (más abajo)? Entonces su ventana no se ve: se cierra y se abre de nuevo, con el arreglo. Si hay un juego
  # abierto no se toca (se cerraría el juego)
  sin_arreglo=0
  for d in "$STEAMDIR/bin/cef/"cef.win*; do
    [ -f "$d/steamwebhelper.exe" ] && [ "$(stat -f %z "$d/steamwebhelper.exe")" -gt 500000 ] && sin_arreglo=1
  done
  juego=0; ps -axo command= | grep -qi 'steamapps.common' && juego=1
  if [ $sin_arreglo = 0 ] || [ $juego = 1 ]; then
    # Ya abierto: traer su ventana adelante
    WINEDEBUG=-all "$WINE" "C:\\Program Files (x86)\\Steam\\steam.exe" "steam://open/main" >/dev/null 2>&1 < /dev/null &
    disown
    if [ $sin_arreglo = 1 ]; then
      say "Steam se actualizó y su ventana puede no verse: cerrá el juego y tocá «Mostrar Steam» otra vez para reiniciarlo." \
          "Steam updated itself and its window may not show: close the game and press “Show Steam” again to restart it."
    else
      say "Steam ya estaba abierto: mostrando su ventana." "Steam was already open: showing its window."
    fi
    exit 0
  fi
  say "Steam se actualizó y su ventana no se ve: reiniciándolo con el arreglo…" "Steam updated itself and its window can't show: restarting it with the fix…"
  bash "$RES/cerrar.sh" > /dev/null
fi
# Delante del navegador de Steam (steamwebhelper.exe) va un intermediario que le agrega --single-process (de
# ramiabih/play-windows-steam-on-mac, MIT; sin su --disable-gpu: la interfaz de Steam se dibuja con la GPU). El
# original queda como steamwebhelper_real.exe. Se revisa cada vez porque las actualizaciones de Steam lo reemplazan.
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
# Menos carga para Steam (su interfaz es un navegador): sin amigos ni chat, salvo que se
# pidan en VRmin Host › Ajustes (archivo «steam_amigos»: con ellos, Steam usa casi un núcleo de la CPU todo el tiempo)
EXTRA=""; [ -f "$P/steam_amigos" ] || EXTRA="-nofriendsui -nochatui"
# -noverifyfiles: si Steam verifica sus archivos al arrancar, vuelve a poner su steamwebhelper.exe y la ventana
# queda negra otra vez. -cef-single-process: el navegador de Steam en un solo proceso (igual que el arreglo).
EXTRA="$EXTRA -noverifyfiles -cef-single-process"
# Biblioteca de VRmin (Ajustes): Steam de fondo, sin su ventana (-silent), salvo que se pida verla («ventana») o se
# haya elegido la ventana de Steam (archivo «steam_ventana»). Sin una sesión guardada aparece igual, para iniciarla.
if [ "$1" != ventana ] && [ ! -f "$P/steam_ventana" ] && grep -qs '"RememberPassword"[[:space:]]*"1"' "$STEAMDIR/config/loginusers.vdf"; then
  EXTRA="$EXTRA -silent"
fi
ajustes_dll
"$WINE" "C:\\Program Files (x86)\\Steam\\steam.exe" -no-cef-sandbox $EXTRA \
  > "$P/logs/steam.log" 2>&1 < /dev/null &
disown
sleep 2
