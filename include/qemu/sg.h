/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#pragma once
#ifndef QEMU_SG_H
#define QEMU_SG_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <EGL/egl.h>
#include <GL/gl.h>
#include <EGL/eglext.h>
#include <xcb/xcb.h>
#include <xcb/sync.h>

/*
 * The shared Tirith graphics ABI -- the comm page, the request ids, the common
 * buffer state and the fallback arena geometry -- defined once in the Saguaro
 * tree (libos/include/graphics_comm_abi.h) and resolved by the build (build.sh
 * adds that directory of SAGUARO_ROOT to the include path). Everything the guest
 * stores into, or this listener serves out of, the comm page comes from there.
 */
#include "graphics_comm_abi.h"

/* QEMU-side name for the shared request ids. */
typedef enum graphics_request RequestType;

/* Uncomment to enable debugging */
// #define COMMAND_DEBUG
// #define GEM_DEBUG
// #define STAT_DEBUG

/* Syscall logging toggle */
extern int syscall_logging_enabled;

static void set_syscall_logging(int enable) { syscall_logging_enabled = enable ? 1 : 0; }

#ifdef COMMAND_DEBUG
#define log_sg(fmt, ...) \
    do { \
        if (syscall_logging_enabled) { \
            fprintf(stderr, "(qemu: COMMAND) "); \
            fprintf(stderr, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#define log_sg(fmt, ...) do {} while (0)
#endif

#ifdef GEM_DEBUG
#define log_gem(fmt, ...) \
    do { \
        fprintf(stderr, "(qemu: GEM) "); \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
    } while (0)
#else
#define log_gem(fmt, ...) do {} while (0)
#endif

#ifdef STAT_DEBUG
#define log_stat(fmt, ...) \
    do { \
        fprintf(stderr, "(qemu: STAT) "); \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
    } while (0)
#else
#define log_stat(fmt, ...) do {} while (0)
#endif

#define log_always(fmt, ...) \
    do { \
        fprintf(stderr, "(qemu) "); \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
    } while (0)

/* sg-listener.c */
extern xcb_window_t win;
extern xcb_connection_t *conn;
void set_presentation_window(xcb_window_t window);
void set_shared_input_mask(xcb_window_t window, uint32_t event_mask);
int get_guest_tsc_info(uint64_t *offset, uint32_t *frequency_khz);

extern void* data_region_actual_address;

/* Arena sizes in bytes, sent by the guest during comm-page setup (c->p2 for
 * the data arena, c->p3 for the hugepage arena); initialized from the compiled
 * ABI fallbacks. */
extern size_t data_region_size;
extern size_t huge_region_size;

/* Per-buffer state; mirrors the guest library's `struct buffer` (common.h).
 * Its layout is part of the ABI -- see graphics_comm_abi.h. */
#define NUM_BUFFERS 3
typedef struct buffer {
    struct gbm_bo *bo;
    int bo_fd;
    xcb_pixmap_t pixmap;
    EGLImageKHR image;
    GLuint tex;
    GLuint fbo;
    int shm_fence_fd;
    struct xshmfence *shm_fence;
    xcb_sync_fence_t sync_fence;
    int idle_fence_fd;
    struct xshmfence *idle_fence;
    xcb_sync_fence_t idle_sync_fence;
    GLuint rbo_depth;
} check;

/* Fallback arena anchors; the runtime addresses live in
 * `data_region_actual_address`/`hugepage_host_addr` (sg-listener.c). */
static void* DATA_REGION = (void*)GRAPHICS_GEM_DATA_START;
static void* UNMAP_DATA_MSG = (void*)0x1234567f1234567fULL;

void* mmap_listener(void* arg);

extern int win_width;
extern int win_height;

static const size_t PAGE_SIZE = 4 * 1024;

#define sys_exec_vmexits 549
#define sys_sg_vmexits_printreset 550
#define I915_EXEC_ASYNC (1<<15)
#define LOG_BATCH_SIZE 100  // Number of entries to hold in memory before flushing

static inline uint64_t clock_gettime_ns(void)
{
    unsigned int lo, hi;
    asm volatile("lfence; rdtscp" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

// Structure to hold our raw measurements
typedef struct {
    uint64_t req_type;
    int frame;
    uint64_t cycles;
    int ret;
    uint64_t flags;
} log_entry_t;

static void prefault_range(void *addr, size_t len) {
    char *p = addr;
    for (size_t off = 0; off < len; off += PAGE_SIZE)
        memset((void*)(p + off), 0, PAGE_SIZE);
}
#endif