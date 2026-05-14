//
// Copyright (c) 2022 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef INTERPOSER_H
#define INTERPOSER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#if __APPLE__
#  include <malloc/malloc.h>
#endif

// ---------------------------------------------------------------------------
// Header-prefix size tracking
//
// Each allocation we hand back to the caller is preceded by a 16-byte header
// that records the requested size and a magic word. On free/realloc we read
// the header instead of calling malloc_size/malloc_usable_size, eliminating
// a libc round-trip per call. Pointers that didn't go through the interposer
// (e.g., aligned-alloc slow path, allocations that pre-date hooking) are
// detected by a failing magic check and fall back to libc bookkeeping.
//
// The header is exactly 16 bytes so user_ptr inherits the 16-byte alignment
// of the underlying libc allocation.
// ---------------------------------------------------------------------------

#define MALLOC_INTERPOSER_MAGIC 0xC0FFEE5AU

typedef struct {
    size_t   requested_size; // offset 0
    uint32_t reserved;       // offset 8
    uint32_t magic;          // offset 12 — last 4 bytes for fast probe via *(user_ptr - 4)
} malloc_header_t;

_Static_assert(sizeof(malloc_header_t) == 16,
               "malloc_header_t must be 16 bytes to preserve 16-byte alignment");

static inline malloc_header_t *malloc_interposer_header_for(void *user_ptr) {
    return (malloc_header_t *)((char *)user_ptr - sizeof(malloc_header_t));
}

static inline void *malloc_interposer_user_for(void *raw) {
    return (char *)raw + sizeof(malloc_header_t);
}

static inline bool malloc_interposer_is_ours(const void *user_ptr) {
    if (!user_ptr) return false;
    // Probe the last 4 bytes of the would-be header. For our pointers this
    // reads our magic; for external pointers it reads into libc chunk
    // metadata (always present and readable for libc-malloc'd pointers).
    uint32_t magic;
    memcpy(&magic, (const char *)user_ptr - sizeof(uint32_t), sizeof(magic));
    return magic == MALLOC_INTERPOSER_MAGIC;
}

// ---------------------------------------------------------------------------
// Public API
//
// The interposer is always linked into the process — but it only updates its
// counters while "enabled". Toggle the enabled flag around the region you
// want to measure. The replacement functions themselves stay wired up
// regardless, so toggling is cheap (one atomic store) and doesn't perturb
// dynamic linker state.

/**
 * Enable counting.
 *
 * Calls to malloc/free/calloc/realloc/... made after this returns will
 * update the counters. Idempotent.
 */
void malloc_interposer_enable(void);

/**
 * Disable counting.
 *
 * Subsequent allocation calls bypass the counters but still go through the
 * interposer's header-prefix machinery (so pointers allocated while disabled
 * are still freeable correctly). Idempotent.
 */
void malloc_interposer_disable(void);

/**
 * Atomically reset all counters to zero.
 *
 * Does not change the enabled/disabled state. Safe to call concurrently with
 * allocation traffic — the reset is a sequence of relaxed stores followed
 * by a release fence, so a subsequent acquire-side read will see all zeros
 * for any counter it observes.
 */
void malloc_interposer_reset(void);

/**
 * Read a snapshot of every counter.
 *
 * Each parameter receives the current value of the corresponding counter,
 * read with relaxed memory order. The six reads are independent — under
 * concurrent allocation they are not guaranteed to form a mutually
 * consistent snapshot. To get a consistent snapshot, bracket the measured
 * region with #malloc_interposer_enable / #malloc_interposer_disable.
 *
 * @param malloc_count   Number of allocations counted.
 * @param malloc_bytes   Total requested bytes allocated.
 * @param malloc_small   Allocations with requested size <= page size.
 * @param malloc_large   Allocations with requested size  > page size.
 * @param free_count     Number of free calls counted.
 * @param free_bytes     Total bytes freed.
 *
 * All output pointers must be non-NULL.
 */
void malloc_interposer_get_stats(int64_t *malloc_count, int64_t *malloc_bytes,
                                 int64_t *malloc_small, int64_t *malloc_large,
                                 int64_t *free_count, int64_t *free_bytes);

// Replacement functions (used internally for DYLD_INTERPOSE and Linux overrides)
void *replacement_malloc(size_t size);
void replacement_free(void *ptr);
void *replacement_calloc(size_t nmemb, size_t size);
void *replacement_realloc(void *ptr, size_t size);
void *replacement_reallocf(void *ptr, size_t size);
void *replacement_valloc(size_t size);
int replacement_posix_memalign(void **memptr, size_t alignment, size_t size);
#if __APPLE__
size_t replacement_malloc_size(const void *ptr);
#else
size_t replacement_malloc_usable_size(void *ptr);
#endif

// On Linux we use LD_PRELOAD to interpose the standard malloc functions
// and we have to declare them ourselves
#if !__APPLE__
void free(void *ptr);
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *reallocf(void *ptr, size_t size);
void *valloc(size_t size);
int posix_memalign(void **memptr, size_t alignment, size_t size);
size_t malloc_usable_size(void *ptr);
#endif

#if __APPLE__
void *replacement_malloc_zone_malloc(malloc_zone_t *zone, size_t size);
void *replacement_malloc_zone_calloc(malloc_zone_t *zone, size_t num_items, size_t size);
void *replacement_malloc_zone_valloc(malloc_zone_t *zone, size_t size);
void *replacement_malloc_zone_realloc(malloc_zone_t *zone, void *ptr, size_t size);
void *replacement_malloc_zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size);
void replacement_malloc_zone_free(malloc_zone_t *zone, void *ptr);
#endif

#endif
