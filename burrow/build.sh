#!/bin/bash
# Builds Burrow (Mac with Apple Silicon, Xcode command line tools) → build/burrow, signed with the hypervisor entitlement
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
clang -O2 -Wall -Wextra -arch arm64 -mmacosx-version-min=13.0 -o build/burrow burrow.c -framework Hypervisor
codesign -f -s "${BURROW_SIGN:--}" --entitlements burrow.entitlements build/burrow
echo build/burrow
