#!/bin/bash
# QuestVRBridge - arranca el servidor y el renderizador de la Mac.
cd "$(dirname "$0")"

if [ ! -f cert.pem ] || [ ! -f key.pem ]; then
  echo "Creando certificado local (solo la primera vez)..."
  openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem \
    -days 825 -subj "/CN=QuestVRBridge" >/dev/null 2>&1 || { echo "No se pudo crear el certificado"; exit 1; }
fi

if [ ! -x renderer/QuestRenderer ] || [ renderer/main.swift -nt renderer/QuestRenderer ]; then
  echo "Compilando el renderizador (puede tardar un minuto)..."
  if xcrun swiftc -O renderer/main.swift -o renderer/QuestRenderer.nuevo 2> renderer/compilacion.log; then
    mv renderer/QuestRenderer.nuevo renderer/QuestRenderer
  else
    cat renderer/compilacion.log
    echo ""
    echo ">>> Falló la compilación (detalle en renderer/compilacion.log). Abrí un Issue en GitHub con este detalle."
    if [ -x renderer/QuestRenderer ]; then
      echo ">>> Sigo con la versión anterior del renderizador para que puedas jugar igual."
      touch renderer/QuestRenderer
    else
      exit 1
    fi
  fi
fi

# Valores por defecto (probados en MacBook Air M2): 1024 por ojo, 90 Hz, 70 Mbps
export VR_EYE=${VR_EYE:-1024} VR_FPS=${VR_FPS:-90} VR_MBPS=${VR_MBPS:-70}
/usr/bin/python3 "$(pwd)/server.py" &
SERVER=$!
trap 'kill $SERVER $RENDER 2>/dev/null; exit 0' INT TERM EXIT
sleep 1
./renderer/QuestRenderer &
RENDER=$!
wait $SERVER
