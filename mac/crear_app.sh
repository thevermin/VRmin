#!/bin/bash
# Arma «VRmin Host.app» con todo adentro (scripts, servidor, renderizador, runtime) y la abre.
#   bash crear_app.sh            en tu Mac: firma con el certificado local fijo (macOS recuerda los permisos) y la abre
#   bash crear_app.sh --paquete  para distribuir: firma «ad hoc» (sin llavero), no la abre, y deja «VRmin Host <versión>.zip»
cd "$(dirname "$0")"
PAQUETE=0; [ "$1" = "--paquete" ] && PAQUETE=1
APP="VRmin Host.app"
NUEVA="VRmin Host.nueva.app"
VIEJA="QuestVRBridge.app"   # nombre anterior de la app
R="$NUEVA/Contents/Resources/qvb"
falla() { cat "$1"; echo ""; echo ">>> Falló la compilación (detalle en $1). Abrí un Issue en GitHub con este detalle."; rm -rf "$NUEVA"; exit 1; }

# Compila con Swift; si las herramientas de línea de comandos son más nuevas que su propio SwiftUI (pasa con las de
# macOS 27: «plugin for module 'SwiftUIMacros' not found»), prueba con el SDK anterior que vienen con ellas
swc() {
  local log=$1; shift
  xcrun swiftc "$@" 2> "$log" && return 0
  grep -q "plugin for module 'SwiftUIMacros'" "$log" || return 1
  local sdk; sdk=$(ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX26*.sdk 2>/dev/null | sort | tail -1)
  [ -n "$sdk" ] || return 1
  echo "   (las herramientas de Apple no traen el SwiftUI nuevo completo: compilando con $(basename "$sdk"))"
  xcrun swiftc -sdk "$sdk" "$@" 2> "$log"
}

rm -rf "$NUEVA"
mkdir -p "$NUEVA/Contents/MacOS" "$R/web" "$R/openxr"
cp app/Info.plist "$NUEVA/Contents/Info.plist"
echo "1/3 Compilando la app..."
swc app/compilacion.log -O -parse-as-library app/VRminHostApp.swift app/Idioma.swift app/Tutoriales.swift app/LogEn.swift -o "$NUEVA/Contents/MacOS/VRminHost" || falla app/compilacion.log
echo "2/3 Compilando el renderizador..."
swc renderer/compilacion.log -O renderer/main.swift -o "$R/QuestRenderer" || falla renderer/compilacion.log
echo "3/3 Copiando archivos..."
xcrun clang -arch x86_64 -arch arm64 -O2 -dynamiclib app/recursos/arreglo_errores.c -o "$R/arreglo_errores.dylib" 2> app/compilacion_arreglo.log || falla app/compilacion_arreglo.log
cp app/recursos/*.sh app/recursos/*.py app/recursos/*.json "$R/"
cp app/recursos/LICENCIAS_DE_TERCEROS.md "$R/"
mkdir -p "$R/steam" && cp app/recursos/steam/steamwebhelper.exe "$R/steam/"
cp server.py "$R/"
cp web/index.html "$R/web/"
cp openxr/questvrbridge_openxr.dll openxr/questvrbridge_openxr.json "$R/openxr/"
chmod +x "$R"/*.sh "$R/QuestRenderer"
# Ícono (a partir de app/AppIcon.png)
I="$(mktemp -d)/AppIcon.iconset"; mkdir -p "$I"
for s in 16 32 128 256 512; do
  sips -z $s $s app/AppIcon.png --out "$I/icon_${s}x${s}.png" >/dev/null
  sips -z $((s*2)) $((s*2)) app/AppIcon.png --out "$I/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$I" -o "$NUEVA/Contents/Resources/AppIcon.icns" || echo "Aviso: no se pudo armar el ícono"
if [ $PAQUETE = 1 ]; then
  # Sin cuenta de desarrollador de Apple: quien la baje la abre con clic derecho → Abrir.
  codesign --force --sign - "$R/arreglo_errores.dylib" || { echo "Falló la firma"; exit 1; }
  if [ "$VRMIN_ADHOC" = 1 ]; then
    # (GitHub Actions: firma «ad hoc», solo para probar que compila)
    codesign --force --sign - "$R/QuestRenderer" && codesign --force --deep --sign - "$NUEVA" || { echo "Falló la firma"; exit 1; }
  else
    # Con el certificado fijo de VRmin (el mismo de cada versión): así macOS conserva los permisos al actualizar
    bash app/firmar.sh "$NUEVA" "$R/QuestRenderer"
    codesign -dvv "$NUEVA" 2>&1 | grep -q "Authority=QuestVRBridge Local" || { echo "Falló la firma con el certificado de VRmin"; exit 1; }
  fi
  # (en una carpeta aparte: la app instalada en esta Mac, con firma local, queda igual y conserva sus permisos)
  T=$(mktemp -d); mv "$NUEVA" "$T/$APP"
  V=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$T/$APP/Contents/Info.plist")
  ditto -c -k --keepParent "$T/$APP" "VRmin Host $V.zip"; rm -rf "$T"
  echo "Paquete listo: $(pwd)/VRmin Host $V.zip"
  exit 0
fi
# Firma local fija (sin cuenta de desarrollador) para que macOS recuerde los permisos entre compilaciones
bash app/firmar.sh "$NUEVA" "$R/QuestRenderer"
# Cerrar la app anterior (con cualquiera de los dos nombres) antes de reemplazarla
osascript -e 'quit app id "local.questvrbridge.app"' >/dev/null 2>&1; sleep 1
rm -rf "$APP" "$VIEJA"
mv "$NUEVA" "$APP"
echo "Listo: $(pwd)/$APP  (pesa $(du -sh "$APP" | cut -f1); los datos van aparte)"
open "$APP"
