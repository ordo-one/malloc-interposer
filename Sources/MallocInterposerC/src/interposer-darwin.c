//
// Copyright (c) 2022 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

#include <assert.h>
#if __APPLE__

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <interposer.h>

// ---------------------------------------------------------------------------
// Counting model
//
// Counters live in a per-thread block (TLS). On the hot path that's a plain
// pointer load + a handful of non-atomic stores: zero atomic round-trips,
// zero cross-thread cache-line ping-pong. Reads aggregate across the linked
// list of live thread blocks plus an accumulator for blocks belonging to
// threads that have already exited.
//
// We keep g_counting_enabled atomic so enable()/disable() from one thread
// is immediately visible to others, but the per-thread counts themselves
// are owned by the writing thread.
// ---------------------------------------------------------------------------

typedef struct counter_block {
    int64_t malloc_bytes;
    // size_class[0] = small (<= page_size), size_class[1] = large (> page_size).
    // Indexed-store keeps count_malloc branchless, which lets clang keep the
    // counting body inlined into the malloc hot path instead of outlining it
    // into a cold helper.
    int64_t malloc_size_class[2];
    int64_t free_count;
    int64_t free_bytes;
    struct counter_block *next;
} counter_block_t;

static _Atomic bool g_counting_enabled = false;

static pthread_mutex_t g_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static counter_block_t *g_blocks_head = NULL;
// Aggregated counts inherited from threads that have already exited. The
// destructor folds the dying thread's counts here so a subsequent reader
// still sees them.
static counter_block_t g_dead_aggregate = {0};

static _Thread_local counter_block_t *t_block = NULL;
static pthread_key_t g_block_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

// Page size — pre-seeded with the Darwin/arm64 default so the value is valid
// even during very early dylib-init (libobjc's `map_images` runs DYLD_INTERPOSE
// callbacks into us before our own constructor fires; if g_page_size were 0
// the page-alignment fast path would compute `(uintptr_t)p & SIZE_MAX`,
// declare every pointer non-page-aligned, then crash trying to read magic
// bytes from the page before a page-aligned static address). The constructor
// refreshes it just in case the runtime page size differs.
#if defined(__aarch64__) || defined(__arm64__)
static size_t g_page_size = 16384;
#else
static size_t g_page_size = 4096;
#endif

__attribute__((constructor)) static void init_page_size(void) {
    g_page_size = (size_t)getpagesize();
}

static void block_destructor(void *arg) {
    counter_block_t *b = (counter_block_t *)arg;
    if (!b) return;

    pthread_mutex_lock(&g_list_mutex);
    g_dead_aggregate.malloc_bytes        += b->malloc_bytes;
    g_dead_aggregate.malloc_size_class[0] += b->malloc_size_class[0];
    g_dead_aggregate.malloc_size_class[1] += b->malloc_size_class[1];
    g_dead_aggregate.free_count          += b->free_count;
    g_dead_aggregate.free_bytes          += b->free_bytes;

    counter_block_t **cur = &g_blocks_head;
    while (*cur) {
        if (*cur == b) { *cur = b->next; break; }
        cur = &(*cur)->next;
    }
    pthread_mutex_unlock(&g_list_mutex);

    // Calls from inside this dylib resolve directly to libsystem, so this
    // is the real libc free — not a recursive call into ourselves.
    free(b);
}

static void init_block_key(void) {
    pthread_key_create(&g_block_key, block_destructor);
}

static __attribute__((noinline)) counter_block_t *tls_block_init(void) {
    pthread_once(&g_key_once, init_block_key);

    counter_block_t *b = (counter_block_t *)calloc(1, sizeof(counter_block_t));
    if (!b) return NULL;
    pthread_setspecific(g_block_key, b);

    pthread_mutex_lock(&g_list_mutex);
    b->next = g_blocks_head;
    g_blocks_head = b;
    pthread_mutex_unlock(&g_list_mutex);

    t_block = b;
    return b;
}

static __attribute__((always_inline)) counter_block_t *get_tls_block(void) {
    counter_block_t *b = t_block;
    if (__builtin_expect(b == NULL, 0)) {
        b = tls_block_init();
    }
    return b;
}

// Public API ----------------------------------------------------------------

void malloc_interposer_enable(void) {
    atomic_store_explicit(&g_counting_enabled, true, memory_order_release);
}

void malloc_interposer_disable(void) {
    atomic_store_explicit(&g_counting_enabled, false, memory_order_release);
}

void malloc_interposer_reset(void) {
    pthread_mutex_lock(&g_list_mutex);
    memset(&g_dead_aggregate, 0, sizeof(g_dead_aggregate));
    for (counter_block_t *b = g_blocks_head; b; b = b->next) {
        // Relaxed stores — the writing thread may race, but reset is called
        // between measured regions (workload paused), so the race is benign
        // in practice.
        __atomic_store_n(&b->malloc_bytes,        0, __ATOMIC_RELAXED);
        __atomic_store_n(&b->malloc_size_class[0], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&b->malloc_size_class[1], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&b->free_count,          0, __ATOMIC_RELAXED);
        __atomic_store_n(&b->free_bytes,          0, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_list_mutex);
}

void malloc_interposer_get_stats(int64_t *malloc_count, int64_t *malloc_bytes,
                                 int64_t *malloc_small, int64_t *malloc_large,
                                 int64_t *free_count, int64_t *free_bytes) {
    int64_t mb = 0, ms = 0, ml = 0, fc = 0, fb = 0;

    pthread_mutex_lock(&g_list_mutex);
    mb = g_dead_aggregate.malloc_bytes;
    ms = g_dead_aggregate.malloc_size_class[0];
    ml = g_dead_aggregate.malloc_size_class[1];
    fc = g_dead_aggregate.free_count;
    fb = g_dead_aggregate.free_bytes;

    for (counter_block_t *b = g_blocks_head; b; b = b->next) {
        mb += __atomic_load_n(&b->malloc_bytes,        __ATOMIC_RELAXED);
        ms += __atomic_load_n(&b->malloc_size_class[0], __ATOMIC_RELAXED);
        ml += __atomic_load_n(&b->malloc_size_class[1], __ATOMIC_RELAXED);
        fc += __atomic_load_n(&b->free_count,          __ATOMIC_RELAXED);
        fb += __atomic_load_n(&b->free_bytes,          __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_list_mutex);

    // Total = small + large; we don't store it separately because the split
    // counters already carry all the information.
    *malloc_count = ms + ml;
    *malloc_bytes = mb;
    *malloc_small = ms;
    *malloc_large = ml;
    *free_count   = fc;
    *free_bytes   = fb;
}

// ---------------------------------------------------------------------------

#define DYLD_INTERPOSE(_replacement,_replacee) \
   __attribute__((used)) static struct { const void *replacement; const void *replacee; } _interpose_##_replacee \
            __attribute__ ((section("__DATA,__interpose"))) = { (const void *)(unsigned long)&_replacement, (const void *)(unsigned long)&_replacee };

// Inline counting helpers ---------------------------------------------------
//
// All counter updates land in the calling thread's TLS block. The block is
// created lazily on first use. Once the pointer is cached in the _Thread_local
// slot, every subsequent call is a non-atomic increment on private memory.

static __attribute__((always_inline)) void count_malloc(size_t size) {
    counter_block_t *b = get_tls_block();
    if (__builtin_expect(b == NULL, 0)) return;
    b->malloc_bytes += (int64_t)size;
    // Branchless small/large split — index 0 is small, 1 is large.
    b->malloc_size_class[size > g_page_size]++;
}

static __attribute__((always_inline)) void count_free(size_t size) {
    counter_block_t *b = get_tls_block();
    if (__builtin_expect(b == NULL, 0)) return;
    b->free_count++;
    b->free_bytes += (int64_t)size;
}

// Header-write helpers ------------------------------------------------------

static __attribute__((always_inline)) void *write_header(void *raw, size_t size) {
    malloc_header_t *hdr = (malloc_header_t *)raw;
    hdr->requested_size = size;
    hdr->reserved = 0;
    hdr->magic = MALLOC_INTERPOSER_MAGIC;
    return malloc_interposer_user_for(raw);
}

// Replacement functions -----------------------------------------------------
//
// On Darwin, calls from inside this dylib resolve directly to libsystem
// (DYLD_INTERPOSE only rewrites calls in OTHER images), so plain `malloc`,
// `free` etc. below are libsystem's, not recursive into ourselves.

__attribute__((flatten)) void *replacement_malloc(size_t size) {
    void *raw = malloc(size + sizeof(malloc_header_t));
    if (!raw) return NULL;
    if (atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        count_malloc(size);
    }
    return write_header(raw, size);
}

__attribute__((flatten)) void replacement_free(void *user_ptr) {
    if (!user_ptr) return;
    if (malloc_interposer_is_ours(user_ptr)) {
        malloc_header_t *hdr = malloc_interposer_header_for(user_ptr);
        if (atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
            count_free(hdr->requested_size);
        }
        free(hdr);
    } else {
        // External pointer (rare on Darwin once DYLD_INTERPOSE is active).
        // Fall back to libc bookkeeping for byte accounting.
        if (atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
            count_free(malloc_size(user_ptr));
        }
        free(user_ptr);
    }
}

__attribute__((flatten)) void *replacement_calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        // Let libc surface the overflow exactly as the user would expect.
        return calloc(count, size);
    }
    // libc calloc zeros the entire allocation including where the header
    // sits; we then overwrite those 16 bytes. Slightly redundant but simple.
    void *raw = calloc(1, total + sizeof(malloc_header_t));
    if (!raw) return NULL;
    if (atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        count_malloc(total);
    }
    return write_header(raw, total);
}

__attribute__((flatten)) void *replacement_realloc(void *user_ptr, size_t new_size) {
    if (!user_ptr) return replacement_malloc(new_size);
    if (new_size == 0) {
        replacement_free(user_ptr);
        return NULL;
    }

    bool counting = atomic_load_explicit(&g_counting_enabled, memory_order_relaxed);

    if (malloc_interposer_is_ours(user_ptr)) {
        malloc_header_t *old_hdr = malloc_interposer_header_for(user_ptr);
        size_t old_size = old_hdr->requested_size;

        void *new_raw = realloc(old_hdr, new_size + sizeof(malloc_header_t));
        if (!new_raw) return NULL;

        if (counting) {
            count_free(old_size);
            count_malloc(new_size);
        }
        // realloc may have moved memory; rewrite the header unconditionally.
        return write_header(new_raw, new_size);
    }

    // External pointer; use libc bookkeeping.
    size_t old_size = malloc_size(user_ptr);
    void *new_ptr = realloc(user_ptr, new_size);
    if (!new_ptr) return NULL;
    if (counting) {
        count_free(old_size);
        count_malloc(malloc_size(new_ptr));
    }
    return new_ptr;
}

void *replacement_reallocf(void *user_ptr, size_t new_size) {
    void *new_ptr = replacement_realloc(user_ptr, new_size);
    // reallocf semantics: if reallocation fails, free the original pointer.
    // replacement_realloc handles size==0 (frees) and ptr==NULL (no original)
    // itself, so only free on the actual-failure case.
    if (!new_ptr && user_ptr && new_size != 0) {
        replacement_free(user_ptr);
    }
    return new_ptr;
}

// ---- Aligned/legacy paths: alignment requirements rule out the header ----
// We let libc place a properly-aligned chunk and use malloc_size on free
// (paid by the rare allocations that use these). Magic check on free will
// fail, falling through to the external path that reads malloc_size.

void *replacement_valloc(size_t size) {
    void *ptr = valloc(size);
    if (ptr && atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        count_malloc(malloc_size(ptr));
    }
    return ptr;
}

int replacement_posix_memalign(void **memptr, size_t alignment, size_t size) {
    int result = posix_memalign(memptr, alignment, size);
    if (result == 0
        && memptr
        && atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        count_malloc(malloc_size(*memptr));
    }
    return result;
}

// ---- Zone-level wrappers (rarely hit by user code) ------------------------

void *replacement_malloc_zone_malloc(malloc_zone_t *zone, size_t size) {
    void *raw = malloc_zone_malloc(zone, size + sizeof(malloc_header_t));
    if (!raw) return NULL;
    if (atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        count_malloc(size);
    }
    return write_header(raw, size);
}

void *replacement_malloc_zone_calloc(malloc_zone_t *zone, size_t num_items, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(num_items, size, &total)) {
        return malloc_zone_calloc(zone, num_items, size);
    }
    void *raw = malloc_zone_calloc(zone, 1, total + sizeof(malloc_header_t));
    if (!raw) return NULL;
    if (atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        count_malloc(total);
    }
    return write_header(raw, total);
}

void *replacement_malloc_zone_valloc(malloc_zone_t *zone, size_t size) {
    void *ptr = malloc_zone_valloc(zone, size);
    if (ptr && atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        count_malloc(malloc_size(ptr));
    }
    return ptr;
}

void *replacement_malloc_zone_realloc(malloc_zone_t *zone, void *user_ptr, size_t new_size) {
    if (!user_ptr) return replacement_malloc_zone_malloc(zone, new_size);
    if (new_size == 0) {
        replacement_malloc_zone_free(zone, user_ptr);
        return NULL;
    }

    bool counting = atomic_load_explicit(&g_counting_enabled, memory_order_relaxed);

    if (malloc_interposer_is_ours(user_ptr)) {
        malloc_header_t *old_hdr = malloc_interposer_header_for(user_ptr);
        size_t old_size = old_hdr->requested_size;
        void *new_raw = malloc_zone_realloc(zone, old_hdr, new_size + sizeof(malloc_header_t));
        if (!new_raw) return NULL;
        if (counting) {
            count_free(old_size);
            count_malloc(new_size);
        }
        return write_header(new_raw, new_size);
    }

    size_t old_size = malloc_size(user_ptr);
    void *new_ptr = malloc_zone_realloc(zone, user_ptr, new_size);
    if (!new_ptr) return NULL;
    if (counting) {
        count_free(old_size);
        count_malloc(malloc_size(new_ptr));
    }
    return new_ptr;
}

void *replacement_malloc_zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size) {
    void *ptr = malloc_zone_memalign(zone, alignment, size);
    if (ptr && atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        count_malloc(malloc_size(ptr));
    }
    return ptr;
}

void replacement_malloc_zone_free(malloc_zone_t *zone, void *user_ptr) {
    if (!user_ptr) return;
    if (malloc_interposer_is_ours(user_ptr)) {
        malloc_header_t *hdr = malloc_interposer_header_for(user_ptr);
        if (atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
            count_free(hdr->requested_size);
        }
        malloc_zone_free(zone, hdr);
    } else {
        if (atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
            count_free(malloc_size(user_ptr));
        }
        malloc_zone_free(zone, user_ptr);
    }
}

// ---- Size queries ---------------------------------------------------------
// External code that calls malloc_size on one of our pointers would see the
// offset address (not the libc chunk start), so libsystem can't find it in
// any zone. Interpose to return the requested size from the header.

size_t replacement_malloc_size(const void *user_ptr) {
    if (!user_ptr) return 0;
    if (malloc_interposer_is_ours(user_ptr)) {
        return malloc_interposer_header_for((void *)user_ptr)->requested_size;
    }
    return malloc_size(user_ptr);
}

DYLD_INTERPOSE(replacement_free, free)
DYLD_INTERPOSE(replacement_malloc, malloc)
DYLD_INTERPOSE(replacement_realloc, realloc)
DYLD_INTERPOSE(replacement_calloc, calloc)
DYLD_INTERPOSE(replacement_reallocf, reallocf)
DYLD_INTERPOSE(replacement_valloc, valloc)
DYLD_INTERPOSE(replacement_posix_memalign, posix_memalign)
DYLD_INTERPOSE(replacement_malloc_size, malloc_size)
DYLD_INTERPOSE(replacement_malloc_zone_malloc, malloc_zone_malloc)
DYLD_INTERPOSE(replacement_malloc_zone_calloc, malloc_zone_calloc)
DYLD_INTERPOSE(replacement_malloc_zone_valloc, malloc_zone_valloc)
DYLD_INTERPOSE(replacement_malloc_zone_realloc, malloc_zone_realloc)
DYLD_INTERPOSE(replacement_malloc_zone_memalign, malloc_zone_memalign)
DYLD_INTERPOSE(replacement_malloc_zone_free, malloc_zone_free)
#endif
