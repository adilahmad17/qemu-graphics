#!/usr/bin/env bash
# Build qemu-system-x86_64 with the Tirith graphics listener into ./build (no install, no sudo).
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
mkdir -p build
cd build
[ -f build.ninja ] || ../configure --target-list=x86_64-softmmu --disable-werror --disable-docs
make -j"$(nproc)" qemu-system-x86_64
echo "built: $PWD/qemu-system-x86_64"
