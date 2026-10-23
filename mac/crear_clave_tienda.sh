#!/bin/bash
# Crea la clave para firmar VRmin en la tienda de Meta (una sola vez) y la sube a los secretos de GitHub.
#   bash crear_clave_tienda.sh
# La clave queda en ~/Documents/VRmin-claves (NO la subas al repositorio ni la pierdas:
# sin ella no se puede publicar ninguna actualización de la app).
set -e
D="$HOME/Documents/VRmin-claves"
KS="$D/vrmin-tienda.jks"
ALIAS="vrmin"
if [ -f "$KS" ]; then echo "Ya existe $KS (no la piso)."; exit 1; fi
if ! /usr/libexec/java_home >/dev/null 2>&1; then
  echo "Falta Java para crear la clave. Instalalo con:  brew install --cask temurin   (o desde adoptium.net) y volvé a correr esto."
  exit 1
fi
mkdir -p "$D"; chmod 700 "$D"
PASS=$(openssl rand -base64 24 | tr -d '/+=' | cut -c1-24)
keytool -genkeypair -v -keystore "$KS" -alias "$ALIAS" -keyalg RSA -keysize 4096 -validity 10000 \
  -storepass "$PASS" -keypass "$PASS" -dname "CN=VRmin, O=VRmin" >/dev/null
cat > "$D/LEEME.txt" <<TXT
Clave de VRmin para la tienda de Meta — GUARDALA (copia en un lugar seguro). Sin ella no se puede actualizar la app publicada.
Archivo: $KS
Alias: $ALIAS
Contraseña (almacén y clave): $PASS
TXT
chmod 600 "$KS" "$D/LEEME.txt"
echo "Clave creada en $D (los datos están en LEEME.txt)."
if command -v gh >/dev/null 2>&1; then
  echo "Subiendo los secretos a GitHub (para que GitHub Actions firme la versión de la tienda)…"
  base64 -i "$KS" | gh secret set VRMIN_KEYSTORE_B64 -R thevermin/vrmin
  printf '%s' "$PASS" | gh secret set VRMIN_KEYSTORE_PASSWORD -R thevermin/vrmin
  printf '%s' "$ALIAS" | gh secret set VRMIN_KEY_ALIAS -R thevermin/vrmin
  printf '%s' "$PASS" | gh secret set VRMIN_KEY_PASSWORD -R thevermin/vrmin
  echo "Listo. Cuando tengas el App ID de la tienda:  gh secret set VRMIN_META_APP_ID -R thevermin/vrmin"
else
  echo "Para que GitHub firme con esta clave, cargá en el repositorio (Settings → Secrets → Actions):"
  echo "  VRMIN_KEYSTORE_B64 = (el resultado de: base64 -i \"$KS\")"
  echo "  VRMIN_KEYSTORE_PASSWORD y VRMIN_KEY_PASSWORD = la contraseña de LEEME.txt · VRMIN_KEY_ALIAS = $ALIAS"
  echo "  VRMIN_META_APP_ID = el App ID de la tienda (cuando lo tengas)"
fi
