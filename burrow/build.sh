#!/bin/bash
# Builds Burrow (Mac with Apple Silicon): elfuse (github.com/sysprog21/elfuse) at the commit in ELFUSE_COMMIT, with
# patches/*.patch applied → build/elfuse, signed with the hypervisor entitlement.
# Needs the Xcode command line tools and GNU objcopy («brew install binutils»).
set -euo pipefail
cd "$(dirname "$0")"
commit=$(cat ELFUSE_COMMIT)
src=.deps/elfuse
if [ "$(git -C "$src" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
  rm -rf "$src"; mkdir -p .deps
  git clone -q https://github.com/sysprog21/elfuse.git "$src"
  git -C "$src" checkout -q "$commit"
  for p in patches/*.patch; do [ -f "$p" ] && git -C "$src" apply "$PWD/$p"; done
fi
# (GNU objcopy from Homebrew, if it isn't in PATH)
for d in /opt/homebrew/opt/binutils/bin /usr/local/opt/binutils/bin; do [ -d "$d" ] && PATH="$d:$PATH"; done
make -C "$src" -j"$(sysctl -n hw.ncpu)" elfuse ${BURROW_SIGN:+SIGN_IDENTITY="$BURROW_SIGN"} >/dev/null
mkdir -p build
cp "$src/build/elfuse" build/elfuse.tmp && mv build/elfuse.tmp build/elfuse
echo build/elfuse
