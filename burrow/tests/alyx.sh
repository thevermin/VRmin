#!/bin/bash
# Half-Life: Alyx para Linux ARM64 (desde la actualización para Steam Frame, sus programas ARM64 vienen en la versión
# de Linux): baja solo sus programas y bibliotecas (no los ~60 GB del juego) y dice qué necesita cada uno, para saber
# qué le falta a Burrow para abrirlo.
#   alyx.sh <DepotDownloader> <usuario de Steam> [carpeta]   (la contraseña la escribís vos; o -qr en lugar del usuario)
# Necesita GNU objdump («brew install binutils»). Deja el informe en <carpeta>/alyx-informe.txt
set -euo pipefail
DD="$1"; USUARIO="$2"; DIR="${3:-$PWD/alyx-linux}"
mkdir -p "$DIR"
LISTA="$DIR/.lista"
# (los programas del juego y todas sus bibliotecas)
printf '%s\n' 'regex:^game/bin/' 'regex:\.so(\.[0-9.]+)?$' 'regex:\.json$' > "$LISTA"
if [ "$USUARIO" = "-qr" ]; then LOGIN=(-qr); else LOGIN=(-username "$USUARIO" -remember-password); fi
"$DD" -app 546560 -os linux -osarch 64 "${LOGIN[@]}" -filelist "$LISTA" -dir "$DIR"
for d in /opt/homebrew/opt/binutils/bin /usr/local/opt/binutils/bin; do [ -d "$d" ] && PATH="$d:$PATH"; done
OBJ=$(command -v gobjdump || command -v objdump)
INF="$DIR/alyx-informe.txt"
{
  echo "== Programas y bibliotecas ARM64 (aarch64) de Half-Life: Alyx para Linux"
  find "$DIR" -type f ! -path '*/.DepotDownloader/*' | while IFS= read -r f; do
    head -c 20 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n' | grep -q '^7f454c46' || continue   # (solo ELF)
    arq=$("$OBJ" -f "$f" 2>/dev/null | sed -n 's/.*file format //p' | head -1)
    [ "$arq" = "elf64-littleaarch64" ] || continue
    echo "-- ${f#$DIR/}"
    "$OBJ" -p "$f" 2>/dev/null | awk '/NEEDED/ { print "   necesita: " $2 }'
  done
  echo
  echo "== Bibliotecas que piden y no vienen con el juego (las tiene que dar el Linux de Burrow)"
} > "$INF.tmp"
find "$DIR" -type f -name '*.so*' -exec basename {} \; | sort -u > "$DIR/.trae"
grep 'necesita:' "$INF.tmp" | awk '{ print $2 }' | sort -u | comm -23 - "$DIR/.trae" | sed 's/^/   /' >> "$INF.tmp"
mv "$INF.tmp" "$INF"
cat "$INF"
