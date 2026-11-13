#!/bin/bash
# Rosetta 2 de Apple (el motor de juegos de Windows es de Intel). macOS pide la contraseña de la Mac en su propia
# ventana. Lo usan el botón de VRmin Host y instalar_motor.sh.
say() { if [ "$VRMIN_LANG" = es ]; then echo "$1"; else echo "${2:-$1}"; fi; }
arch -x86_64 /usr/bin/true 2>/dev/null && { say "Rosetta 2 ya está instalada." "Rosetta 2 is already installed."; exit 0; }
say "Instalando Rosetta 2: la Mac te va a pedir tu contraseña…" "Installing Rosetta 2: your Mac will ask for your password…"
PROMPT=$(say "VRmin necesita Rosetta 2 de Apple para los juegos de Windows." "VRmin needs Apple's Rosetta 2 to run Windows games.")
osascript -e "do shell script \"/usr/sbin/softwareupdate --install-rosetta --agree-to-license\" with prompt \"$PROMPT\" with administrator privileges" || true
arch -x86_64 /usr/bin/true 2>/dev/null && { say "Rosetta 2 instalada." "Rosetta 2 installed."; exit 0; }
say "Falta Rosetta 2. Instalala con:  softwareupdate --install-rosetta --agree-to-license" "Rosetta 2 is missing. Install it with:  softwareupdate --install-rosetta --agree-to-license"
exit 1
