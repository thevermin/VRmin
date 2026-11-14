# VRmin driver for SteamVR

`driver_vrmin.cpp` is a SteamVR driver (OpenVR driver API): SteamVR sees a headset and two Touch controllers, and
VRmin streams what SteamVR's compositor draws. It's the "SteamVR" choice for a game in VRmin Host: the game runs with
Valve's real SteamVR (its compositor, its per-game controller bindings) instead of OpenComposite.

On the Mac it runs inside VRmin's Windows, with SteamVR for Windows installed in VRmin's Steam.

- `driver_vrmin/`: the driver folder SteamVR loads (`driver.vrdrivermanifest`, `bin/win64/driver_vrmin.dll`, built by
  the «Driver de SteamVR» workflow and kept in the repo like the OpenXR runtime). VRmin installs it in
  `C:\questvrbridge\driver_vrmin` and registers it in `openvrpaths.vrpath` (`runtime.sh`, mode `steamvr`).
- Log: `C:\questvrbridge\driver_vrmin.log`.

Status: step 1 (the devices, a fixed pose, the play area; frames are only counted).
