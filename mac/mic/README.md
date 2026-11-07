# VRmin Mic

A virtual microphone for macOS, so games can use the Quest's microphone. VRmin Host sends the audio it receives from
the headset to a hidden output, and it comes out of the **VRmin Mic** input, which Wine uses as the games' microphone.

`BlackHole.c` is [BlackHole](https://github.com/ExistentialAudio/BlackHole) (commit 574029f, GPL-3.0, see
`LICENSE-BlackHole.txt`), unmodified. `crear_app.sh` compiles it with these settings: one channel, a visible input-only
device ("VRmin Mic") and a hidden output-only mirror device (UID `VRminMic_2_UID`).
