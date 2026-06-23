//
// Copyright (c) 2022 Ordo One AB.
// Copyright (c) 2017-2018 Apple Inc. and the SwiftNIO project authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// SPDX-License-Identifier: Apache-2.0
//
// Portions of this file are derived from the SwiftNIO open source project's
// allocation-counter test framework — specifically the file
// IntegrationTests/allocation-counter-tests-framework/.../hooked-functions-unix.c
// (https://github.com/apple/swift-nio). The libc-resolution machinery
// originates there: the dlsym(RTLD_NEXT, …) lookup cached in atomic globals,
// the recursive-malloc-during-dlsym BSS bump allocator, and the
// JUMP_INTO_LIBC_FUN macro.
//
// Modifications by Ordo One AB: replaced SwiftNIO's global atomic counters with
// a per-thread TLS counting model; added header-prefix per-allocation size
// tracking and pointer classification, a small/large size-class split, runtime
// enable/disable/reset gating, alignment-honouring aligned-allocation paths,
// overflow-checked calloc, and malloc_usable_size interposition; and removed
// SwiftNIO's socket / file-descriptor-tracking hooks.
//

#ifndef __APPLE__

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <malloc.h>
#include <pthread.h>

#include <interposer.h>

// The classifier reads the word *before* a user pointer, which AddressSanitizer
// and ThreadSanitizer treat as out of bounds, so the global malloc overrides
// are compiled out under those sanitizers (a benchmarked process is never
// sanitized anyway).
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define MALLOC_INTERPOSER_SANITIZER 1
#  endif
#endif
#if !defined(MALLOC_INTERPOSER_SANITIZER) && (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
#  define MALLOC_INTERPOSER_SANITIZER 1
#endif
#ifndef MALLOC_INTERPOSER_SANITIZER
#  define MALLOC_INTERPOSER_SANITIZER 0
#endif

int malloc_interposer_global_hooks_installed(void) {
    return !MALLOC_INTERPOSER_SANITIZER;
}

/* a big block of memory that we'll use for recursive mallocs */
static char g_recursive_malloc_mem[10 * 1024 * 1024] = {0};
/* the index of the first free byte */
static _Atomic ptrdiff_t g_recursive_malloc_next_free_ptr = 0;

#define LIBC_SYMBOL(_fun) "" # _fun

/* Some thread-local flags we use to check if we're recursively in a hooked function. */
static __thread bool g_in_malloc = false;
static __thread bool g_in_realloc = false;
static __thread bool g_in_free = false;
static __thread bool g_in_malloc_usable_size = false;
static __thread bool g_in_posix_memalign = false;
static __thread bool g_in_aligned_alloc = false;
static __thread bool g_in_memalign = false;
static __thread bool g_in_calloc = false;

/* The types of the variables holding the libc function pointers. */
typedef void   *(*type_libc_malloc)(size_t);
typedef void   *(*type_libc_calloc)(size_t, size_t);
typedef void   *(*type_libc_realloc)(void *, size_t);
typedef void    (*type_libc_free)(void *);
typedef size_t  (*type_libc_malloc_usable_size)(void *);
typedef int     (*type_libc_posix_memalign)(void **, size_t, size_t);
typedef void   *(*type_libc_aligned_alloc)(size_t, size_t);
typedef void   *(*type_libc_memalign)(size_t, size_t);

/* The (atomic) globals holding the pointer to the original libc implementation. */
_Atomic type_libc_malloc g_libc_malloc;
_Atomic type_libc_calloc g_libc_calloc;
_Atomic type_libc_realloc g_libc_realloc;
_Atomic type_libc_free g_libc_free;
_Atomic type_libc_malloc_usable_size g_libc_malloc_usable_size;
_Atomic type_libc_posix_memalign g_libc_posix_memalign;
_Atomic type_libc_aligned_alloc g_libc_aligned_alloc;
_Atomic type_libc_memalign g_libc_memalign;

// ---------------------------------------------------------------------------
// Counting model
//
// Counters live in a per-thread block (TLS). The hot path is a plain pointer
// load + a handful of non-atomic stores — no atomic round-trips, no
// cross-thread cache-line ping-pong. Reads aggregate across the linked list
// of live thread blocks plus an accumulator for blocks belonging to threads
// that have already exited.
//
// g_counting_enabled stays atomic so enable()/disable() from one thread is
// immediately visible to others, but per-thread counts are owned by the
// writing thread.
//
// On Linux _Thread_local resolves to ELF TLS with the initial-exec model
// once the dylib is preloaded, so `t_block` lookup is a single TPIDR-relative
// load — much cheaper than macOS's `_tlv_get_addr` thunk.
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

static __thread counter_block_t *t_block = NULL;
// Set while this thread is creating its counter block; if block creation itself
// allocates, the nested count would otherwise recurse into tls_block_init and
// deadlock on g_list_mutex.
static __thread bool t_initializing = false;
static pthread_key_t g_block_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

// Page size — pre-seeded with a sane default for the build target so the
// value is valid even before our constructor runs (the dynamic linker can
// dispatch early calls through our LD_PRELOAD entry points). The constructor
// refreshes it just in case the runtime page size differs.
#if defined(__aarch64__) || defined(__arm64__)
static size_t g_page_size = 4096;
#else
static size_t g_page_size = 4096;
#endif

__attribute__((constructor)) static void init_page_size(void) {
    g_page_size = (size_t)getpagesize();
}

// Forward declarations — the libc-resolving macros below reference these.
static void *recursive_malloc(size_t size);
static void recursive_free(void *ptr);
static bool is_recursive_malloc_block(void *ptr);

// Helper that calls libc's real malloc via the resolved function pointer,
// avoiding recursion into our own replacement. Returns NULL on failure.
static void *libc_calloc_block(size_t size) {
    type_libc_malloc local_malloc = atomic_load(&g_libc_malloc);
    if (!local_malloc) {
        // Resolution still pending; fall back to the recursive-malloc backing
        // store so tls init doesn't deadlock during dlsym.
        return recursive_malloc(size);
    }
    void *p = local_malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static void libc_free_block(void *ptr) {
    if (!ptr) return;
    // Blocks vended from the recursive-malloc BSS backing store don't belong
    // to libc; freeing them would corrupt libc's heap.
    if (is_recursive_malloc_block(ptr)) return;
    type_libc_free local_free = atomic_load(&g_libc_free);
    if (!local_free) {
        // Should never happen post-init; just leak rather than abort.
        return;
    }
    local_free(ptr);
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

    libc_free_block(b);
}

static void init_block_key(void) {
    pthread_key_create(&g_block_key, block_destructor);
}

static __attribute__((noinline)) counter_block_t *tls_block_init(void) {
    t_initializing = true;
    pthread_once(&g_key_once, init_block_key);

    counter_block_t *b = (counter_block_t *)libc_calloc_block(sizeof(counter_block_t));
    if (b) {
        pthread_setspecific(g_block_key, b);

        pthread_mutex_lock(&g_list_mutex);
        b->next = g_blocks_head;
        g_blocks_head = b;
        pthread_mutex_unlock(&g_list_mutex);

        t_block = b;
    }
    t_initializing = false;
    return b;
}

static __attribute__((always_inline)) counter_block_t *get_tls_block(void) {
    counter_block_t *b = t_block;
    if (__builtin_expect(b == NULL, 0)) {
        // A reentrant allocation while we're building the block must not recurse
        // back in (that would deadlock on g_list_mutex); skip counting it.
        if (t_initializing) return NULL;
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
        __atomic_store_n(&b->malloc_bytes,         0, __ATOMIC_RELAXED);
        __atomic_store_n(&b->malloc_size_class[0], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&b->malloc_size_class[1], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&b->free_count,           0, __ATOMIC_RELAXED);
        __atomic_store_n(&b->free_bytes,           0, __ATOMIC_RELAXED);
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
        mb += __atomic_load_n(&b->malloc_bytes,         __ATOMIC_RELAXED);
        ms += __atomic_load_n(&b->malloc_size_class[0], __ATOMIC_RELAXED);
        ml += __atomic_load_n(&b->malloc_size_class[1], __ATOMIC_RELAXED);
        fc += __atomic_load_n(&b->free_count,           __ATOMIC_RELAXED);
        fb += __atomic_load_n(&b->free_bytes,           __ATOMIC_RELAXED);
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

// this is called if malloc is called whilst trying to resolve libc's realloc.
// we just vend out pointers to a large block in the BSS (which we never free).
// This block should be large enough because it's only used when malloc is
// called from dlsym which should only happen once per thread.
static void *recursive_malloc(size_t size_in) {
    size_t size = size_in;
    if ((size & 0xf) != 0) {
        // make size 16 byte aligned
        size = (size + 0xf) & (~(size_t)0xf);
    }

    ptrdiff_t next = atomic_fetch_add_explicit(&g_recursive_malloc_next_free_ptr,
                                               size,
                                               memory_order_relaxed);
    if ((size_t)next >= sizeof(g_recursive_malloc_mem)) {
        // we ran out of memory
        return NULL;
    }
    return (void *)((intptr_t)g_recursive_malloc_mem + next);
}

static bool is_recursive_malloc_block(void *ptr) {
    uintptr_t block_begin = (uintptr_t)g_recursive_malloc_mem;
    uintptr_t block_end = block_begin + sizeof(g_recursive_malloc_mem);
    uintptr_t user_ptr = (uintptr_t)ptr;

    return user_ptr >= block_begin && user_ptr < block_end;
}

// this is called if realloc is called whilst trying to resolve libc's realloc.
static void *recursive_realloc(void *ptr, size_t size) {
    (void)ptr; (void)size;
    abort();
}

// this is called if free is called whilst trying to resolve libc's free.
static void recursive_free(void *ptr) {
    (void)ptr;
    abort();
}

// If malloc_usable_size is queried during dlsym handshake, we have nothing
// useful to report — return 0. Reaching here is exceptional.
static size_t recursive_malloc_usable_size(void *ptr) {
    (void)ptr;
    return 0;
}

// The aligned allocators are never needed to resolve libc symbols; if we
// somehow re-enter during the dlsym handshake, fail the allocation rather than
// recurse.
static int recursive_posix_memalign(void **memptr, size_t alignment, size_t size) {
    (void)memptr; (void)alignment; (void)size;
    return ENOMEM;
}
static void *recursive_aligned_alloc(size_t alignment, size_t size) {
    (void)alignment; (void)size;
    return NULL;
}
static void *recursive_memalign(size_t alignment, size_t size) {
    (void)alignment; (void)size;
    return NULL;
}
// calloc may be called during the dlsym handshake; the recursive bump allocator
// vends from zero-initialized BSS, so its blocks are already zeroed.
static void *recursive_calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) return NULL;
    return recursive_malloc(total);
}

#define JUMP_INTO_LIBC_FUN(_fun, ...) /* \
*/ do { /* \
*/     type_libc_ ## _fun local_fun = atomic_load(&g_libc_ ## _fun); /* \
*/     if (!local_fun) { /* \
*/         if (!g_in_ ## _fun) { /* \
*/             g_in_ ## _fun = true; /* \
*/             type_libc_ ## _fun desired = dlsym(RTLD_NEXT, LIBC_SYMBOL(_fun)); /* \
*/             if (atomic_compare_exchange_strong(&g_libc_ ## _fun, &local_fun, desired)) { /* \
*/                 local_fun = desired; /* \
*/             } else { /* \
*/                 local_fun = atomic_load(&g_libc_ ## _fun); /* \
*/              } /* \
*/         } else { /* \
*/             return recursive_ ## _fun (__VA_ARGS__); /* \
*/         } /* \
*/     } /* \
*/     return local_fun(__VA_ARGS__); /* \
*/ } while(0)

/* Companion to JUMP_INTO_LIBC_FUN that captures the libc result into _outvar
 * instead of returning. Used when we need to inspect the result before
 * returning (e.g. to write the size header). */
#define CALL_LIBC_FUN_CAPTURE(_outvar, _fun, ...) \
    do { \
        type_libc_ ## _fun local_fun = atomic_load(&g_libc_ ## _fun); \
        if (!local_fun) { \
            if (!g_in_ ## _fun) { \
                g_in_ ## _fun = true; \
                type_libc_ ## _fun desired = dlsym(RTLD_NEXT, LIBC_SYMBOL(_fun)); \
                if (atomic_compare_exchange_strong(&g_libc_ ## _fun, &local_fun, desired)) { \
                    local_fun = desired; \
                } else { \
                    local_fun = atomic_load(&g_libc_ ## _fun); \
                } \
            } else { \
                (_outvar) = recursive_ ## _fun (__VA_ARGS__); \
                break; \
            } \
        } \
        (_outvar) = local_fun(__VA_ARGS__); \
    } while (0)

// Inline counting helpers ---------------------------------------------------
//
// Byte basis: `size` is whatever each path supplies — requested bytes on the
// header-prefixed paths (malloc/calloc/realloc), and libc's usable size on the
// aligned/legacy paths (valloc/posix_memalign/aligned_alloc/memalign) which
// can't carry a header. A given allocation's alloc and free use the same basis,
// so the delta metrics stay correct; only the gross byte total can read
// slightly high for aligned allocations.
//
// All counter updates land in the calling thread's TLS block. The block is
// created lazily on first use. Once the pointer is cached in the
// thread-local slot, every subsequent call is a non-atomic increment on
// private memory.

static __attribute__((always_inline)) void count_malloc(size_t size) {
    counter_block_t *b = get_tls_block();
    if (__builtin_expect(b == NULL, 0)) return;
    b->malloc_bytes += (int64_t)size;
    // Branchless small/large split — index 0 is small, 1 is large. The boundary
    // is a fixed constant (not the page size) so the split is architecture-
    // independent; see MALLOC_INTERPOSER_LARGE_THRESHOLD.
    b->malloc_size_class[size > MALLOC_INTERPOSER_LARGE_THRESHOLD]++;
}

static __attribute__((always_inline)) void count_free(size_t size) {
    counter_block_t *b = get_tls_block();
    if (__builtin_expect(b == NULL, 0)) return;
    b->free_count++;
    b->free_bytes += (int64_t)size;
}

// Header-write helper -------------------------------------------------------

static __attribute__((always_inline)) void *write_header(void *raw, size_t size) {
    malloc_header_t *hdr = (malloc_header_t *)raw;
    void *user = malloc_interposer_user_for(raw);
    hdr->requested_size = size;
    hdr->addr_tag = malloc_interposer_addr_tag(user);
    hdr->magic = MALLOC_INTERPOSER_MAGIC;
    return user;
}

// Replacement functions -----------------------------------------------------

__attribute__((flatten)) void *replacement_malloc(size_t size) {
    void *raw;
    CALL_LIBC_FUN_CAPTURE(raw, malloc, size + sizeof(malloc_header_t));
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
        // Recursive-malloc blocks live in our static buffer; never call libc free on them.
        if (!is_recursive_malloc_block(hdr)) {
            JUMP_INTO_LIBC_FUN(free, hdr);
        }
        return;
    }
    // Externally-allocated pointer (no header).
    if (is_recursive_malloc_block(user_ptr)) return;
    if (atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        size_t size;
        CALL_LIBC_FUN_CAPTURE(size, malloc_usable_size, user_ptr);
        count_free(size);
    }
    JUMP_INTO_LIBC_FUN(free, user_ptr);
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

        void *new_raw;
        CALL_LIBC_FUN_CAPTURE(new_raw, realloc, old_hdr, new_size + sizeof(malloc_header_t));
        if (!new_raw) return NULL;

        if (counting) {
            count_free(old_size);
            count_malloc(new_size);
        }
        return write_header(new_raw, new_size);
    }

    // External pointer; use libc bookkeeping. Route every malloc_usable_size
    // call through CALL_LIBC_FUN_CAPTURE so we hit libc, not our override.
    size_t old_size;
    CALL_LIBC_FUN_CAPTURE(old_size, malloc_usable_size, user_ptr);
    void *new_ptr;
    CALL_LIBC_FUN_CAPTURE(new_ptr, realloc, user_ptr, new_size);
    if (!new_ptr) return NULL;
    if (counting) {
        count_free(old_size);
        size_t new_usable;
        CALL_LIBC_FUN_CAPTURE(new_usable, malloc_usable_size, new_ptr);
        count_malloc(new_usable);
    }
    return new_ptr;
}

__attribute__((flatten)) void *replacement_calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    // Allocate the underlying block with libc calloc (not malloc+memset) so a
    // large allocation keeps libc's demand-zeroed pages instead of being eagerly
    // faulted in by memset; we then overwrite just the 16-byte header.
    void *raw;
    CALL_LIBC_FUN_CAPTURE(raw, calloc, 1, total + sizeof(malloc_header_t));
    if (!raw) return NULL;
    if (atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        count_malloc(total);
    }
    return write_header(raw, total);
}

void *replacement_reallocf(void *user_ptr, size_t new_size) {
    void *new_ptr = replacement_realloc(user_ptr, new_size);
    if (!new_ptr && user_ptr && new_size != 0) {
        replacement_free(user_ptr);
    }
    return new_ptr;
}

// Aligned/legacy paths can't carry our size header: the 16-byte prefix would
// shift the user pointer off the requested alignment. We let libc place a
// correctly-aligned (header-less) chunk and account it via malloc_usable_size;
// the magic probe on free fails for these, routing them through the external
// path (which also bills them via malloc_usable_size, so alloc/free balance).

int replacement_posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (!memptr) return EINVAL;
    int result;
    CALL_LIBC_FUN_CAPTURE(result, posix_memalign, memptr, alignment, size);
    if (result == 0 && *memptr
        && atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        size_t usable;
        CALL_LIBC_FUN_CAPTURE(usable, malloc_usable_size, *memptr);
        count_malloc(usable);
    }
    return result;
}

void *replacement_valloc(size_t size) {
    void *ptr = NULL;
    if (replacement_posix_memalign(&ptr, (size_t)g_page_size, size) != 0) {
        return NULL;
    }
    return ptr;
}

void *replacement_aligned_alloc(size_t alignment, size_t size) {
    void *ptr;
    CALL_LIBC_FUN_CAPTURE(ptr, aligned_alloc, alignment, size);
    if (ptr && atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        size_t usable;
        CALL_LIBC_FUN_CAPTURE(usable, malloc_usable_size, ptr);
        count_malloc(usable);
    }
    return ptr;
}

void *replacement_memalign(size_t alignment, size_t size) {
    void *ptr;
    CALL_LIBC_FUN_CAPTURE(ptr, memalign, alignment, size);
    if (ptr && atomic_load_explicit(&g_counting_enabled, memory_order_relaxed)) {
        size_t usable;
        CALL_LIBC_FUN_CAPTURE(usable, malloc_usable_size, ptr);
        count_malloc(usable);
    }
    return ptr;
}

// Size queries --------------------------------------------------------------
//
// External callers may pass our pointers to malloc_usable_size; libc would
// see an offset address and return garbage from its chunk-header probe.
// Override and route ours through the header. Internal calls go via
// CALL_LIBC_FUN_CAPTURE (dlsym-cached), bypassing our override.

size_t replacement_malloc_usable_size(void *user_ptr) {
    if (!user_ptr) return 0;
    if (malloc_interposer_is_ours(user_ptr)) {
        malloc_header_t *hdr = malloc_interposer_header_for(user_ptr);
        // Report the usable capacity the caller really has (libc's usable size
        // of the underlying block, minus our header), not just the requested
        // size — otherwise in-place growers like Swift Array/ManagedBuffer
        // never see the spare room and reallocate sooner than they would
        // natively, perturbing the allocation pattern being measured. Never
        // report less than was requested.
        size_t raw_usable;
        CALL_LIBC_FUN_CAPTURE(raw_usable, malloc_usable_size, hdr);
        size_t user_usable = raw_usable > sizeof(malloc_header_t)
                                 ? raw_usable - sizeof(malloc_header_t)
                                 : 0;
        return user_usable > hdr->requested_size ? user_usable : hdr->requested_size;
    }
    size_t size;
    CALL_LIBC_FUN_CAPTURE(size, malloc_usable_size, user_ptr);
    return size;
}

// Public symbol overrides ---------------------------------------------------

#if !MALLOC_INTERPOSER_SANITIZER
void free(void *ptr) { replacement_free(ptr); }
void *malloc(size_t size) { return replacement_malloc(size); }
void *calloc(size_t nmemb, size_t size) { return replacement_calloc(nmemb, size); }
void *realloc(void *ptr, size_t size) { return replacement_realloc(ptr, size); }
void *reallocf(void *ptr, size_t size) { return replacement_reallocf(ptr, size); }
void *valloc(size_t size) { return replacement_valloc(size); }
int posix_memalign(void **memptr, size_t alignment, size_t size) {
    return replacement_posix_memalign(memptr, alignment, size);
}
void *aligned_alloc(size_t alignment, size_t size) { return replacement_aligned_alloc(alignment, size); }
void *memalign(size_t alignment, size_t size) { return replacement_memalign(alignment, size); }
size_t malloc_usable_size(void *ptr) { return replacement_malloc_usable_size(ptr); }
#endif

#endif
