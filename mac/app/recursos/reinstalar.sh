#!/bin/bash
# «Reinstalar todo, conservando los juegos»: deja el Windows de VRmin y su motor como recién instalados, sin volver a
# bajar los juegos. Aparta los juegos de Steam (steamapps), los datos de tus cuentas de Steam (userdata: ajustes y
# partidas que Steam guarda en la Mac) y las carpetas de usuario de Windows (donde muchos juegos guardan sus partidas),
# borra el resto, instala el motor y Steam de nuevo, y los devuelve. Los ajustes de VRmin Host se conservan.
# Si algo falla a mitad de camino, lo apartado queda en «<datos>.respaldo» y la próxima vez se usa ese.
source "$(dirname "$0")/entorno.sh"
R="$P.respaldo"
C="$WINEPREFIX/drive_c"
set -o pipefail

say "Cerrando Steam y lo que esté abierto…" "Closing Steam and anything open…"
bash "$RES/cerrar.sh" > /dev/null

# 1) Apartar (si ya hay un respaldo de una vez que no terminó, se usa ese y no se pisa)
mkdir -p "$R"
apartar() {   # origen, nombre en el respaldo
  if [ -e "$R/$2" ]; then return 0; fi
  if [ -e "$1" ]; then mv "$1" "$R/$2" || { say "No pude apartar $1" "Couldn't move aside $1"; exit 1; }; fi
}
apartar "$STEAMDIR/steamapps" steamapps
apartar "$STEAMDIR/userdata" userdata
apartar "$C/users" users
say "Juegos y partidas apartados en $R" "Games and saves moved aside to $R"

# 2) Borrar el Windows de VRmin, el motor y lo que se calcula solo (los ajustes de VRmin Host y de cada juego quedan)
say "Borrando el Windows de VRmin y el motor…" "Deleting VRmin's Windows and the engine…"
rm -rf "$WINEPREFIX" "$MOTOR" "$P/perfiles/detectado" "$P/lanzando.txt"

# 3) Instalar de nuevo
bash "$RES/instalar_motor.sh" || { say "No se pudo instalar el motor: tus juegos siguen en $R. Probá de nuevo." \
  "Couldn't install the engine: your games are still in $R. Try again."; exit 1; }
bash "$RES/instalar_steam.sh" || { say "No se pudo instalar Steam: tus juegos siguen en $R. Probá de nuevo." \
  "Couldn't install Steam: your games are still in $R. Try again."; exit 1; }
"$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w 2>/dev/null

# 4) Devolver
devolver() {   # nombre en el respaldo, destino
  [ -e "$R/$1" ] || return 0
  rm -rf "$2" && mkdir -p "$(dirname "$2")" && mv "$R/$1" "$2" || { say "No pude devolver $1 (sigue en $R)" "Couldn't put back $1 (it's still in $R)"; exit 1; }
}
devolver steamapps "$STEAMDIR/steamapps"
devolver userdata "$STEAMDIR/userdata"
devolver users "$C/users"
rmdir "$R" 2>/dev/null
say "Listo: VRmin está como recién instalado, con tus juegos. Abrí Steam e iniciá sesión (la primera vez se actualiza y puede revisar los juegos un rato)." \
    "Done: VRmin is like freshly installed, with your games. Open Steam and sign in (the first time it updates itself and may check the games for a while)."
