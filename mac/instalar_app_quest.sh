#!/bin/bash
# Instala VRmin en el Quest (conectado por cable USB-C, con modo desarrollador).
# Busca el APK en Descargas (el .zip que baja de GitHub, o el .apk suelto).
cd "$(dirname "$0")"
H="$PWD/herramientas"
ADB="$H/platform-tools/adb"
# Si VRmin Host ya instaló adb (Modo Centinela), usar ese mismo: dos adb de versiones distintas se cierran el servidor
# uno al otro y la primera instalación falla («adb server version doesn't match»)
DATOS="$(defaults read local.questvrbridge.app dataDir 2>/dev/null)"
[ -n "$DATOS" ] && [ -x "$DATOS/herramientas/platform-tools/adb" ] && ADB="$DATOS/herramientas/platform-tools/adb"
if [ ! -x "$ADB" ]; then
  echo "==> Descargando las herramientas oficiales de Android (adb, ≈15 MB)"
  mkdir -p "$H"
  curl -fL --progress-bar -o "$H/pt.zip" "https://dl.google.com/android/repository/platform-tools-latest-darwin.zip" || { echo "No se pudo descargar"; exit 1; }
  (cd "$H" && unzip -q -o pt.zip && rm pt.zip)
  xattr -dr com.apple.quarantine "$H" 2>/dev/null
fi
# Lo más nuevo de Descargas: el .zip que baja de GitHub o un .apk suelto
NUEVO=$(ls -t ~/Downloads/VRmin-quest-apk*.zip ~/Downloads/VRmin*.apk ~/Downloads/QuestVRBridge-quest-apk*.zip ~/Downloads/QuestVRBridge*.apk ~/Downloads/app-debug*.apk 2>/dev/null | head -1)
[ -n "$NUEVO" ] || { echo "No encontré el APK en Descargas. Bajalo de GitHub (Actions → último → VRmin-quest-apk)."; exit 1; }
if [[ "$NUEVO" == *.zip ]]; then
  rm -rf "$H/apk" && mkdir -p "$H/apk" && unzip -q -o "$NUEVO" -d "$H/apk"
  APK=$(ls "$H"/apk/*.apk | head -1)
else
  APK="$NUEVO"
fi
echo "Usando: $NUEVO ($(date -r "$NUEVO" '+%d/%m %H:%M'))"
echo "APK: $APK"
"$ADB" start-server >/dev/null 2>&1
echo "==> Buscando el Quest (conectalo por cable; si en el visor aparece \"¿Permitir depuración USB?\", aceptá)"
for i in $(seq 1 30); do
  st=$("$ADB" devices | awk 'NR>1 && $2!="" {print $2; exit}')
  [ "$st" = "device" ] && break
  [ "$st" = "unauthorized" ] && echo "   Esperando que aceptes en el visor..."
  sleep 2
done
[ "$st" = "device" ] || { echo "No veo el Quest. ¿Está el modo desarrollador activado y el cable conectado?"; exit 1; }
# La versión vieja tenía otro identificador (org.questvrbridge.client): se quita para no tener dos VRmin
if "$ADB" shell pm list packages 2>/dev/null | grep -q "org.questvrbridge.client"; then
  echo "==> Quitando la versión anterior (QuestVRBridge). Los ajustes del visor vuelven a los de fábrica una vez."
  "$ADB" uninstall org.questvrbridge.client >/dev/null 2>&1
fi
echo "==> Instalando"
"$ADB" shell am force-stop com.vrmin.app >/dev/null 2>&1   # (si estaba abierta, se cierra antes de reemplazarla)
for intento in 1 2 3; do
  if "$ADB" install -r "$APK"; then echo "Listo: en el Quest buscala en Biblioteca → Fuentes desconocidas → VRmin"; exit 0; fi
  echo "   Falló el intento $intento; reintentando…"
  "$ADB" wait-for-device; sleep 2
done
echo "No se pudo instalar después de 3 intentos."; exit 1
