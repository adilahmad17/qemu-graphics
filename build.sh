#!/usr/bin/env bash
# build.sh -- build the QEMU with the graphics host listener of Saguaro (branch dev-graphics)
#
# The listener (Tirith) serves the GPU requests of a saguaro-vm guest under KVM. The graphics ABI (the
# comm page, the request ids, the input ring) is defined once in the Saguaro tree and resolved from there:
# this repository sits at vmm/graphics in Saguaro, so the Saguaro root defaults to ../.. (SAGUARO_ROOT
# overrides it). Everything goes to one prefix: bin/qemu-system-x86_64 and share/qemu (the ROMs QEMU
# looks for next to the binary, which is why the binary must stay where it was installed).
#
#   ./build.sh [--prefix DIR] [--jobs N] [--install-deps] [--clean]
#
#   --prefix DIR     install prefix (default: <this directory>/prefix)
#   --jobs N         parallel jobs (default: all CPUs)
#   --install-deps   install the Ubuntu/Debian packages the build needs first (apt-get, sudo unless root)
#   --clean          remove the build directory first
#
#   SAGUARO_ROOT     the Saguaro tree (default: <this directory>/../..); needs
#                    libos/include/graphics_comm_abi.h and libos/src/graphics/{input_ring.h,xcb.c}
#
# Needs: a C toolchain, git (QEMU fetches a small subproject), ninja, python3 with venv, pkg-config,
# flex, bison, the glib/pixman/fdt development packages and the X11, xcb, xshmfence, GL, EGL, gbm and
# libdrm development packages of the listener. When it finishes it checks the result and prints how
# to use it with Saguaro:
#   export QEMU_PATH=<prefix>/bin/qemu-system-x86_64
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
PREFIX=$HERE/prefix
JOBS=$(nproc)
DEPS=0
CLEAN=0
APT_PACKAGES="build-essential ninja-build python3 python3-venv pkg-config flex bison git ca-certificates
              libglib2.0-dev libpixman-1-dev libfdt-dev libslirp-dev libcap-ng-dev libseccomp-dev
              libx11-dev libx11-xcb-dev libxshmfence-dev libxcb1-dev libxcb-dri3-dev libxcb-present-dev
              libxcb-sync-dev libxcb-xfixes0-dev libxcb-shape0-dev libxcb-randr0-dev libxcb-shm0-dev
              libxcb-render0-dev libgl-dev libegl-dev libgbm-dev libdrm-dev"

while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$(readlink -m "${2:?--prefix needs a directory}"); shift ;;
        --jobs) JOBS=${2:?--jobs needs a number}; shift ;;
        --install-deps) DEPS=1 ;;
        --clean) CLEAN=1 ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "build.sh: unknown argument: $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done

BUILD=$HERE/build
log() { echo ">>> $*"; }
die() { echo "build.sh: $*" >&2; exit 1; }

# the Saguaro tree that defines the graphics ABI
SAGUARO_ROOT=$(readlink -m "${SAGUARO_ROOT:-$HERE/../..}")
SAGUARO_ABI_DIR=$SAGUARO_ROOT/libos/include
SAGUARO_GRAPHICS_DIR=$SAGUARO_ROOT/libos/src/graphics
[ -f "$SAGUARO_ABI_DIR/graphics_comm_abi.h" ] || die "no libos/include/graphics_comm_abi.h under SAGUARO_ROOT=$SAGUARO_ROOT:" \
    "build this repository from vmm/graphics of a Saguaro checkout, or set SAGUARO_ROOT to the Saguaro tree"
[ -f "$SAGUARO_GRAPHICS_DIR/input_ring.h" ] || die "no libos/src/graphics/input_ring.h under SAGUARO_ROOT=$SAGUARO_ROOT"
# util/sg-xcb.c is the one copy of Saguaro's libos/src/graphics/xcb.c; only the SPDX line may differ
cmp -s <(tail -n +2 "$HERE/util/sg-xcb.c") <(tail -n +2 "$SAGUARO_GRAPHICS_DIR/xcb.c") ||
    die "util/sg-xcb.c differs from $SAGUARO_GRAPHICS_DIR/xcb.c (only the SPDX license line may differ):" \
        "copy the file from that tree, or use the Saguaro revision this one matches"

if [ $DEPS = 1 ]; then
    command -v apt-get >/dev/null || die "--install-deps needs apt-get (Ubuntu or Debian)"
    sudo=; [ "$(id -u)" = 0 ] || sudo=sudo
    log "installing packages"
    $sudo apt-get update -qq
    # shellcheck disable=SC2086
    $sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $APT_PACKAGES
fi

missing=
for c in gcc make ninja pkg-config python3 flex bison git; do
    command -v "$c" >/dev/null || missing="$missing $c"
done
for p in glib-2.0 pixman-1 x11 xcb xcb-dri3 xcb-present xcb-render xcb-sync xcb-xfixes xcb-shape xcb-randr \
         xcb-shm xshmfence gl egl gbm libdrm; do
    pkg-config --exists "$p" 2>/dev/null || missing="$missing lib:$p"
done
python3 -c 'import venv, ensurepip' 2>/dev/null || missing="$missing python3-venv"
[ -z "$missing" ] || die "missing:$missing (on Ubuntu or Debian: ./build.sh --install-deps)"

[ $CLEAN = 0 ] || { log "cleaning"; rm -rf "$BUILD"; }
mkdir -p "$PREFIX"

# QEMU: x86_64 system emulation with KVM (the listener serves a KVM guest). Reconfigured on every run:
# cheap when nothing changed, and it picks up the include paths of the Saguaro tree.
log "configuring QEMU (graphics ABI from $SAGUARO_ROOT)"
mkdir -p "$BUILD"
cd "$BUILD"
../configure --prefix="$PREFIX" --target-list=x86_64-softmmu --enable-kvm \
    --extra-cflags="-I$SAGUARO_ABI_DIR -I$SAGUARO_GRAPHICS_DIR" \
    --disable-docs --disable-werror
log "building QEMU ($JOBS jobs)"
make -j"$JOBS"
log "installing into $PREFIX"
make install

# check the result: the binary, KVM, the graphics listener and the ROMs the guest needs
Q=$PREFIX/bin/qemu-system-x86_64
[ -x "$Q" ] || die "$Q was not built"
for rom in bios-256k.bin kvmvapic.bin linuxboot_dma.bin; do
    [ -f "$PREFIX/share/qemu/$rom" ] || die "$PREFIX/share/qemu/$rom is missing: the guest could not start"
done
"$Q" -accel help 2>/dev/null | grep -qx kvm || die "$Q has no KVM accelerator"
# the listener adds no QEMU option or object; its run-time settings are environment variables
for marker in TIRITH_HONOR_GUEST_PAT TIRITH_SLOT_IDLE_US GRAMINE_RAM_BASES; do
    grep -qaF "$marker" "$Q" || die "$Q does not contain the graphics listener ($marker): is this the dev-graphics branch?"
done

version=$("$Q" --version | sed -n '1s/^QEMU emulator version //p')
echo
log "done: QEMU $version with the graphics listener"
echo "  $Q"
echo
echo "Use it with Saguaro's saguaro-vm (keep the binary where it is: it finds its ROMs in ../share/qemu):"
echo "  export QEMU_PATH=$Q"
