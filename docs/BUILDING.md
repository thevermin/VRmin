# Building VRmin from source

GitHub Actions builds everything on every push (see `.github/workflows`); download the results from the run's
**Artifacts**. To build yourself:

## VRmin Host (Mac)

Needs an Apple Silicon Mac with the Xcode Command Line Tools (`xcode-select --install`).

```bash
bash mac/crear_app.sh            # builds "VRmin Host.app", signs it locally and opens it
bash mac/crear_app.sh --paquete  # ad-hoc signed, packaged as "VRmin Host <version>.zip" for distribution
```

## VRmin (Quest app)

Needs JDK 17, Gradle 8.9 and the Android SDK (compileSdk 34).

```bash
cd quest-app
gradle assembleRelease
```

Without a store key the APK is signed with your local Android debug key. Install it with
`adb install -r app/build/outputs/apk/release/*.apk`.

## OpenXR runtime (Windows DLL)

`mac/openxr/questvrbridge_openxr.dll` is committed, and rebuilt by the `runtime-openxr` workflow when the source
changes. To build it by hand you need mingw-w64 and the headers from
[OpenXR-SDK release-1.1.43](https://github.com/KhronosGroup/OpenXR-SDK/releases/tag/release-1.1.43) and
[Vulkan-Headers v1.3.296](https://github.com/KhronosGroup/Vulkan-Headers/releases/tag/v1.3.296):

```bash
x86_64-w64-mingw32-gcc -O2 -shared -o mac/openxr/questvrbridge_openxr.dll \
  mac/openxr/questvrbridge_openxr.c mac/openxr/questvrbridge_openxr.def -I<OpenXR-SDK>/include -I<Vulkan-Headers>/include \
  -ld3d11 -ldxgi -ldxguid -luuid -lws2_32 -lwinmm -static-libgcc -Wl,--kill-at
```

## Project layout

| Folder | What it is |
|---|---|
| `mac/app` | VRmin Host (SwiftUI) and the scripts it runs (`recursos/`: engine install, Steam, games) |
| `mac/renderer` | Renderer and video encoder (Swift + Metal) |
| `mac/openxr` | The Windows OpenXR runtime that games load inside Wine |
| `mac/server.py`, `mac/web` | Stream server and its web panel |
| `quest-app` | The Quest app (Java + OpenXR) |

Code comments and script messages are mostly in Spanish; the apps themselves are in English and Spanish.

## Contributing

Issues and pull requests are welcome. By contributing you agree that your contribution is licensed under the
GPL-3.0 with the additional terms in [NOTICE.md](../NOTICE.md).
