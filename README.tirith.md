# qemu-tirith

QEMU with the Tirith graphics host listener, for use with `gramine-vm` from
[gramine-multiarch](https://github.com/adilahmad17/gramine-multiarch). x86_64 hosts only.

The Tirith changes are by the ASTERISC Research Lab.

Based on upstream QEMU commit `94474a7733` (2025-10-09, version 10.1.50). The Tirith changes are:

| File | Change |
|---|---|
| `util/mmap-alloc.c` | places the three guest RAM backends at host virtual addresses equal to their guest addresses, and starts the listener thread once the third is mapped |
| `util/sg-listener.c`, `include/qemu/sg.h` | the listener: serves guest requests (DRM ioctls, buffer mmaps, X11 presentation) arriving through the shared comm page |
| `util/sg-xcb.c`, `util/input_ring.h` | host-side X11 presentation and input forwarding; copies of `xcb.c` and `input_ring.h` from the Gramine tree's preload library (`libos/src/egl-backend`, Tirith's `egl-redirect`) |
| `util/meson.build` | builds the above and links the xcb libraries |

The comm-page layout, request ids and arena addresses in `include/qemu/sg.h` must match
`libos/include/libos_graphics.h` and `libos/src/egl-backend/common.h` in the Gramine tree.

## Build

```sh
./tirith-build.sh          # configures build/ for x86_64-softmmu and builds qemu-system-x86_64
```

No install step: point `gramine-vm` at the binary with
`QEMU_PATH=<this repo>/build/qemu-system-x86_64`.

The listener expects the memory layout that `gramine-vm` sets up (three `memory-backend-memfd`
objects: 2G, 2G, the rest) and a guest of at least 5G.
