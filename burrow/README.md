# Burrow

Burrow runs Linux ARM64 programs on Apple Silicon Macs, without a Linux virtual machine.

The Linux program runs in its own memory space under macOS's Hypervisor framework, which catches every request it
makes to the Linux kernel (a system call). Burrow answers each one with macOS. There's no Linux kernel and no disk
image: the program's files are normal files on the Mac.

It's being built for VRmin (to run the Linux ARM64 versions of SteamVR games), but it doesn't depend on VRmin.

## Status

Step 1: static and simple dynamic programs (memory, files, time, the program's own startup). No threads, signals,
GPU, sound or network yet.

## Build and run (Mac with Apple Silicon)

```
./build.sh                          # → build/burrow (signed with the hypervisor entitlement)
build/burrow [-r <root>] <program> [args…]
```

`-r <root>`: a folder with a Linux system (`/lib`, `/usr`…) that the program sees as `/`. Without it, the program
sees the Mac's files.

`BURROW_LOG=1` prints every system call.
