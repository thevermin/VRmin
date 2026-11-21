#!/bin/bash
# Cierra la sesión de Steam en el Steam de VRmin: cierra Steam (y lo que esté abierto en el Windows de VRmin) y borra
# lo que guarda para entrar solo (la cuenta recordada y sus llaves de sesión). La próxima vez que se abre, Steam pide
# iniciar sesión. Los juegos y las partidas no se tocan.
source "$(dirname "$0")/entorno.sh"
cd "$P"
[ -d "$STEAMDIR" ] || { say "Steam no está instalado." "Steam isn't installed."; exit 1; }
say "Cerrando Steam…" "Closing Steam…"
bash "$RES/cerrar.sh" > /dev/null
# (las cuentas recordadas, y las llaves de sesión: «ConnectCache» en config.vdf y local.vdf)
rm -f "$STEAMDIR/config/loginusers.vdf"
rm -f "$WINEPREFIX/drive_c/users/"*/AppData/Local/Steam/local.vdf
"$PY" - "$STEAMDIR/config/config.vdf" <<'PY'
import re, sys
p = sys.argv[1]
try:
    s = open(p, encoding="utf-8", errors="replace").read()
except OSError:
    sys.exit(0)
# el bloque «"ConnectCache" { … }» (sin bloques adentro)
n = re.sub(r'\n[ \t]*"ConnectCache"\s*\{[^{}]*\}', "", s)
if n != s:
    open(p, "w", encoding="utf-8").write(n)
PY
# (y que no intente entrar solo con la última cuenta)
sed -i '' 's/^"AutoLoginUser"=".*"$/"AutoLoginUser"=""/; s/^"RememberPassword"=dword:00000001$/"RememberPassword"=dword:00000000/' "$WINEPREFIX/user.reg" 2>/dev/null
say "Listo: la sesión de Steam está cerrada. Al abrir Steam, pide iniciar sesión." \
    "Done: you're signed out of Steam. When Steam opens, it asks you to sign in."
