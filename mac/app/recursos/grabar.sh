#!/bin/bash
# Grabar un video (por ejemplo, para un trailer): la pantalla de la Mac y, si el visor está conectado por cable con adb,
# también lo que se ve en el visor. Termina cuando aparece el archivo de parada.
# Uso: grabar.sh <adb o vacío> <carpeta> <archivo de parada>
ADB="$1"; DIR="$2"; STOP="$3"
mkdir -p "$DIR"
stamp=$(date +"%Y-%m-%d %H.%M.%S")
mac="$DIR/VRmin $stamp - Mac.mov"

# Mac: screencapture graba hasta que recibe una tecla; se la mandamos cuando aparece el archivo de parada
( while [ ! -e "$STOP" ]; do sleep 0.4; done; echo q ) | /usr/sbin/screencapture -v -x "$mac" &
macpid=$!

# Visor: screenrecord de Android, en tramos de 3 minutos (su límite), hasta la parada
questpid=""
if [ -n "$ADB" ] && [ -x "$ADB" ]; then
    (
        n=1; avisado=0
        while [ ! -e "$STOP" ]; do
            f="/sdcard/Movies/vrmin_grabacion_$n.mp4"
            t0=$(date +%s)
            "$ADB" shell screenrecord --bit-rate 24000000 --time-limit 180 "$f" &
            rp=$!
            while kill -0 $rp 2>/dev/null; do
                [ -e "$STOP" ] && "$ADB" shell pkill -INT screenrecord >/dev/null 2>&1
                sleep 0.5
            done
            sleep 1
            # Si terminó enseguida, el visor no estaba mostrando nada (pantalla apagada: no lo tenés puesto).
            # No se guarda ese tramo vacío; se vuelve a probar en un rato con el mismo número.
            if [ ! -e "$STOP" ] && [ $(( $(date +%s) - t0 )) -lt 4 ]; then
                "$ADB" shell rm -f "$f" >/dev/null 2>&1
                [ $avisado = 0 ] && echo "Visor: la pantalla está apagada; empieza a grabar cuando te lo pongas" && avisado=1
                sleep 2
                continue
            fi
            dest="$DIR/VRmin $stamp - Visor $n.mp4"
            if "$ADB" pull "$f" "$dest" >/dev/null 2>&1 && [ "$(wc -c < "$dest")" -gt 100000 ]; then
                echo "Grabación del visor: $dest"
                n=$((n + 1)); avisado=0
            else
                rm -f "$dest"
                echo "El tramo $n del visor salió vacío (¿se apagó la pantalla del visor?): no se guardó"
            fi
            "$ADB" shell rm -f "$f" >/dev/null 2>&1
        done
    ) &
    questpid=$!
    echo "Grabando la Mac y el visor…"
else
    echo "Grabando la Mac… (el visor no está conectado por cable con adb: solo se graba la Mac)"
fi

# Esperar la parada; si screencapture no termina solo, se lo corta con Ctrl-C (igual guarda el video)
while [ ! -e "$STOP" ]; do sleep 0.5; done
for _ in $(seq 1 20); do kill -0 $macpid 2>/dev/null || break; sleep 0.25; done
kill -INT $macpid 2>/dev/null
wait $macpid 2>/dev/null
[ -n "$questpid" ] && wait $questpid 2>/dev/null
rm -f "$STOP"
if [ -s "$mac" ]; then echo "Grabación de la Mac: $mac"; else echo "La grabación de la Mac no se guardó (¿falta el permiso de Grabación de pantalla para VRmin Host?)"; fi
