#!/bin/bash
# Instala (o quita, con --quitar) el micrófono virtual «VRmin Mic». Lo corre VRmin Host como administrador
# (macOS pide la contraseña): los componentes de audio van en /Library/Audio/Plug-Ins/HAL y el audio de macOS se
# reinicia para verlo (se corta el sonido un par de segundos).
set -e
DEST="/Library/Audio/Plug-Ins/HAL/VRminMic.driver"
if [ "$1" = "--quitar" ]; then
  rm -rf "$DEST"
else
  [ -d "$1" ] || { echo "Falta VRminMic.driver"; exit 1; }
  rm -rf "$DEST"
  cp -R "$1" "$DEST"
  chown -R root:wheel "$DEST"
  chmod -R go-w "$DEST"
fi
killall coreaudiod 2>/dev/null || launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || true
