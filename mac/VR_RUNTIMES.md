# Runtimes de VR en VRmin Host

Cómo llega un juego de PC VR (Windows, dentro de Wine) hasta el Quest, y cómo se elige el camino de cada juego.

## Los caminos

| Lo que usa el juego | Camino | Estado |
|---|---|---|
| **OpenXR** | juego → cargador OpenXR → `questvrbridge_openxr.dll` (nuestro runtime) → Mac → Quest | Funciona (solo DirectX 11) |
| **OpenVR / SteamVR** | juego → `openvr_api.dll` → OpenComposite (`vrclient_x64.dll`) → OpenXR → nuestro runtime | Funciona |
| **SteamVR real** | juego → SteamVR de Valve (`vrserver.exe`) → driver de VRmin (`driver_vrmin`) → Quest | Preparado: falta escribir el driver |
| **Oculus (LibOVR)** | juego → ReVive → OpenVR → OpenComposite → nuestro runtime | Preparado: falta incluir ReVive |
| **Sin VR** | sin runtime: el juego no encuentra visor (Unity `-vrmode None`, Unreal `-nohmd`) | Funciona |

## Dónde se elige el runtime

`app/recursos/vrjuegos.py` decide, para cada juego, en este orden:
1. lo que eligió el usuario en la app de la Mac (menú de cada juego) → `DATOS/perfiles/<appid>.json`
2. lo conocido de ese juego → `app/recursos/perfiles.json` (viene con la app)
3. lo detectado mirando sus archivos → `DATOS/perfiles/detectado/<appid>.json` (caché; se rehace si Steam actualiza el juego)

Detección (`detectar`): `openxr_loader.dll` → OpenXR · `openvr_api.dll` → OpenVR · `OVRPlugin.dll`/`LibOVRRT*` → Oculus;
`UnityPlayer.dll` → Unity · `*-Win64-Shipping.exe` → Unreal; `vulkan-1.dll`, `D3D12Core.dll` → avisos de gráficos
(nuestro runtime OpenXR solo tiene DirectX 11). Si no encuentra nada, prueba OpenVR primero.

En **Automático**, si la vez anterior el juego no mandó ninguna imagen con un runtime (OpenXR u OpenVR), la próxima vez
se prueba el otro (si el juego trae esa API o no se encontró ninguna); el que anduvo queda elegido
(`DATOS/perfiles/intentos.json`: lo escribe `jugar.sh` al abrir y VRmin Host al recibir el primer cuadro).

A Unity y Unreal se les pide siempre DirectX 11 (`-force-d3d11` / `-dx11`), lo único que recibe nuestro runtime
(también a través de OpenComposite). Se apaga por juego con `"dx11": false` en su perfil.

Si se pide algo que todavía no existe (SteamVR real sin driver, Oculus sin ReVive), se usa OpenVR y se avisa.

## Cómo se aplica (`app/recursos/runtime.sh`)

Los juegos se abren desde Steam, así que **no heredan variables de entorno**: el runtime se elige cambiando el Windows de VRmin:
- OpenXR: registro `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime` (solo se reescribe si cambió el modo).
- OpenVR: `AppData\Local\openvr\openvrpaths.vrpath` (carpeta del runtime y drivers externos).

## Abrir un juego (`app/recursos/jugar.sh APPID`)

- **Desde Steam** (normal): `steam.exe -applaunch APPID args`.
- **Directo** (para probar): ejecuta el `.exe` del juego con Wine; así sí recibe variables de entorno (`env` del perfil)
  y el **diagnóstico** (`WINEDEBUG` detallado y `XR_LOADER_DEBUG=all`). Crea `steam_appid.txt` si falta. Steam tiene que estar abierto.

Cada vez que se abre un juego queda un resumen en `DATOS/logs/juegos/<appid>.log` (qué se detectó, qué runtime, avisos y,
si se abrió directo, todo lo que dijo el juego). `registros_juego.sh APPID` junta además el registro de nuestro runtime,
los de OpenComposite/SteamVR y los del juego (Unity `Player.log`, Unreal `Saved/Logs`) en una carpeta.

## Probar un juego nuevo

1. Instalarlo en el Steam de VRmin. Aparece en la app de la Mac con lo detectado.
2. Abrirlo en **Automático**. Si no anda, menú del juego → probar otro runtime (OpenXR ↔ OpenVR).
3. Si sigue sin andar: **Cómo abrirlo → Directo** + **Diagnóstico** y abrir de nuevo; después **Juntar registros**.
4. Marcar el **Estado** (funciona / con problemas / no funciona). Lo que se aprenda se pasa a `perfiles.json`
   (runtime, argumentos, ejecutable) para que ande solo para todos.

## Lo que falta para cada camino

- **SteamVR real**: un driver OpenVR (`driver_vrmin/bin/win64/driver_vrmin.dll` + `driver.vrdrivermanifest`) que entregue
  las imágenes al mismo puente que usa nuestro runtime OpenXR, y las poses/controles del Quest. `runtime.sh` ya apunta
  `openvrpaths.vrpath` a SteamVR con ese driver externo, y OpenXR a `steamxr_win64.json`.
- **Oculus**: incluir ReVive (`Revive.dll` + inyector) y abrir el juego a través del inyector en modo directo.
- **DirectX 12 / Vulkan en OpenXR**: agregar `XR_KHR_D3D12_enable` / `XR_KHR_vulkan_enable2` al runtime
  (hoy solo `XR_KHR_D3D11_enable`). Mientras tanto: OpenVR (OpenComposite) o forzar DX11 (`-force-d3d11`, `-dx11`).
