#!/bin/bash
# Baja el Steam nativo de Linux ARM64 (el del Steam Frame) de los servidores de actualización de Valve, como su
# instalador, para abrirlo bajo Burrow (probar.sh con STEAM=<esta carpeta>/Steam).
#   steam-arm64.sh [carpeta]   (por defecto ./steam-arm64)
# Deja Steam en <carpeta>/Steam y su carpeta personal en <carpeta>/home (con ~/.steam/sdkarm64, donde los juegos
# buscan steamclient.so).
set -euo pipefail
DIR="${1:-$PWD/steam-arm64}"; CDN=https://client-update.steamstatic.com
mkdir -p "$DIR/.paquetes" "$DIR/Steam" "$DIR/home/.steam"
curl -fsSL -o "$DIR/.paquetes/manifiesto.vdf" "$CDN/steam_client_linuxarm64"
# (los paquetes nativos ARM64 y los comunes a todas las plataformas; los «_steamrt» son los de x86, que en ARM corren
#  con FEX, y no hacen falta)
grep -o '"file"[[:space:]]*"[^"]*"' "$DIR/.paquetes/manifiesto.vdf" | sed 's/.*"\([^"]*\)"$/\1/' \
  | grep -E '_all\.zip\.|_linuxarm64_linuxarm64\.zip\.' > "$DIR/.paquetes/lista"
while read -r f; do
  [ -s "$DIR/.paquetes/$f" ] || { echo "Bajando $f"; curl -fsSL -o "$DIR/.paquetes/$f" "$CDN/$f"; }
done < "$DIR/.paquetes/lista"
# Los zip de Steam usan «\» en algunas rutas y guardan enlaces: se abren con Python para respetar las dos cosas
python3 -I - "$DIR/Steam" "$DIR/.paquetes" <<'EOF'
import os, sys, zipfile
dest, paq = os.path.abspath(sys.argv[1]), sys.argv[2]
for name in open(os.path.join(paq, 'lista')).read().split():
    with zipfile.ZipFile(os.path.join(paq, name)) as z:
        for i in z.infolist():
            ruta = os.path.normpath(os.path.join(dest, i.filename.replace('\\', '/')))
            if not ruta.startswith(dest + os.sep): continue
            if i.filename.endswith(('/', '\\')): os.makedirs(ruta, exist_ok=True); continue
            os.makedirs(os.path.dirname(ruta), exist_ok=True)
            modo = i.external_attr >> 16
            if (modo & 0o170000) == 0o120000:
                if os.path.lexists(ruta): os.remove(ruta)
                os.symlink(z.read(i).decode(), ruta)
            else:
                datos = z.read(i)
                with open(ruta, 'wb') as out: out.write(datos)
                # (los zip no guardan bien los permisos: programas y scripts, ejecutables)
                os.chmod(ruta, 0o755 if datos[:4] == b'\x7fELF' or datos[:2] == b'#!' else 0o644)
EOF
ln -sfn ../../Steam/linuxarm64 "$DIR/home/.steam/sdkarm64"
ln -sfn ../../Steam "$DIR/home/.steam/steam"
echo "Steam ARM64 listo en $DIR/Steam ($(du -sh "$DIR/Steam" | cut -f1))"
