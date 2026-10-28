#!/bin/bash
# Wine con el arreglo de errores cargado (ver entorno.sh): solo los procesos de Wine lo llevan
export DYLD_INSERT_LIBRARIES="$QVB_ARREGLO"
exec "$WINE_REAL" "$@"
