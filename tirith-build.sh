#!/usr/bin/env bash
# Build qemu-system-x86_64 with the Tirith graphics listener into ./build (no install, no sudo).
#
# The graphics ABI (the comm page, request ids, input ring) is defined once in the sibling
# gramine-multiarch checkout; this build resolves it from there:
#   - GRAMINE_ABI_DIR (the gramine-multiarch libos/include) -- the comm-page ABI header
#   - GRAMINE_GRAPHICS_DIR (its libos/src/graphics) -- input_ring.h
# Both default to ../gramine-multiarch and can be overridden (GRAMINE_ABI_ROOT sets the root).
# Only util/sg-xcb.c remains a copy here (identical to that tree's xcb.c apart from the GPL
# license header) -- the sync check below fails the build if the two ever drift.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

GRAMINE_ROOT=${GRAMINE_ABI_ROOT:-$(readlink -f ../gramine-multiarch)}
GRAMINE_ABI_DIR=$GRAMINE_ROOT/libos/include
GRAMINE_GRAPHICS_DIR=$GRAMINE_ROOT/libos/src/graphics

[ -e "$GRAMINE_ABI_DIR/graphics_comm_abi.h" ] || {
    echo "error: no graphics_comm_abi.h under $GRAMINE_ABI_DIR; check out gramine-multiarch next" \
         "to this repository (or point GRAMINE_ABI_ROOT at it)" >&2
    exit 1
}

cmp -s <(tail -n +2 util/sg-xcb.c) <(tail -n +2 "$GRAMINE_GRAPHICS_DIR/xcb.c") || {
    echo "error: util/sg-xcb.c has drifted from the gramine-multiarch libos/src/graphics/xcb.c" \
         "(only the SPDX license line may differ); re-copy the file from that tree" >&2
    exit 1
}

export GRAMINE_ABI_DIR GRAMINE_GRAPHICS_DIR
mkdir -p build
cd build
# Reconfigure on every run: cheap when nothing changed, and it picks up the ABI include path.
CFLAGS="${CFLAGS:-} -I$GRAMINE_ABI_DIR -I$GRAMINE_GRAPHICS_DIR" \
    ../configure --target-list=x86_64-softmmu --disable-werror --disable-docs >/dev/null
make -j"$(nproc)" qemu-system-x86_64
echo "built: $PWD/qemu-system-x86_64"