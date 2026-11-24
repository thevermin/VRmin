# KosmicKrisp patches from steamac

These patches come unchanged from [fxgl/steamac](https://github.com/fxgl/steamac) (`host/kosmickrisp/patches`,
commit 79aeadb, Apache-2.0): upstream Mesa merge requests not merged yet (geometry shaders !44786, transform feedback
!44928, …) and steamac's own fixes for DXVK and vkd3d-proton games. They apply in order on Mesa ce576c29 (MIT).
KosmicKrisp needs macOS 26+ on Apple silicon (Metal 4). See `.github/workflows/kosmickrisp.yml`.
