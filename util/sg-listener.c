/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#include "qemu/osdep.h"
#include "include/qemu/sg.h"
#include "hw/core/cpu.h"
#include <linux/kvm.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/xshmfence.h>
#include <assert.h>
#include <drm/drm.h>
#include <drm/i915_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

/* SYSCALL logging */
int syscall_logging_enabled = 0;
static uint64_t fs_offset = GRAPHICS_FS_OFFSET; /* the shared ABI header (graphics_comm_abi.h) */

/* Idle sleep configuration: sleep instead of spinning when no requests arrive
 */
#define ENABLE_IDLE_SLEEP 0
#define IDLE_TIMEOUT_MS 1000 /* ms before thread sleeps */
#define IDLE_SLEEP_US 1000  /* us to sleep when idle */

/* IOCTL logging */
static uint64_t ioctl_count = 0;
static uint64_t ioctl_total_ns = 0;
static uint64_t ioctl_max_ns = 0;
static uint64_t frame_count = 0;
static bool ioctl_logging_enabled = false;
#define IOCTL_STATS_MAX 64
typedef struct ioctl_stat {
  unsigned long request;
  uint64_t count;
  uint64_t total_ns;
  uint64_t max_ns;
  uint64_t last_fd;
} ioctl_stat;
static ioctl_stat ioctl_stats[IOCTL_STATS_MAX];
static size_t ioctl_stats_used = 0;
static uint64_t execbuffer2_last_flags = 0;
static uint32_t syncobj_wait_last_flags = 0;

#ifdef STAT_DEBUG
static void print_syncobj_wait_flags(FILE *out, uint32_t flags) {
  if (flags == 0) {
    fprintf(out, "NONE\n");
    return;
  }

  bool first = true;
#ifdef STAT_DEBUG
#define PRINT_SYNCOBJ_FLAG(flag)                                               \
  do {                                                                         \
    if (flags & (flag)) {                                                      \
      fprintf(out, "%s%s", first ? "" : "|", #flag);                           \
      first = false;                                                           \
    }                                                                          \
  } while (0)
#else
#define PRINT_SYNCOBJ_FLAG(flag)                                               \
  do {                                                                         \
  } while (0)
#endif

  PRINT_SYNCOBJ_FLAG(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL);
  PRINT_SYNCOBJ_FLAG(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);

  if (first)
    fprintf(out, "0x%x", flags);

  fprintf(out, "\n");

#undef PRINT_SYNCOBJ_FLAG
}
#else
static void print_syncobj_wait_flags(FILE *out, uint32_t flags) {}
#endif

#ifdef STAT_DEBUG
static void print_execbuffer2_flags(uint64_t flags) {
  if (flags == 0) {
    fprintf(stderr, "NONE");
    return;
  }

  bool first = true;

#ifdef STAT_DEBUG
#define PRINT_FLAG(flag)                                                       \
  do {                                                                         \
    if (flags & (flag)) {                                                      \
      fprintf(stderr, "%s%s", first ? "" : "|", #flag);                        \
      first = false;                                                           \
    }                                                                          \
  } while (0)
#else
#define PRINT_FLAG(flag)                                                       \
  do {                                                                         \
  } while (0)
#endif

  PRINT_FLAG(I915_EXEC_RING_MASK);
  PRINT_FLAG(I915_EXEC_DEFAULT);
  PRINT_FLAG(I915_EXEC_RENDER);
  PRINT_FLAG(I915_EXEC_BSD);
  PRINT_FLAG(I915_EXEC_BLT);
  PRINT_FLAG(I915_EXEC_VEBOX);
  PRINT_FLAG(I915_EXEC_SECURE);
  PRINT_FLAG(I915_EXEC_NO_RELOC);
  PRINT_FLAG(I915_EXEC_HANDLE_LUT);
  PRINT_FLAG(I915_EXEC_BSD_MASK);
  PRINT_FLAG(I915_EXEC_RESOURCE_STREAMER);
  PRINT_FLAG(I915_EXEC_FENCE_ARRAY);
  PRINT_FLAG(I915_EXEC_FENCE_OUT);
  PRINT_FLAG(I915_EXEC_USE_EXTENSIONS);
#ifdef I915_EXEC_NO_FENCE
  PRINT_FLAG(I915_EXEC_NO_FENCE);
#endif
  PRINT_FLAG(I915_EXEC_BATCH_FIRST);
  PRINT_FLAG(I915_EXEC_FENCE_SUBMIT);
#ifdef I915_EXEC_CAPTURE
  PRINT_FLAG(I915_EXEC_CAPTURE);
#endif
#ifdef I915_EXEC_DEBUG
  PRINT_FLAG(I915_EXEC_DEBUG);
#endif

  if (first)
    fprintf(stderr, "0x%llx", (unsigned long long)flags);

  fprintf(stderr, "\n");

#undef PRINT_FLAG
}
#else
static void print_execbuffer2_flags(uint64_t flags) {}
#endif

static const char *i915_ioctl_name(unsigned long request) {
  switch (request) {
  case DRM_IOCTL_I915_GEM_EXECBUFFER:
    return "GEM_EXECBUFFER";
  case DRM_IOCTL_I915_GEM_EXECBUFFER2:
    return "GEM_EXECBUFFER2";
  case DRM_IOCTL_I915_GEM_EXECBUFFER2_WR:
    return "GEM_EXECBUFFER2_WR";
  case DRM_IOCTL_I915_GEM_CREATE:
    return "GEM_CREATE";
  case DRM_IOCTL_I915_GEM_CREATE_EXT:
    return "GEM_CREATE_EXT";
  case DRM_IOCTL_I915_GEM_SET_DOMAIN:
    return "GEM_SET_DOMAIN";
  case DRM_IOCTL_I915_GEM_GET_TILING:
    return "GEM_GET_TILING";
  case DRM_IOCTL_I915_GEM_SET_TILING:
    return "GEM_SET_TILING";
  case DRM_IOCTL_I915_GEM_BUSY:
    return "GEM_BUSY";
  case DRM_IOCTL_I915_GEM_MMAP:
    return "GEM_MMAP";
  case DRM_IOCTL_I915_GEM_MMAP_GTT:
    return "GEM_MMAP_GTT";
  case DRM_IOCTL_I915_GEM_MMAP_OFFSET:
    return "GEM_MMAP_OFFSET";
  case DRM_IOCTL_I915_GEM_PREAD:
    return "GEM_PREAD";
  case DRM_IOCTL_I915_GEM_PWRITE:
    return "GEM_PWRITE";
  case DRM_IOCTL_I915_GEM_THROTTLE:
    return "GEM_THROTTLE";
  case DRM_IOCTL_I915_GEM_CONTEXT_CREATE:
    return "GEM_CONTEXT_CREATE";
  case DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT:
    return "GEM_CONTEXT_CREATE_EXT";
  case DRM_IOCTL_I915_GEM_CONTEXT_DESTROY:
    return "GEM_CONTEXT_DESTROY";
  case DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM:
    return "GEM_CONTEXT_SETPARAM";
  case DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM:
    return "GEM_CONTEXT_GETPARAM";
  case DRM_IOCTL_I915_GEM_USERPTR:
    return "GEM_USERPTR";
  case DRM_IOCTL_I915_GEM_WAIT:
    return "GEM_WAIT";
  case DRM_IOCTL_I915_GEM_SW_FINISH:
    return "GEM_SW_FINISH";
  case DRM_IOCTL_I915_GEM_GET_APERTURE:
    return "GEM_GET_APERTURE";
  case DRM_IOCTL_I915_GEM_SET_CACHING:
    return "GEM_SET_CACHING";
  case DRM_IOCTL_I915_GEM_GET_CACHING:
    return "GEM_GET_CACHING";
  case DRM_IOCTL_I915_REG_READ:
    return "REG_READ";
  case DRM_IOCTL_I915_GETPARAM:
    return "GETPARAM";
  case DRM_IOCTL_I915_SETPARAM:
    return "SETPARAM";
  case DRM_IOCTL_I915_GEM_MADVISE:
    return "GEM_MADVISE";
#ifdef DRM_IOCTL_SYNCOBJ_CREATE
  case DRM_IOCTL_SYNCOBJ_CREATE:
    return "SYNCOBJ_CREATE";
#endif
#ifdef DRM_IOCTL_SYNCOBJ_DESTROY
  case DRM_IOCTL_SYNCOBJ_DESTROY:
    return "SYNCOBJ_DESTROY";
#endif
#ifdef DRM_IOCTL_SYNCOBJ_WAIT
  case DRM_IOCTL_SYNCOBJ_WAIT:
    return "SYNCOBJ_WAIT";
#endif
#ifdef DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD
  case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
    return "SYNCOBJ_HANDLE_TO_FD";
#endif
#ifdef DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE
  case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
    return "SYNCOBJ_FD_TO_HANDLE";
#endif
#ifdef DRM_IOCTL_SYNCOBJ_RESET
  case DRM_IOCTL_SYNCOBJ_RESET:
    return "SYNCOBJ_RESET";
#endif
#ifdef DRM_IOCTL_SYNCOBJ_SIGNAL
  case DRM_IOCTL_SYNCOBJ_SIGNAL:
    return "SYNCOBJ_SIGNAL";
#endif
#ifdef DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT
  case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
    return "SYNCOBJ_TIMELINE_WAIT";
#endif
#ifdef DRM_IOCTL_SYNCOBJ_QUERY
  case DRM_IOCTL_SYNCOBJ_QUERY:
    return "SYNCOBJ_QUERY";
#endif
#ifdef DRM_IOCTL_SYNCOBJ_EVENTFD
  case DRM_IOCTL_SYNCOBJ_EVENTFD:
    return "SYNCOBJ_EVENTFD";
#endif
#ifdef DRM_IOCTL_I915_GEM_PIN
  case DRM_IOCTL_I915_GEM_PIN:
    return "GEM_PIN";
#endif
#ifdef DRM_IOCTL_I915_GEM_UNPIN
  case DRM_IOCTL_I915_GEM_UNPIN:
    return "GEM_UNPIN";
#endif
#ifdef DRM_IOCTL_I915_GEM_ENTERVT
  case DRM_IOCTL_I915_GEM_ENTERVT:
    return "GEM_ENTERVT";
#endif
#ifdef DRM_IOCTL_I915_GEM_LEAVEVT
  case DRM_IOCTL_I915_GEM_LEAVEVT:
    return "GEM_LEAVEVT";
#endif
#ifdef DRM_IOCTL_I915_GEM_SET_EXEC_TIMEOUT
  case DRM_IOCTL_I915_GEM_SET_EXEC_TIMEOUT:
    return "GEM_SET_EXEC_TIMEOUT";
#endif
#ifdef DRM_IOCTL_I915_GEM_GET_EXEC_TIMEOUT
  case DRM_IOCTL_I915_GEM_GET_EXEC_TIMEOUT:
    return "GEM_GET_EXEC_TIMEOUT";
#endif
#ifdef DRM_IOCTL_I915_GEM_GETPARAM
  case DRM_IOCTL_I915_GEM_GETPARAM:
    return "GEM_GETPARAM";
#endif
#ifdef DRM_IOCTL_I915_GEM_SHMEM_CREATE
  case DRM_IOCTL_I915_GEM_SHMEM_CREATE:
    return "GEM_SHMEM_CREATE";
#endif
#ifdef DRM_IOCTL_I915_GEM_CONTEXT_RESET_STATS
  case DRM_IOCTL_I915_GEM_CONTEXT_RESET_STATS:
    return "GEM_CONTEXT_RESET_STATS";
#endif
#ifdef DRM_IOCTL_I915_ALLOC
  case DRM_IOCTL_I915_ALLOC:
    return "ALLOC";
#endif
#ifdef DRM_IOCTL_I915_FREE
  case DRM_IOCTL_I915_FREE:
    return "FREE";
#endif
#ifdef DRM_IOCTL_I915_INIT
  case DRM_IOCTL_I915_INIT:
    return "INIT";
#endif
#ifdef DRM_IOCTL_I915_FLUSH
  case DRM_IOCTL_I915_FLUSH:
    return "FLUSH";
#endif
#ifdef DRM_IOCTL_I915_BATCHBUFFER
  case DRM_IOCTL_I915_BATCHBUFFER:
    return "BATCHBUFFER";
#endif
#ifdef DRM_IOCTL_I915_IRQ_EMIT
  case DRM_IOCTL_I915_IRQ_EMIT:
    return "IRQ_EMIT";
#endif
#ifdef DRM_IOCTL_I915_IRQ_WAIT
  case DRM_IOCTL_I915_IRQ_WAIT:
    return "IRQ_WAIT";
#endif
#ifdef DRM_IOCTL_I915_SWAP
  case DRM_IOCTL_I915_SWAP:
    return "SWAP";
#endif
#ifdef DRM_IOCTL_I915_CLIP
  case DRM_IOCTL_I915_CLIP:
    return "CLIP";
#endif
#ifdef DRM_IOCTL_I915_GEM_CONTEXT_RESET_STATS
  case DRM_IOCTL_I915_GEM_CONTEXT_RESET_STATS:
    return "GEM_CONTEXT_RESET_STATS";
#endif
#ifdef DRM_IOCTL_I915_GEM_VM_CREATE
  case DRM_IOCTL_I915_GEM_VM_CREATE:
    return "GEM_VM_CREATE";
#endif
#ifdef DRM_IOCTL_I915_GEM_VM_DESTROY
  case DRM_IOCTL_I915_GEM_VM_DESTROY:
    return "GEM_VM_DESTROY";
#endif
#ifdef DRM_IOCTL_I915_GEM_VM_BIND
  case DRM_IOCTL_I915_GEM_VM_BIND:
    return "GEM_VM_BIND";
#endif
#ifdef DRM_IOCTL_I915_GEM_VM_UNBIND
  case DRM_IOCTL_I915_GEM_VM_UNBIND:
    return "GEM_VM_UNBIND";
#endif
  default:
    return NULL;
  }
}

static void print_ioctl_stats(void) {
  if (ioctl_count == 0) {
    log_stat("IOCTL stats: no ioctls recorded in this interval\n");
    return;
  }

  /* Convert totals to microseconds for reporting */
  double total_us = (double)ioctl_total_ns / 1000.0;
  double avg_us = total_us / (double)ioctl_count;
  double max_us = (double)ioctl_max_ns / 1000.0;

  log_stat("==== IOCTL STATS (last %lu frames) ===\n", (unsigned long)5000);
  log_stat("Total IOCTLs: %lu\n", (unsigned long)ioctl_count);
  log_stat("Total time: %.2f us\n", total_us);
  log_stat("Average time: %.2f us\n", avg_us);
  log_stat("Max time: %.2f us\n", max_us);
  log_stat("Per-request breakdown:\n");

  log_stat("last EXECBUFFER2 flags: 0x%llx => ",
           (unsigned long long)execbuffer2_last_flags);
  print_execbuffer2_flags(execbuffer2_last_flags);

  log_stat("last SYNCOBJ_WAIT flags: 0x%x => ", syncobj_wait_last_flags);
  print_syncobj_wait_flags(stderr, syncobj_wait_last_flags);

  for (size_t i = 0; i < ioctl_stats_used; ++i) {
    ioctl_stat *s = &ioctl_stats[i];
    const char *name = i915_ioctl_name(s->request);
    double total_req_us = (double)s->total_ns / 1000.0;
    double avg_req_us =
        (s->count ? (double)s->total_ns / (double)s->count / 1000.0 : 0.0);
    double max_req_us = (double)s->max_ns / 1000.0;
    if (name)
      log_stat("  %s: count=%llu, total=%.2f us, avg=%.2f us, max=%.2f us, "
               "last_fd=%llu\n",
               name, (unsigned long long)s->count, total_req_us, avg_req_us,
               max_req_us, (unsigned long long)s->last_fd);
    else
      log_stat("  0x%lx: count=%llu, total=%.2f us, avg=%.2f us, max=%.2f us, "
               "last_fd=%llu\n",
               (unsigned long)s->request, (unsigned long long)s->count,
               total_req_us, avg_req_us, max_req_us,
               (unsigned long long)s->last_fd);
  }

  log_stat("====================================\n");
}

static ioctl_stat *get_ioctl_stat(unsigned long request) {
  for (size_t i = 0; i < ioctl_stats_used; ++i) {
    if (ioctl_stats[i].request == request)
      return &ioctl_stats[i];
  }

  if (ioctl_stats_used >= IOCTL_STATS_MAX)
    return NULL;

  ioctl_stat *slot = &ioctl_stats[ioctl_stats_used++];
  memset(slot, 0, sizeof(*slot));
  slot->request = request;
  return slot;
}

struct timespec ts;
void *data_region_actual_address = NULL;
void *hugepage_host_addr = NULL;
size_t data_region_size = GRAPHICS_GEM_DATA_SIZE;
size_t huge_region_size = GRAPHICS_GEM_HUGEPAGE_SIZE;
typedef struct {
  uint64_t host_address;
  uint64_t guest_address;
} gem_slots_t;
gem_slots_t gem_slots = {0};
gem_slots_t gem_hugepage_slots = {0};

/* [mem] Bucket usage instrumentation: the bases of the bump pointers and the
 * high-water marks seen so far, logged when they grow. Sizing input for the
 * memory-reservation work. */
static uint64_t gem_regular_base = 0;
static uint64_t gem_hugepage_base = 0;
static uint64_t gem_regular_peak = 0;
static uint64_t gem_hugepage_peak = 0;

static void gem_report_peak(const char *name, uint64_t *peak, uint64_t base,
                            uint64_t next, uint64_t region_size) {
  uint64_t used = next > base ? next - base : 0;
  if (used <= *peak) {
    return;
  }
  *peak = used;
  log_always("[mem] Gem %s bucket: %llu / %llu MiB (%.1f%%)\n", name,
             (unsigned long long)(used >> 20),
             (unsigned long long)(region_size >> 20),
             region_size ? (100.0 * used) / region_size : 0.0);
}

xcb_window_t win;
xcb_connection_t *conn;

static long mmap_freq = 0;
static long ioctl_freq = 0;
// static long frame_count = 0;
static double frame_latency = 0.0;
static double time_spent_in_ioctl = 0.0;
static double start_frame = 0.0;
static uint64_t execbuf_count = 0;

static volatile bool qemu_setup_done = false;
static check *bufs_persistent = NULL;
static pthread_mutex_t gem_slots_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uint64_t gem_regular_advance(uint64_t size) {
  return PAGE_SIZE * (int)((PAGE_SIZE + size) / PAGE_SIZE);
}

static inline bool gem_bucket_has_capacity(const gem_slots_t *slots,
                                           uint64_t size,
                                           uint64_t region_base,
                                           uint64_t region_size) {
  return slots->host_address + size < region_base + region_size;
}


/*
 * The GEM arenas: guest-physical (= host-virtual) ranges where device mappings replace guest RAM.
 * Sizes that are a multiple of 2 MiB go to the hugepage arena, others to the regular one, with a
 * fallback to the other arena when one is full. Freed ranges (GEM_FREE, from the guest's munmap)
 * go back to a free list per arena and are reused first-fit; the device mapping there is replaced
 * by anonymous memory, so the host range stays valid for KVM. Regular allocations keep their
 * trailing guard page (as before); it is freed with the allocation's last page. When neither arena
 * has room, the guest's mmap fails with ENOMEM instead of QEMU aborting.
 */
#define GEM_FREE_MAX 8192
#define GEM_HUGE_ALIGN 0x200000ULL
typedef struct {
  uint64_t start, end;
} gem_range_t;
typedef struct {
  gem_range_t r[GEM_FREE_MAX];
  int n;
} gem_free_list_t;
static gem_free_list_t gem_free_regular, gem_free_huge;
/* regular allocations: mapped end -> guard page end, so a free of the last page frees the guard */
typedef struct {
  uint64_t mapped_end, guard_end;
} gem_guard_t;
static gem_guard_t *gem_guards;
static size_t gem_guards_n, gem_guards_cap;

static void gem_free_list_add(gem_free_list_t *fl, uint64_t start, uint64_t end) {
  if (start >= end)
    return;
  int i = 0;
  while (i < fl->n && fl->r[i].start < start)
    i++;
  /* merge with the previous and the next range where they touch */
  if (i > 0 && fl->r[i - 1].end >= start) {
    if (end > fl->r[i - 1].end)
      fl->r[i - 1].end = end;
    i--;
  } else {
    if (fl->n == GEM_FREE_MAX) {
      log_always("[mem] GEM free list full; 0x%lx bytes not reused\n", end - start);
      return;
    }
    memmove(&fl->r[i + 1], &fl->r[i], (fl->n - i) * sizeof(fl->r[0]));
    fl->r[i].start = start;
    fl->r[i].end = end;
    fl->n++;
  }
  while (i + 1 < fl->n && fl->r[i + 1].start <= fl->r[i].end) {
    if (fl->r[i + 1].end > fl->r[i].end)
      fl->r[i].end = fl->r[i + 1].end;
    memmove(&fl->r[i + 1], &fl->r[i + 2], (fl->n - i - 2) * sizeof(fl->r[0]));
    fl->n--;
  }
}

static bool gem_free_list_take(gem_free_list_t *fl, uint64_t size, uint64_t align,
                               uint64_t *out) {
  for (int i = 0; i < fl->n; i++) {
    uint64_t a = (fl->r[i].start + align - 1) & ~(align - 1);
    if (a < fl->r[i].start || a + size > fl->r[i].end)
      continue;
    uint64_t s = fl->r[i].start, e = fl->r[i].end;
    /* remove [a, a+size) from the range, keeping what is left on both sides */
    memmove(&fl->r[i], &fl->r[i + 1], (fl->n - i - 1) * sizeof(fl->r[0]));
    fl->n--;
    gem_free_list_add(fl, s, a);
    gem_free_list_add(fl, a + size, e);
    *out = a;
    return true;
  }
  return false;
}

static bool gem_bump(gem_slots_t *slots, uint64_t region_base, uint64_t region_size,
                     uint64_t size, uint64_t *out) {
  if (!gem_bucket_has_capacity(slots, size, region_base, region_size))
    return false;
  *out = slots->host_address;
  slots->host_address += size;
  slots->guest_address += size;
  return true;
}

/* Finds room for `size` bytes (the mapping plus, in the regular arena, its guard page). */
static bool gem_find(uint64_t size, bool huge, uint64_t *out, uint64_t *reserved) {
  uint64_t regular_size = gem_regular_advance(size);
  uint64_t huge_base = (uint64_t)(uintptr_t)hugepage_host_addr;
  uint64_t regular_base = (uint64_t)(uintptr_t)data_region_actual_address;
  for (int attempt = 0; attempt < 2; attempt++) {
    bool h = attempt == 0 ? huge : !huge;
    if (h) {
      if (gem_free_list_take(&gem_free_huge, size, GEM_HUGE_ALIGN, out) ||
          gem_bump(&gem_hugepage_slots, huge_base, huge_region_size, size, out)) {
        *reserved = size;
        gem_report_peak("hugepage", &gem_hugepage_peak, gem_hugepage_base,
                        gem_hugepage_slots.host_address, huge_region_size);
        return true;
      }
    } else {
      if (gem_free_list_take(&gem_free_regular, regular_size, PAGE_SIZE, out) ||
          gem_bump(&gem_slots, regular_base, data_region_size, regular_size, out)) {
        *reserved = regular_size;
        gem_report_peak("regular", &gem_regular_peak, gem_regular_base,
                        gem_slots.host_address, data_region_size);
        return true;
      }
    }
  }
  return false;
}

static bool gem_in_huge(uint64_t addr) {
  uint64_t base = (uint64_t)(uintptr_t)hugepage_host_addr;
  return addr >= base && addr < base + huge_region_size;
}

static bool gem_in_regular(uint64_t addr) {
  uint64_t base = (uint64_t)(uintptr_t)data_region_actual_address;
  return addr >= base && addr < base + data_region_size;
}

static uint64_t gem_allocate(uint64_t size, uint64_t prot, uint64_t flags, uint64_t fd,
                             uint64_t offset) {
  if (!size)
    return (uint64_t)(int64_t)-EINVAL;
  size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
  uint64_t addr, reserved;
  pthread_mutex_lock(&gem_slots_lock);
  bool found = gem_find(size, size % GEM_HUGE_ALIGN == 0, &addr, &reserved);
  pthread_mutex_unlock(&gem_slots_lock);
  if (!found) {
    log_always("[mem] GEM arenas full: no room for 0x%lx bytes; the guest's mmap gets ENOMEM\n",
               size);
    return (uint64_t)(int64_t)-ENOMEM;
  }
  void *p = mmap((void *)(uintptr_t)addr, size, prot, flags | MAP_SHARED | MAP_FIXED,
                 fd - fs_offset, offset);
  if (p == MAP_FAILED) {
    int err = errno;
    log_always("[mem] GEM mapping of 0x%lx bytes at 0x%lx failed: %s\n", size, addr,
               strerror(err));
    pthread_mutex_lock(&gem_slots_lock);
    gem_free_list_add(gem_in_huge(addr) ? &gem_free_huge : &gem_free_regular, addr,
                      addr + reserved);
    pthread_mutex_unlock(&gem_slots_lock);
    return (uint64_t)(int64_t)-err;
  }
  if (reserved > size) {
    pthread_mutex_lock(&gem_slots_lock);
    if (gem_guards_n == gem_guards_cap) {
      size_t cap = gem_guards_cap ? gem_guards_cap * 2 : 1024;
      gem_guard_t *g = realloc(gem_guards, cap * sizeof(*g));
      if (g) {
        gem_guards = g;
        gem_guards_cap = cap;
      }
    }
    if (gem_guards_n < gem_guards_cap) {
      gem_guards[gem_guards_n].mapped_end = addr + size;
      gem_guards[gem_guards_n].guard_end = addr + reserved;
      gem_guards_n++;
    }
    pthread_mutex_unlock(&gem_slots_lock);
  }
  log_gem("allocation of 0x%lx at 0x%lx (fd %ld)\n", size, addr, (long)fd);
  return addr;
}

/* The guest unmapped [addr, addr + len) of the arenas. */
static uint64_t gem_free(uint64_t addr, uint64_t len) {
  len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
  if (!len || !(gem_in_huge(addr) || gem_in_regular(addr)))
    return (uint64_t)(int64_t)-EINVAL;
  void *p = mmap((void *)(uintptr_t)addr, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED)
    return (uint64_t)(int64_t)-errno;
  uint64_t end = addr + len;
  pthread_mutex_lock(&gem_slots_lock);
  for (size_t i = 0; i < gem_guards_n; i++) {
    if (gem_guards[i].mapped_end == end) {
      end = gem_guards[i].guard_end;
      gem_guards[i] = gem_guards[--gem_guards_n];
      break;
    }
  }
  gem_free_list_add(gem_in_huge(addr) ? &gem_free_huge : &gem_free_regular, addr, end);
  pthread_mutex_unlock(&gem_slots_lock);
  return 0;
}

/* Defined in egl-redirect/xcb.c */
void create_pixmap_from_kbuf(check *bufs, int i, uint32_t size_bytes,
                             uint32_t stride);
int create_xcb_fence(check *bufs, int buf_index);
void create_and_setup_xcb_window(void);
void update_window(void);
void xcb_present(check *bufs, int cur);
int query_idle_fence_buffer(check *bufs, int cur);

int win_width = 300;
int win_height = 300;
uint64_t handle_to_size[1024];

int get_guest_tsc_info(uint64_t *offset, uint32_t *frequency_khz) {
  if (!first_cpu || !offset || !frequency_khz)
    return -EINVAL;

  struct kvm_device_attr attribute = {
      .group = KVM_VCPU_TSC_CTRL,
      .attr = KVM_VCPU_TSC_OFFSET,
      .addr = (uint64_t)(uintptr_t)offset,
  };
  int ret = ioctl(first_cpu->kvm_fd, KVM_GET_DEVICE_ATTR, &attribute);
  if (ret < 0)
    return -errno;

  ret = ioctl(first_cpu->kvm_fd, KVM_GET_TSC_KHZ, 0);
  if (ret < 0)
    return -errno;

  *frequency_khz = (uint32_t)ret;
  return 0;
}

/* The value a forwarded system call returns to the guest: the result, or -errno on failure (the
 * guest LibOS hands it to the application as the system call's return value). */
static inline uint64_t host_ret(long ret) {
  return ret < 0 ? (uint64_t)(int64_t)-errno : (uint64_t)ret;
}

/* The same for calls that return a host fd, which the guest sees shifted by fs_offset. */
static inline uint64_t host_fd_ret(long ret) {
  return ret < 0 ? (uint64_t)(int64_t)-errno : (uint64_t)ret + fs_offset;
}

static inline void complete_request(volatile comm_page_t *c) {
  __atomic_store_n(&c->req_bit, 0, __ATOMIC_RELEASE);
}

void setup_comm_data_regions(volatile comm_page_t *c) {
  while (c->magic != COMM_MAGIC) {
    usleep(1000);
  }
  log_always("COMM: 0x%llx\n", (unsigned long long)(uint64_t)(uintptr_t)c);
  log_always("COMM MAGIC: %p\n", (void *)*((uint64_t *)COMM_ADDR));
  /* the guest's comm ABI version comes with the magic (graphics_comm_abi.h); a mismatch (for
   * example a different GRAPHICS_FS_OFFSET) must stop here, loudly, not fail later */
  if (c->p4 != GRAPHICS_COMM_ABI_VERSION) {
    fprintf(stderr, "tirith listener: the guest speaks comm ABI version %llu, this QEMU version "
            "%llu: rebuild qemu-tirith against the guest's gramine-multiarch tree "
            "(tirith-build.sh)\n", (unsigned long long)c->p4,
            (unsigned long long)GRAPHICS_COMM_ABI_VERSION);
    abort();
  }
  c->p5 = GRAPHICS_COMM_ABI_VERSION;
  __atomic_store_n(&c->magic, 0x2, __ATOMIC_RELEASE);

  /* The data arena address: the launcher exports GRAMINE_DATA_ADDR into this
   * QEMU process's environment; the compiled 4G anchor remains the fallback.
   * All guest RAM above the low backend is identity-mapped, so the host side
   * of every guest physical address is the address itself. */
  void *data_region_addr = DATA_REGION;
  {
    const char *env_addr = getenv("GRAMINE_DATA_ADDR");
    if (env_addr && *env_addr) {
      char *end;
      unsigned long long v = strtoull(env_addr, &end, 10);
      if (*end == '\0' && v) {
        data_region_addr = (void *)(uintptr_t)v;
      } else {
        log_always("[sg] bad GRAMINE_DATA_ADDR '%s'; keeping the compiled anchor\n", env_addr);
      }
    }
  }

  volatile comm_page_t *d = (comm_page_t *)(uintptr_t)data_region_addr;
  while (d->magic != 0x1234567812344678ULL) {
    usleep(1000);
  }
  data_region_actual_address = data_region_addr;

  log_always("DATA MAGIC: %p\n", (void *)*((uint64_t *)data_region_addr));
  log_always("DATA REGION: (guest=%p, host=%p)\n",
             (void *)(uint64_t)data_region_addr, data_region_actual_address);

  while (*((uint64_t *)data_region_actual_address) != 0x1234567812344678ULL) {
    usleep(1000);
  }
  *((uint64_t *)data_region_actual_address) = 0x2;

  gem_slots.host_address = ((uint64_t)data_region_actual_address);
  gem_slots.guest_address = ((uint64_t)data_region_addr);
  gem_regular_base = gem_slots.host_address;

  // Wait for hugedata addr
  while (c->p1 == 0) {
    usleep(1000);
  }
  /* The guest sends the arena sizes alongside the hugepage address (c->p2 =
   * data size, c->p3 = hugepage size); adopt them when present. All guest RAM
   * is identity-mapped, so the host address of every guest physical address is
   * the address itself. */
  if (c->p2) {
    data_region_size = c->p2;
  }
  if (c->p3) {
    huge_region_size = c->p3;
  }
  hugepage_host_addr = (void *)(uintptr_t)c->p1;
  uint64_t huge_guest_addr = c->p1;

  if (huge_guest_addr <
      (uint64_t)(uintptr_t)data_region_addr + data_region_size) {
    log_always("FATAL: hugepage arena %p does not start at or after the data "
               "arena end %p\n",
               (void *)(uintptr_t)hugepage_host_addr,
               (void *)((uint64_t)(uintptr_t)data_region_addr + data_region_size));
    assert(false);
  }

  c->p1 = 0; // clear addr arg
  c->p2 = 0;
  c->p3 = 0;
  log_always("HUGEPAGE_DATA REGION: (guest=%p, host=%p)\n",
             (void *)(uint64_t)huge_guest_addr, hugepage_host_addr);

  while (*((uint64_t *)hugepage_host_addr) != 0x1234567812344678ULL) {
    usleep(1000);
  }
  log_always("HUGEPAGE_DATA MAGIC: %p\n",
             (void *)*((uint64_t *)hugepage_host_addr));
  *((uint64_t *)hugepage_host_addr) = 0x2;

  gem_hugepage_slots.host_address = ((uint64_t)hugepage_host_addr);
  gem_hugepage_slots.guest_address = ((uint64_t)huge_guest_addr);
  gem_hugepage_base = gem_hugepage_slots.host_address;

  create_and_setup_xcb_window();
  log_always("XCB window created\n");

  c->ret = 0;
  __atomic_store_n(&c->magic, 0x3, __ATOMIC_RELEASE);
}

extern void *mmap_listener(void *arg) {
  long index = (long)arg;

  /* TODO: Is this really needed? */
  // cpu_set_t cpuset;
  // CPU_ZERO(&cpuset);
  // CPU_SET(3 + index, &cpuset);
  // pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  /* Setup the communications and data regions */
  volatile comm_page_t *c =
      (comm_page_t *)(uintptr_t)(COMM_ADDR + (index * sizeof(comm_page_t)));

  if (index == 0) {
    setup_comm_data_regions(c);
    qemu_setup_done = true;
  } else {
    while (!qemu_setup_done) {
      usleep(1000);
    }
    while (c->magic != COMM_MAGIC) {
      usleep(1000);
    }
    c->magic = 0x2;
  }

  /* slots 1 and up: how long a slot spins after a request before it polls every 50 us
   * (TIRITH_SLOT_IDLE_US, default 2000) */
  const char *idle_env = getenv("TIRITH_SLOT_IDLE_US");
  long slot_idle_us = idle_env ? atol(idle_env) : 2000;
  /* slot 0 spins without pause unless TIRITH_SLOT0_IDLE_US is set: then it backs off like the
   * other slots after that many microseconds without a request (an idle graphics VM then does not
   * keep a host CPU busy, at the price of up to 50 us for the first request after a pause) */
  const char *idle0_env = getenv("TIRITH_SLOT0_IDLE_US");
  long slot0_idle_us = idle0_env ? atol(idle0_env) : -1;
  if (index == 0 && slot0_idle_us >= 0)
    slot_idle_us = slot0_idle_us;

  /*
   * Event Processing loop
   */
  uint64_t ret;

#if ENABLE_IDLE_SLEEP
  struct timespec last_request_time;
  clock_gettime(CLOCK_MONOTONIC, &last_request_time);
#endif

  for (;;) {
    RequestType req;

#if ENABLE_IDLE_SLEEP
    req = (RequestType)__atomic_load_n(&c->req_bit, __ATOMIC_ACQUIRE);
    if (req == 0) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long elapsed_ms = (now.tv_sec - last_request_time.tv_sec) * 1000 +
                        (now.tv_nsec - last_request_time.tv_nsec) / 1000000;
      if (elapsed_ms >= IDLE_TIMEOUT_MS) {
        usleep(IDLE_SLEEP_US);
      }
      continue;
    }
    clock_gettime(CLOCK_MONOTONIC, &last_request_time);
#else
    /* This is a latency-sensitive SPSC doorbell. Keep the idle path to one load and one branch;
     * running the full switch range check for every empty poll needlessly halves the listener's
     * polling rate. The acquire load pairs with the guest's release publication and makes the
     * request payload visible before dispatch. */
    if (index == 0 && slot0_idle_us < 0) {
      do {
        req = (RequestType)__atomic_load_n(&c->req_bit, __ATOMIC_ACQUIRE);
      } while (__builtin_expect(req == 0, true));
    } else {
      /* The other slots serve threads that run into a busy slot 0 (a GPU wait in another thread,
       * for example): spin for a while after a request, then poll every 50 us, so that idle
       * slots do not each keep a host CPU busy. */
      struct timespec last, now;
      clock_gettime(CLOCK_MONOTONIC, &last);
      for (;;) {
        req = (RequestType)__atomic_load_n(&c->req_bit, __ATOMIC_ACQUIRE);
        if (req)
          break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long idle_us = (now.tv_sec - last.tv_sec) * 1000000L + (now.tv_nsec - last.tv_nsec) / 1000;
        if (idle_us > slot_idle_us)
          usleep(50);
      }
    }
#endif

    switch (req) {
    case SYSCALL_LOGGING_ENABLE:
      set_syscall_logging(1);
      log_sg("Syscall logging enabled\n");
      complete_request(c);
      break;

    case SYSCALL_LOGGING_DISABLE:
      set_syscall_logging(0);
      log_sg("Syscall logging disabled\n");
      complete_request(c);
      break;

    case GEM_ALLOCATION:
      c->ret = gem_allocate(c->p1, c->p2, c->p3, c->p4, c->p5);
      complete_request(c);
      log_sg("mmap() returned: 0x%lx\n", c->ret);
      mmap_freq++;
      break;

    case GEM_FREE:
      c->ret = gem_free(c->p1, c->p2);
      complete_request(c);
      break;

    case FSTAT:
      ret = fstat(c->p1 - fs_offset, (struct stat *)c->p2);
      c->ret = host_ret((long)ret);
      log_sg("[syscall] fstat(%d, %p) = %ld\n", c->p1, (void *)c->p2, ret);
      complete_request(c);
      break;

    case IOCTL: {
      // uint64_t start,end;
      // struct timespec start_ts;
      // struct timespec end_ts;
      // uint64_t req_type = _IOC_NR(c->p2);
      // int already_done = 0;

      // /* Log the time it takes for the IOCTLs */
      // clock_gettime(CLOCK_MONOTONIC_RAW, &start_ts);
      /* A deadline the guest made relative to its own clock (see GRAPHICS_IOCTL_TIME_REL_MAGIC):
       * move it onto this host's CLOCK_MONOTONIC. The guest restores the field afterwards. */
      if (c->p5 == GRAPHICS_IOCTL_TIME_REL_MAGIC && c->p4) {
        int64_t *field = (int64_t *)(uintptr_t)(c->p3 + c->p4 - 1);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t host_now = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
        int64_t rel = *field;
        int64_t abs_deadline;
        if (rel <= 0)
          abs_deadline = host_now;
        else if (__builtin_add_overflow(host_now, rel, &abs_deadline))
          abs_deadline = INT64_MAX;
        *field = abs_deadline;
      }
      ret = host_ret(ioctl(c->p1 - fs_offset, c->p2, (void *)c->p3));
      // clock_gettime(CLOCK_MONOTONIC_RAW, &end_ts);

      // uint64_t start_ns = (uint64_t)start_ts.tv_sec * 1000000000ULL +
      //                     (uint64_t)start_ts.tv_nsec;
      // uint64_t end_ns = (uint64_t)end_ts.tv_sec * 1000000000ULL +
      //                     (uint64_t)end_ts.tv_nsec;
      // uint64_t delta_ns = end_ns - start_ns;

      // ioctl_count++;
      // ioctl_total_ns += delta_ns;
      // if (delta_ns > ioctl_max_ns)
      //     ioctl_max_ns = delta_ns;

      /* track additional derived counters */
      // ioctl_freq++;
      /* keep time_spent_in_ioctl in microseconds for consistency */
      // time_spent_in_ioctl += (double)delta_ns / 1000.0; /* microseconds */

      //       ioctl_stat *stat = get_ioctl_stat(c->p2);
      //       if (stat) {
      //         stat->count++;
      //         stat->total_ns += delta_ns;
      //         if (delta_ns > stat->max_ns)
      //           stat->max_ns = delta_ns;
      //         stat->last_fd = c->p1;
      //       }

      // if (c->p2 == DRM_IOCTL_I915_GEM_CREATE_EXT) {
      //     struct drm_i915_gem_create_ext *create = (struct
      //     drm_i915_gem_create_ext *)c->p3;
      //     // We need the handle AFTER the ioctl returns
      //     if (ret == 0 && create->handle < 1024) {
      //         handle_to_size[create->handle] = create->size;
      //         log_sg("Tracking BO: Handle %u, Size %llu\n", create->handle,
      //         create->size);
      //     }
      // }
      // if (c->p2 == DRM_IOCTL_I915_GEM_EXECBUFFER2 && c->p3) {
      //     const struct drm_i915_gem_execbuffer2 *execbuf =
      //         (const struct drm_i915_gem_execbuffer2 *)c->p3;
      //     execbuffer2_last_flags = execbuf->flags;

      //     struct drm_i915_gem_exec_object2 *objs = (struct
      //     drm_i915_gem_exec_object2 *)(uintptr_t)execbuf->buffers_ptr;

      //     for (int i = 0; i < execbuf->buffer_count; i++) {
      //         uint32_t h = objs[i].handle;
      //         uint64_t s = (h < 1024) ? handle_to_size[h] : 0;

      //         if (s == 4194304) {
      //             log_sg("Found 4MB BO in ExecBuf! Handle: %u, GPU Offset:
      //             0x%llx\n", h, objs[i].offset);
      //             // 1. Prepare the mmap request
      //             struct drm_i915_gem_mmap mmap_arg = {0};
      //             mmap_arg.handle = h;
      //             mmap_arg.size = s;
      //             mmap_arg.offset = 0; // In this ioctl, offset is for the BO
      //             internal offset

      //             // 2. Ask the driver to map this BO into our address space
      //             if (ioctl(c->p1 - fs_offset, DRM_IOCTL_I915_GEM_MMAP,
      //             &mmap_arg) == 0) {
      //                 unsigned char* ptr = (unsigned char*)mmap_arg.addr_ptr;
      //
      //                 // 3. Dump the first 256 bytes
      //                 log_sg("--- BO Header Dump (First 256B) ---\n");
      //                 for (int j = 0; j < 256; j++) {
      //                     log_sg("%02x ", ptr[j]);
      //                     if ((j + 1) % 16 == 0) log_sg("\n");
      //                 }

      //                 // 4. Dump 256 bytes from the center (2MB mark)
      //                 log_sg("--- BO Center Dump (256B from 2MB offset)
      //                 ---\n"); unsigned char* center_ptr = ptr + (2 * 1024 *
      //                 1024); for (int j = 0; j < 256; j++) {
      //                     log_sg("%02x ", center_ptr[j]);
      //                     if ((j + 1) % 16 == 0) log_sg("\n");
      //                 }

      //                 // 5. Cleanup: Always unmap when done!
      //                 munmap(ptr, s);
      //             } else {
      //                 log_sg("Failed to mmap BO handle %u\n", h);
      //             }
      //         }
      //     }
      // }

      // if (c->p2 == DRM_IOCTL_SYNCOBJ_WAIT && c->p3) {
      //     const struct drm_syncobj_wait *wait = (const struct
      //     drm_syncobj_wait *)c->p3; syncobj_wait_last_flags = wait->flags;
      // }

      c->ret = ret;

      // log_sg("[syscall] ioctl(fd=%d, req=0x%lx) = %d\n", c->p1 - fs_offset,
      // (void*) c->p2, ret);
      complete_request(c);
      break;
    }

    case OPEN:
      int oret = open((const char *)c->p1, c->p2, c->p3);
      if (oret < 0) {
        int open_errno = errno;
        log_sg("[syscall] open(\"%s\", %#lx) failed: %s\n",
               (const char *)c->p1, c->p2, strerror(open_errno));
        c->ret = (uint64_t)(int64_t)-open_errno;
      } else {
        c->ret = oret + fs_offset;
        // fprintf(stderr, "[QEMU] open(\"%s\", 0x%x) = %d (guest fd: %d)\n",
        // (const char*) c->p1, c->p2, oret, c->ret);
      }
      log_sg("[syscall] open(\"%s\", 0x%x) = %d\n", (const char *)c->p1, c->p2,
             oret);
      complete_request(c);
      break;

    case FCNTL:
      ret = fcntl(c->p1 - fs_offset, c->p2, c->p3);
      if (c->p2 == F_DUPFD_CLOEXEC || c->p2 == F_DUPFD) {
        c->ret = host_fd_ret((long)ret);
      } else {
        c->ret = host_ret((long)ret);
      }
      log_sg("[syscall] fcntl(%d, %d, 0x%lx) = %ld\n", c->p1, c->p2, c->p3,
             ret);
      complete_request(c);
      break;

    case HOST_FLOCK:
      ret = flock(c->p1 - fs_offset, c->p2);
      c->ret = host_ret((long)ret);
      log_sg("[syscall] flock(%d, 0x%lx) = %ld\n", c->p1, c->p2, ret);
      complete_request(c);
      break;

    case HOST_FTRUNCATE:
      ret = ftruncate(c->p1 - fs_offset, c->p2);
      c->ret = host_ret((long)ret);
      log_sg("[syscall] ftruncate(%d, 0x%lx) = %ld\n", c->p1, c->p2, ret);
      complete_request(c);
      break;

    case LSEEK:
      ret = lseek(c->p1 - fs_offset, c->p2, c->p3);
      c->ret = host_ret((long)ret);
      log_sg("[syscall] lseek(%d, %d, 0x%lx) = %ld\n", c->p1, c->p2, c->p3,
             ret);
      complete_request(c);
      break;

    case READLINK:
      ret = readlink((const char *)c->p1, (char *)c->p2, c->p3);
      c->ret = host_ret((long)ret);
      log_sg("[syscall] readlink(\"%s\", %p, %zu) = %zd\n", (const char *)c->p1,
             (void *)c->p2, c->p3, ret);
      complete_request(c);
      break;

    case NEWFSTAT:
      ret = fstatat(c->p1 - fs_offset, (const char *)c->p2,
                    (struct stat *)c->p3, c->p4);
      c->ret = host_ret((long)ret);
      complete_request(c);
      log_sg("[syscall] newfstat(%d, %p) = %d\n", c->p1, (void *)c->p3, ret);
      break;

    case GETDENT:
      ret = syscall(SYS_getdents64, c->p1 - fs_offset, c->p2, c->p3);
      c->ret = host_ret((long)ret);
      complete_request(c);
      log_sg("[syscall] getdents64(%d, %p, %zu) = %zd\n", c->p1, c->p2, c->p3,
             ret);
      break;

    case DUP:
      ret = dup(c->p1 - fs_offset);
      c->ret = host_fd_ret((long)ret);
      log_sg("[syscall] dup(%d) = %d\n", c->p1, ret);
      complete_request(c);
      break;

    case DUP_IDENTITY:
      ret = dup(c->p1);
      c->ret = host_fd_ret((long)ret);
      log_sg("[syscall] dup(%d) = %d\n", c->p1, ret);
      complete_request(c);
      break;

    case X11_SETUP:
      create_pixmap_from_kbuf((check *)c->p1, c->p2, c->p3, c->p4);
      create_xcb_fence((check *)c->p1, c->p2);
      log_sg("Completed mapping XCB pixmap and fence for buffer index %d\n",
             c->p2);
      complete_request(c);
      break;

    case X11_PRESENT:
      check *tmp_buf = (check *)c->p1;
      int cur = c->p2;
      xcb_present(tmp_buf, cur);

      complete_request(c);
      frame_count++;

      // if(frame_count%5000 == 0){
      //     /* Print IOCTL statistics collected since last report and reset
      //     counters */ print_ioctl_stats();

      //     /* Reset cumulative and per-request stats */
      //     ioctl_count = 0;
      //     ioctl_total_ns = 0;
      //     ioctl_max_ns = 0;
      //     ioctl_stats_used = 0;
      //     execbuffer2_last_flags = 0;
      //     syncobj_wait_last_flags = 0;
      //     memset(ioctl_stats, 0, sizeof(ioctl_stats));

      //     /* Reset runtime counters */
      //     frame_latency = 0;
      //     ioctl_freq = 0;
      //     mmap_freq = 0;
      //     time_spent_in_ioctl = 0;
      //     execbuf_count = 0;
      // }
      break;

    case X11_IDLE_FENCE_BUFFER:
      tmp_buf = (check *)c->p1;
      cur = c->p2;
      c->ret = query_idle_fence_buffer(tmp_buf, cur);

      complete_request(c);
      break;

    case CLOSE:
      ret = close(c->p1 - fs_offset);
      c->ret = host_ret((long)ret);
      log_sg("[syscall] close(%d) = %d\n", c->p1, ret);
      complete_request(c);
      break;

    case UPDATE_WINDOW_SIZE:
      log_sg("Received request to update window size to %dx%d\n", c->p1, c->p2);
      win_width = c->p1;
      win_height = c->p2;
      if (c->p3) {
        xcb_window_t app_win = (xcb_window_t)c->p3;
        set_presentation_window(app_win);
      }
      /* The application and the window manager own the real window's
       * geometry, focus and input. update_window() only resizes QEMU's
       * embedded DRI3 presentation surface. */
      update_window();
      complete_request(c);
      break;

    case UPDATE_INPUT_MASK:
      set_shared_input_mask((xcb_window_t)c->p1, (uint32_t)c->p2);
      c->ret = 0;
      complete_request(c);
      break;

    case READ:
      ret = read(c->p1 - fs_offset, (void *)(uintptr_t)c->p2, c->p3);
      c->ret = host_ret((long)ret);
      log_sg("[syscall] read(%d, %d, %d) = %d\n", c->p1, c->p2, c->p3, ret);
      complete_request(c);
      break;

    case WRITE:
      ret = write(c->p1 - fs_offset, (const void *)(uintptr_t)c->p2, c->p3);
      c->ret = host_ret((long)ret);
      log_sg("[syscall] write(%d, 0x%lx, %d) = %d\n", c->p1, c->p2, c->p3, ret);
      complete_request(c);
      break;

    case SYSV_SEMGET: {
      int semid = semget((key_t)c->p1, (int)c->p2, (int)c->p3);
      c->ret = semid < 0 ? (uint64_t)(int64_t)-errno : (uint64_t)semid;
      complete_request(c);
      break;
    }

    case SYSV_SEMCTL: {
      int semret = semctl((int)c->p1, (int)c->p2, (int)c->p3,
                          (unsigned long)c->p4);
      c->ret = semret < 0 ? (uint64_t)(int64_t)-errno : (uint64_t)semret;
      complete_request(c);
      break;
    }

    case SYSV_SEMOP: {
      struct sembuf op = {
          .sem_num = (unsigned short)c->p2,
          .sem_op = (short)c->p3,
          /* Never block the single request listener. The LibOS retries blocking
           * operations so another guest thread can still submit the matching post. */
          .sem_flg = (short)c->p4 | IPC_NOWAIT,
      };
      int semret = semop((int)c->p1, &op, 1);
      c->ret = semret < 0 ? (uint64_t)(int64_t)-errno : (uint64_t)semret;
      complete_request(c);
      break;
    }

    default:
      /* an unknown request: complete it, or the guest spins forever */
      c->ret = (uint64_t)(int64_t)-ENOSYS;
      complete_request(c);
      break;
    }
  }
  return NULL;
}
