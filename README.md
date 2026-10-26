# qemu-graphics

A fork of QEMU 10.1.50 (a development snapshot, upstream commit `94474a7733`) that contains the host side of the graphics redirection of the [Saguaro](https://github.com/ASTERISC-Release/saguaro) library OS. An OpenGL or Vulkan application inside a `saguaro-vm` guest draws on the host's GPU: a preload library in the guest (`libgraphics.so`) forwards the GPU work through shared memory to a listener in this QEMU, which executes it on the host's render node and presents the result on the host's X server. The technique is Tirith ([arXiv:2609.17525](https://arxiv.org/abs/2609.17525)).

The branch `dev-graphics` (default) contains the changes; `main` is the upstream base commit unmodified. In Saguaro the repository is the submodule `vmm/graphics`.

## Features

- The listener (`util/sg-listener.c`, `include/qemu/sg.h`): a host thread that serves the requests the guest places in the shared comm page (DRM ioctls, buffer allocation and mapping in the GEM arenas, forwarded file-descriptor calls, X11 presentation), with several request slots and a comm ABI version check at the handshake.
- Host-side X11 presentation and input forwarding (`util/sg-xcb.c`) over xcb, DRI3 and Present, with the input ring shared with the guest.
- Identity-mapped guest RAM (`util/mmap-alloc.c`): the memfd memory backends are placed at host virtual addresses equal to their guest-physical addresses, following the backend map that `saguaro-vm` exports in `GRAMINE_RAM_BASES`; the listener starts once the last backend is mapped. The data arena address is read from `GRAMINE_DATA_ADDR`.
- Guest page attributes under KVM (`target/i386/kvm/kvm.c`): KVM honors the guest PAT (`KVM_X86_QUIRK_IGNORE_GUEST_PAT` disabled), so write-combined GPU buffers stay write-combined in the guest; `TIRITH_HONOR_GUEST_PAT=0` keeps KVM's default.
- Polling of the request slots: `TIRITH_SLOT_IDLE_US` sets the spin time of the additional slots before they back off (default 2000), `TIRITH_SLOT0_IDLE_US` enables the same backoff for slot 0.

The graphics ABI (the comm page layout, the request ids, the input ring) is defined once in the Saguaro tree (`libos/include/graphics_comm_abi.h`, `libos/src/graphics/input_ring.h`) and is taken from there at build time. `util/sg-xcb.c` is a copy of Saguaro's `libos/src/graphics/xcb.c` that differs only in its license line; the build fails when the two differ.

## Build

The build requires a C toolchain, git, ninja, Python 3 with `venv`, flex, bison, and the development packages of glib, pixman, fdt, X11, xcb, xshmfence, GL, EGL, gbm and libdrm. It is run from the Saguaro tree, where this repository is `vmm/graphics`; Saguaro's `scripts/` call it. On Ubuntu or Debian:

    cd vmm/graphics
    ./build.sh --install-deps    # installs the packages with apt-get, then builds
    ./build.sh                   # builds when the packages are installed

`build.sh` builds QEMU for `x86_64-softmmu` with KVM and installs it into `./prefix` (`--prefix DIR`, `--jobs N`, `--clean`). The installed `qemu-system-x86_64` locates its ROMs in `../share/qemu` and is used from its installation directory. The Saguaro tree is `../..` of this directory; a build outside it sets `SAGUARO_ROOT` to a Saguaro checkout:

    SAGUARO_ROOT=/path/to/saguaro ./build.sh

The script finishes by checking the KVM accelerator, the graphics listener and the ROMs, and prints the `QEMU_PATH` to use.

## Use

`saguaro-vm` runs the QEMU named by `QEMU_PATH`. With Saguaro installed with graphics support (`scripts/install.sh --vm`):

    export QEMU_PATH=<prefix>/bin/qemu-system-x86_64
    cd CI-Examples/graphics/glxgears
    make
    make run                     # saguaro-vm glxgears

The host needs KVM (`/dev/kvm`), `/dev/vhost-vsock`, a GPU render node (`/dev/dri/renderD128`) and an X server (`DISPLAY`). `saguaro-vm` creates the memory backends and passes their layout to this QEMU; Saguaro's `Documentation/quickstart/graphics-vm.md` describes the setup and the graphics examples.

## Credits and license

The graphics redirection is the Tirith technique ([arXiv:2609.17525](https://arxiv.org/abs/2609.17525)). The changes to QEMU are by the ASTERISC Research Lab and are licensed under GPL-2.0-or-later, the license of new QEMU source files. QEMU as a whole is licensed under GPL-2.0 (`COPYING`, `LICENSE`).
