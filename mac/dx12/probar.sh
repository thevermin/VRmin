#!/bin/bash
# VRmin · prueba de DirectX 12 en el motor (experimental): Vulkan con KosmicKrisp (x86_64, para el Wine de Rosetta) y
# D3D12 de Wine (vkd3d) o vkd3d-proton, sobre MoltenVK o KosmicKrisp. No cambia nada del motor ni del Windows de VRmin:
# todo va en esta carpeta y con variables de entorno solo para cada prueba. El resultado queda en resultado.txt.
cd "$(dirname "$0")" || exit 1
D="$PWD"
P="$HOME/Documents/QuestVRBridge/prototipo"
WINEDIR="$P/motor/wswine.bundle"
export VKD3D_DEBUG=warn MVK_CONFIG_LOG_LEVEL=0 WINEPREFIX="$P/prefijo" WINEMSYNC=1 WINEDEBUG=-all,err+module,err+d3d12,err+vkd3d
FW="$P/motor/Frameworks"
BASE="$WINEDIR/lib:$FW:/usr/lib"
OUT="$D/resultado.txt"
: > "$OUT"
sw_vers >> "$OUT"
xattr -dr com.apple.quarantine "$D" 2>/dev/null

echo "== Vulkan con KosmicKrisp (vulkaninfo, x86_64)" | tee -a "$OUT"
DYLD_FALLBACK_LIBRARY_PATH="$D/vk" VK_ICD_FILENAMES="$D/kk/kosmickrisp_icd.json" arch -x86_64 "$D/vk/vulkaninfo" --summary >> "$OUT" 2>&1
DYLD_FALLBACK_LIBRARY_PATH="$D/vk" VK_ICD_FILENAMES="$D/kk/kosmickrisp_icd.json" arch -x86_64 "$D/vk/vulkaninfo" 2>/dev/null \
  | grep -E "transform_feedback|geometryShader|descriptor_buffer|mutable_descriptor|shaderInt64 |descriptorIndexing|sparseBinding " | sort -u >> "$OUT"
echo "(salió con $?)" >> "$OUT"

# La dxgi de Wine (la de DXMT no hace cadenas de presentación de D3D12), al lado de la prueba
mkdir -p "$D/run"
cp "$D/prueba_d3d12.exe" "$D/run/"
cp "$WINEDIR/lib/wine/x86_64-windows/dxgi.dll.wine-original" "$D/run/dxgi.dll" 2>/dev/null \
  || echo "(no encontré dxgi.dll.wine-original)" >> "$OUT"
# (sin la marca de «Wine builtin DLL»: si no, Wine busca la incorporada, que es la de DXMT, y no la carga)
/usr/bin/python3 -c "import sys; p=sys.argv[1]; d=open(p,'rb').read(); open(p,'wb').write(d.replace(b'Wine builtin DLL', bytes(16)))" "$D/run/dxgi.dll"
mv "$D/run/dxgi.dll" "$D/run/dxgi-wine.dll"

prueba() {   # nombre, vulkan (moltenvk/kk), d3d12 (wine/proton), variable extra
  echo "== $1" | tee -a "$OUT"
  rm -f "$D/run/d3d12.dll" "$D/run/d3d12core.dll"
  local ov="dxgi=n"
  cp "$D/run/dxgi-wine.dll" "$D/run/dxgi.dll"
  # (vkd3d-proton arma sus ventanas con la dxgi de DXVK: la de Wine le pide algo que no tiene)
  if [ "$3" = proton ]; then cp "$D/vkd3d-proton/d3d12.dll" "$D/vkd3d-proton/d3d12core.dll" "$D/run/"; cp "$D/dxvk/dxgi.dll" "$D/run/dxgi.dll"; ov="dxgi,d3d12,d3d12core=n"; fi
  (
    export WINEDLLOVERRIDES="$ov;winemenubuilder.exe=d"
    [ -n "$4" ] && export "$4"
    if [ "$2" = kk ]; then
      # (DYLD_LIBRARY_PATH: le gana a la libMoltenVK.dylib del motor, que Wine encuentra antes que el FALLBACK)
      export DYLD_LIBRARY_PATH="$D/vk" DYLD_FALLBACK_LIBRARY_PATH="$BASE" VK_ICD_FILENAMES="$D/kk/kosmickrisp_icd.json"
    else
      export DYLD_FALLBACK_LIBRARY_PATH="$BASE"
    fi
    cd "$D/run" && "$WINEDIR/bin/wine" prueba_d3d12.exe 5 >> "$OUT" 2>&1 &
    p=$!; for _ in $(seq 1 40); do kill -0 $p 2>/dev/null || break; sleep 1; done
    kill -0 $p 2>/dev/null && { echo "(se colgó: la cierro)" >> "$OUT"; "$WINEDIR/bin/wineserver" -k; }
    wait $p 2>/dev/null
  )
  "$WINEDIR/bin/wineserver" -w
}
"$WINEDIR/bin/wineserver" -k 2>/dev/null
prueba "D3D12 de Wine (vkd3d) + MoltenVK" moltenvk wine
prueba "D3D12 de Wine (vkd3d) + KosmicKrisp" kk wine
prueba "vkd3d-proton + KosmicKrisp" kk proton
prueba "vkd3d-proton + KosmicKrisp, pidiendo el nivel 12_0" kk proton VKD3D_FEATURE_LEVEL=12_0
echo "Listo: $OUT"
