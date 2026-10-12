# qemu-tirith

QEMU with the Tirith graphics host listener, for use with `gramine-vm` from
[gramine-multiarch](https://github.com/adilahmad17/gramine-multiarch). x86_64 hosts only.

The Tirith changes are by the ASTERISC Research Lab and are licensed like other QEMU contributions,
GPL-2.0-or-later (see `LICENSE`).

Based on upstream QEMU commit `94474a7733` (2025-10-09, version 10.1.50). The Tirith changes are:

| File | Change |
|---|---|
| `util/mmap-alloc.c` | places the three guest RAM backends at host virtual addresses equal to their guest addresses, and starts the listener thread once the third is mapped |
| `util/sg-listener.c`, `include/qemu/sg.h` | the listener: serves guest requests (DRM ioctls, buffer mmaps, X11 presentation) arriving through the shared comm page |
| `util/sg-xcb.c`, `util/input_ring.h` | host-side X11 presentation and input forwarding; copies of `xcb.c` and `input_ring.h` from the Gramine tree's preload library (`libos/src/graphics`, built as `libgraphics.so`; Tirith's `egl-redirect`) |
| `util/meson.build` | builds the above and links the xcb libraries |

The comm-page layout and request ids in `include/qemu/sg.h` must match
`libos/include/libos_graphics.h` and `libos/src/graphics/common.h` in the Gramine tree. The arena
geometry (data-arena size, hugepage arena address) travels at runtime: the PAL reserves the ranges
from an fw_cfg file the Gramine launcher injects (`opt/gramine/graphics_data_size`, bytes of
`GRAMINE_DATA_SIZE`), and the guest ships the same values to the listener in the comm-page setup
(`c->p1`..`p3`). The listener only relies on the data arena starting at `DATA_REGION` (4G) and the
hugepage arena starting at its end; both host and guest sides of those invariants come from that
handshake.

## Build

```sh
./tirith-build.sh          # configures build/ for x86_64-softmmu and builds qemu-system-x86_64
```

No install step: point `gramine-vm` at the binary with
`QEMU_PATH=<this repo>/build/qemu-system-x86_64`.

The listener expects the memory layout that `gramine-vm` sets up: the `huge0` backend (2G of memfd
at guest physical 0, where the kernel and the application live; a second, identity mapping covers
guest 2M--2G so the comm page is reachable), and the GEM arenas from guest 4G up (backends' guest
bases arrive in `GRAMINE_RAM_BASES`; a hugepages deployment adds a dedicated 1G hugetlb bucket for
the hugepage GEM arena at 6G). The floor is guest RAM of 4G (5G with hugepages).
