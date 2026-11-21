#!/bin/bash
# Pausa, sigue o cancela lo que el Steam de VRmin está bajando: descarga.sh <appid> pausar|seguir|cancelar.
# Steam no deja hacerlo desde afuera sin su ventana, así que se hace como al instalar sin ventana: se cierra Steam de
# fondo, se cambia el «appmanifest» del juego (StateFlags 512 = en pausa) o se borra lo bajado, y se abre de nuevo.
# Cancelar un juego que no estaba instalado borra también su carpeta (lo bajado hasta ahí); si era una actualización,
# el juego queda como estaba (Steam la vuelve a pedir más adelante).
source "$(dirname "$0")/entorno.sh"
cd "$P"
ID="$1"; QUE="$2"
[[ "$ID" =~ ^[0-9]+$ ]] || { say "Falta el número del juego." "The game's number is missing."; exit 1; }
# (en la biblioteca de Steam donde se está bajando: la de VRmin u otra que se eligió al instalar)
while IFS= read -r b; do [ -f "$b/appmanifest_$ID.acf" ] && { STEAMLIB="$b"; break; }; done < <("$PY" "$RES/vrjuegos.py" bibliotecas)
ACF="$STEAMLIB/appmanifest_$ID.acf"
[ -f "$ACF" ] || { say "Ese juego no se está bajando." "That game isn't downloading."; exit 1; }
if ps -axo command= | grep -qi '[s]teamapps.common'; then
  say "Primero cerrá el juego que está abierto (Steam tiene que reiniciarse)." "Close the open game first (Steam has to restart)."; exit 1
fi
valor() { sed -n "s/^[[:space:]]*\"$1\"[[:space:]]*\"\(.*\)\"[[:space:]]*$/\1/p" "$ACF" | head -1; }
NOMBRE=$(valor name); FLAGS=$(valor StateFlags); CARP=$(valor installdir)
FLAGS=${FLAGS:-0}
abierto=0; steam_abierto && abierto=1
[ $abierto = 1 ] && bash "$RES/cerrar.sh" > /dev/null
case "$QUE" in
  pausar|seguir)
    if [ "$QUE" = pausar ]; then N=$(( FLAGS | 512 )); else N=$(( FLAGS & ~512 )); fi
    sed -i '' "s/^\([[:space:]]*\"StateFlags\"[[:space:]]*\"\)[0-9]*\"/\1$N\"/" "$ACF"
    [ "$QUE" = pausar ] && say "$NOMBRE: descarga en pausa." "$NOMBRE: download paused." \
                        || say "$NOMBRE: la descarga sigue." "$NOMBRE: download resumed."
    ;;
  cancelar)
    rm -rf "$STEAMLIB/downloading/$ID" "$STEAMLIB/temp/$ID"
    if [ $(( FLAGS & 4 )) = 0 ]; then
      # (no estaba instalado: se va todo; la carpeta solo si su nombre es uno solo, sin «/» ni «..»)
      rm -f "$ACF"
      if [ -n "$CARP" ] && [[ "$CARP" != */* ]] && [ "$CARP" != . ] && [ "$CARP" != .. ]; then
        rm -rf "$STEAMLIB/common/$CARP"
      fi
      say "$NOMBRE: descarga cancelada y borrada." "$NOMBRE: download canceled and deleted."
    else
      # (una actualización: el juego queda instalado como estaba)
      N=$(( (FLAGS & ~(256 | 512 | 1024)) | 4 ))
      sed -i '' "s/^\([[:space:]]*\"StateFlags\"[[:space:]]*\"\)[0-9]*\"/\1$N\"/" "$ACF"
      say "$NOMBRE: actualización cancelada (el juego queda como estaba)." "$NOMBRE: update canceled (the game stays as it was)."
    fi
    ;;
  *) say "Uso: descarga.sh <número> pausar|seguir|cancelar" "Usage: descarga.sh <number> pausar|seguir|cancelar"; exit 1 ;;
esac
# (Steam se abre de nuevo de fondo si estaba abierto, o para seguir bajando)
if [ $abierto = 1 ] || [ "$QUE" = seguir ]; then bash "$RES/abrir_steam.sh" > /dev/null; fi
exit 0
