#!/bin/bash
# Le pide al Steam de VRmin que instale un juego (desde la Tienda de VRmin Host): instalar_juego.sh <appid> [nombre].
# Con «desinstalar» al final, que lo desinstale (Steam pregunta antes).
# Lo abre primero si está cerrado. Steam muestra su ventana de instalación en la Mac (y, si el juego no es tuyo, su
# página para comprarlo); el juego aparece en la Biblioteca apenas empieza a bajar.
source "$(dirname "$0")/entorno.sh"
cd "$P"
ID="$1"; NOMBRE="${2:-$1}"; QUE="${3:-install}"; [ "$QUE" = desinstalar ] && QUE=uninstall
[[ "$ID" =~ ^[0-9]+$ ]] || { say "Falta el número del juego." "The game's number is missing."; exit 1; }
# Instalar sin la ventana de Steam (como GameHub): si el juego es tuyo, se le deja a Steam su «appmanifest» como «hay
# que bajarlo» y Steam lo baja solo al abrirse (si está abierto se reinicia de fondo; no si hay un juego abierto). Si no
# es tuyo, o hay un juego abierto, Steam muestra su ventana de siempre (la de comprar o la de instalar).
es_tuyo() {   # (está en la configuración de alguna cuenta de este Steam: los juegos de su biblioteca)
  grep -qs "^[[:space:]]*\"$1\"[[:space:]]*\$" "$STEAMDIR"/userdata/*/config/localconfig.vdf
}
if [ "$QUE" = install ]; then
  if [ -f "$STEAMLIB/appmanifest_$ID.acf" ]; then
    say "$NOMBRE ya está en Steam (instalado o bajando)." "$NOMBRE is already in Steam (installed or downloading)."; exit 0
  fi
  if es_tuyo "$ID" && ! ps -axo command= | grep -qi 'steamapps.common'; then
    CARP=$(printf '%s' "$NOMBRE" | tr -d ':*?"<>|/\\' | sed 's/[[:space:]]*$//')
    [ -n "$CARP" ] || CARP="$ID"
    printf '"AppState"\n{\n\t"appid"\t\t"%s"\n\t"Universe"\t\t"1"\n\t"name"\t\t"%s"\n\t"StateFlags"\t\t"1026"\n\t"installdir"\t\t"%s"\n\t"LastUpdated"\t\t"0"\n\t"SizeOnDisk"\t\t"0"\n\t"buildid"\t\t"0"\n\t"BytesToDownload"\t\t"0"\n\t"BytesDownloaded"\t\t"0"\n\t"AutoUpdateBehavior"\t\t"0"\n\t"AllowOtherDownloadsWhileRunning"\t\t"0"\n\t"ScheduledAutoUpdate"\t\t"0"\n\t"UserConfig"\n\t{\n\t}\n\t"MountedConfig"\n\t{\n\t}\n}\n' \
      "$ID" "$NOMBRE" "$CARP" > "$STEAMLIB/appmanifest_$ID.acf"
    if steam_abierto; then
      say "Reiniciando Steam de fondo para que empiece a bajar $NOMBRE…" "Restarting Steam in the background so it starts downloading $NOMBRE…"
      bash "$RES/cerrar.sh" > /dev/null
    fi
    bash "$RES/abrir_steam.sh" || exit 1
    say "Bajando $NOMBRE: el avance se ve en la barra de abajo (empieza cuando Steam termina de abrir)." \
        "Downloading $NOMBRE: you can follow it in the bar at the bottom (it starts once Steam finishes opening)."
    exit 0
  fi
fi
if ! steam_abierto; then
  bash "$RES/abrir_steam.sh" || exit 1
  for i in $(seq 1 75); do steam_abierto && break; sleep 2; done
  steam_abierto || { say "Steam no terminó de abrir: probá de nuevo en un momento." "Steam didn't finish opening: try again in a moment."; exit 1; }
  say "Steam abierto, esperando que termine de cargar…" "Steam is open, waiting for it to finish loading…"
  sleep 25
fi
WINEDEBUG=-all "$WINE" "C:\\Program Files (x86)\\Steam\\steam.exe" "steam://$QUE/$ID" >/dev/null 2>&1 < /dev/null &
disown
if [ "$QUE" = uninstall ]; then
  say "Steam pregunta en la Mac si desinstalar $NOMBRE." "Steam asks on the Mac whether to uninstall $NOMBRE."
else
  say "Steam muestra la instalación de $NOMBRE en la Mac: tocá «Instalar» ahí." \
      "Steam shows $NOMBRE's installation on the Mac: click “Install” there."
fi
