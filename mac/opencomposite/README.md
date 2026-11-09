# OpenComposite for VRmin

OpenComposite lets SteamVR (OpenVR) games run on an OpenXR runtime. VRmin builds it from source instead of
downloading a prebuilt copy, so it can be fixed and extended for VRmin.

- Source: [GameNative/opencomposite](https://github.com/GameNative/opencomposite) (a fork of
  [OpenComposite](https://gitlab.com/znixian/OpenOVR)), at the commit in `COMMIT`.
- VRmin's changes: the `*.patch` files in this folder, applied in name order.
- Build: `.github/workflows/opencomposite.yml` (Windows, MSVC). It saves the result here as `vrclient_x64.dll`
  (64-bit games) and `vrclient.dll` (32-bit games); `crear_app.sh` puts them in VRmin Host.
- License: GPL 3 (see `LICENSE-OpenComposite.txt`, saved by the build next to the DLLs).
