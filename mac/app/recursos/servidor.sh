#!/bin/bash
# Servidor (página del Quest + video/sonido) y renderizador de QuestVRBridge.
RES="$(cd "$(dirname "$0")" && pwd)"
DATA="${QVB_DATA:-$HOME/Library/Application Support/QuestVRBridge}"
say() { if [ "$VRMIN_LANG" = es ]; then echo "$1"; else echo "${2:-$1}"; fi; }
mkdir -p "$DATA"
cd "$DATA"
mkdir -p logs
# Todo lo que dicen el servidor y el renderizador queda también en logs/servidor.log
# (el de la vez anterior queda como servidor_anterior.log: si algo falló, se puede ver después de reiniciar)
[ -s logs/servidor.log ] && mv -f logs/servidor.log logs/servidor_anterior.log
exec > >(tee logs/servidor.log) 2>&1
if [ ! -f cert.pem ] || [ ! -f key.pem ]; then
  say "Creando certificado local (solo la primera vez)..." "Creating a local certificate (first time only)..."
  openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 825 -subj "/CN=QuestVRBridge" >/dev/null 2>&1 \
    || { say "No se pudo crear el certificado" "Couldn't create the certificate"; exit 1; }
fi
export QVB_DATA="$DATA"
export VR_EYE=${VR_EYE:-1024} VR_FPS=${VR_FPS:-90} VR_MBPS=${VR_MBPS:-70}
# Puerto local entre el servidor y el renderizador: 9000, o uno libre si otro programa ya lo usa
export RENDER_PORT=$(/usr/bin/python3 -c '
import socket
s = socket.socket()
try:
    s.bind(("127.0.0.1", 9000))
except OSError:
    s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])')
/usr/bin/python3 "$RES/server.py" &
SERVER=$!
trap 'kill $SERVER $RENDER 2>/dev/null; exit 0' INT TERM EXIT
sleep 1
"$RES/QuestRenderer" &
RENDER=$!
wait $SERVER
