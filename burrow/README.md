# Burrow

Burrow runs Linux ARM64 games on Apple Silicon Macs, without a Linux virtual machine.

It's built on [elfuse](https://github.com/sysprog21/elfuse) (Apache 2.0): the Linux program runs in its own memory
space under macOS's Hypervisor framework, which catches every request it makes to the Linux kernel (a system call),
and elfuse answers each one with macOS. Burrow adds what games need on top: the GPU (Vulkan → Metal), sound,
controllers and VR (VRmin).

It's being built for VRmin, but it doesn't depend on VRmin.

## Status

elfuse as is: Linux ARM64 programs (static or dynamic, with threads, signals and processes). Nothing for games yet.

## Build and run (Mac with Apple Silicon)

```
brew install binutils
./build.sh                                     # → build/elfuse
build/elfuse [--sysroot <root>] <program> [args…]
```

`--sysroot <root>`: a folder with a Linux system (`/lib`, `/usr`…) where dynamic programs find their libraries.

elfuse is downloaded at the commit in `ELFUSE_COMMIT`, and Burrow's changes to it are in `patches/`.
