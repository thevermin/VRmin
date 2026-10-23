#!/bin/bash
# Firma la app con un certificado propio y fijo, guardado en el llavero de esta Mac.
# Así macOS reconoce que es "la misma app" después de recompilar y no vuelve a pedir
# (ni a rechazar en silencio) los permisos de Grabación de pantalla y Accesibilidad.
# Uso: firmar.sh <ruta a «VRmin Host.app»> <ejecutable extra a firmar>
APP="$1"; EXTRA="$2"
NOMBRE="QuestVRBridge Local"
BUNDLE="local.questvrbridge.app"
LLAVERO="$HOME/Library/Keychains/login.keychain-db"
MARCA="$HOME/Library/Application Support/QuestVRBridge/firma_fija"

tiene_identidad() { security find-identity -p codesigning "$LLAVERO" 2>/dev/null | grep -q "\"$NOMBRE\""; }

if ! tiene_identidad; then
  echo "   Creando el certificado de firma de QuestVRBridge (solo la primera vez)..."
  T=$(mktemp -d)
  cat > "$T/conf" <<CONF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $NOMBRE
[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
CONF
  /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -keyout "$T/key.pem" -out "$T/cert.pem" -config "$T/conf" >/dev/null 2>&1
  /usr/bin/openssl pkcs12 -export -inkey "$T/key.pem" -in "$T/cert.pem" -name "$NOMBRE" -out "$T/id.p12" -passout pass:qvb >/dev/null 2>&1
  security import "$T/id.p12" -k "$LLAVERO" -P qvb -T /usr/bin/codesign >/dev/null 2>&1
  echo "   macOS te va a pedir tu contraseña para confiar en el certificado (es solo para firmar esta app)."
  security add-trusted-cert -r trustRoot -p codeSign -k "$LLAVERO" "$T/cert.pem" 2>/dev/null
  rm -rf "$T"
fi

ID="-"
if tiene_identidad && codesign --force --sign "$NOMBRE" --timestamp=none "$EXTRA" >/dev/null 2>&1; then
  ID="$NOMBRE"
else
  echo "   Aviso: no se pudo usar el certificado propio; firmo sin él (macOS va a volver a pedir los permisos en cada compilación)."
  codesign --force --sign - "$EXTRA" >/dev/null 2>&1
fi
codesign --force --sign "$ID" --timestamp=none "$APP" >/dev/null 2>&1 || echo "Aviso: no se pudo firmar la app (igual debería abrir)"

# La primera vez con la firma fija: borrar los permisos viejos (quedaron atados a la firma anterior
# y macOS los muestra activados pero los rechaza). La app los vuelve a pedir una sola vez.
if [ "$ID" = "$NOMBRE" ] && [ ! -f "$MARCA" ]; then
  tccutil reset ScreenCapture "$BUNDLE" >/dev/null 2>&1
  tccutil reset Accessibility "$BUNDLE" >/dev/null 2>&1
  mkdir -p "$(dirname "$MARCA")"; touch "$MARCA"
  echo "   Permisos viejos borrados: cuando la app los pida, aceptalos una vez más (ya no se van a perder al recompilar)."
fi
