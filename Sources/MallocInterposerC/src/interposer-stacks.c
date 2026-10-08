//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

// ---------------------------------------------------------------------------
// Allocation call-stack capture
//
// When enabled (in addition to counting), every counted allocation walks the
// caller's frame-pointer chain and aggregates the raw return addresses into
// a global open-addressing hash table keyed by the stack's contents. The
// capture path is allocation-free: the frame buffer is stack-local, the
// table is mmap'd, and the side index of occupied slots lives in BSS;
// nothing on this path can recurse into the interposed malloc.
//
// The frame record layout {previous_fp, return_address} is identical on
// x86_64 and arm64 (both Darwin and SysV ABIs), so one implementation serves
// every supported platform. Builds that omit frame pointers produce
// truncated (or empty) stacks; those samples are dropped and counted in
// g_stacks_dropped rather than crashing the walk.
//
// A single global lock-free table is used instead of the per-thread blocks
// the plain counters use: memory is bounded once (not per thread), no merge
// or dead-thread handoff is needed, and the walk itself dominates the cost
// of the two relaxed fetch_adds, so the TLS design's rationale (keeping the
// always-on metric path atomic-free) does not apply to this opt-in
// diagnostic mode.
// ---------------------------------------------------------------------------

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include <interposer.h>
#include "interposer-stacks-internal.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

#define MI_STACK_MAX_DEPTH 64
#define MI_STACK_TABLE_CAPACITY (1u << 16) // power of two
#define MI_STACK_MAX_PROBES 4096
// Frame pointers must ascend by less than this between frames to be
// considered part of the same stack.
#define MI_STACK_MAX_FRAME_STRIDE ((uintptr_t)16 << 20)

typedef struct {
    _Atomic uint32_t state; // 0 = empty, 1 = claimed (being written), 2 = ready
    uint32_t depth;
    uint64_t hash;
    _Atomic uint64_t count; // running total since reset
    _Atomic uint64_t bytes; // sum of requested sizes, running total
    // Measurement-window bookkeeping (see malloc_interposer_stacks_mark /
    // _commit): the running totals at the last mark, and the totals of all
    // windows committed so far. Only touched by the control-plane thread.
    uint64_t mark_count;
    uint64_t mark_bytes;
    uint64_t committed_count;
    uint64_t committed_bytes;
    void *frames[MI_STACK_MAX_DEPTH]; // leaf-first raw return addresses
} mi_stack_entry_t;

_Atomic bool malloc_interposer_stack_capture_enabled = false;

// Capacity is fixed at MI_STACK_TABLE_CAPACITY except when shrunk by the
// testing-only setter before the table is first mapped.
static uint32_t g_table_capacity = MI_STACK_TABLE_CAPACITY;
static mi_stack_entry_t *g_stack_table = NULL; // mmap'd on first enable

// Append-only index of occupied slots so iterate/reset only touch pages that
// actually hold entries (the table's virtual reservation is ~34 MiB; resident
// cost stays proportional to the number of unique stacks).
static uint32_t g_used_slots[MI_STACK_TABLE_CAPACITY];
static _Atomic uint32_t g_used_count = 0;
static _Atomic uint64_t g_stacks_dropped = 0;

// FNV-1a over the frame array.
static uint64_t mi_stack_hash(void *const *frames, uint32_t depth) {
    const unsigned char *bytes = (const unsigned char *)frames;
    size_t len = (size_t)depth * sizeof(void *);
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

void malloc_interposer_record_alloc_stack(size_t size) {
    void *frames[MI_STACK_MAX_DEPTH];
    uint32_t depth = 0;

    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t prev = 0;
    while (depth < MI_STACK_MAX_DEPTH) {
        // Frame records are 16-byte aligned on both supported ABIs; a null,
        // unaligned, descending, or wildly-striding FP means we've walked off
        // the end of the chain (or into an omit-frame-pointer frame).
        if (fp == 0 || (fp & 0xF) != 0) break;
        if (prev != 0 && (fp <= prev || fp - prev > MI_STACK_MAX_FRAME_STRIDE)) break;
        void *const *record = (void *const *)fp;
        void *return_address = record[1];
        if (return_address == NULL) break;
#if __has_feature(ptrauth_calls)
        return_address = ptrauth_strip(return_address, ptrauth_key_return_address);
#endif
        frames[depth++] = return_address;
        prev = fp;
        fp = (uintptr_t)record[0];
    }

    mi_stack_entry_t *table = g_stack_table;
    if (depth == 0 || table == NULL) {
        atomic_fetch_add_explicit(&g_stacks_dropped, 1, memory_order_relaxed);
        return;
    }

    uint64_t hash = mi_stack_hash(frames, depth);
    uint32_t mask = g_table_capacity - 1;
    uint32_t idx = (uint32_t)hash & mask;
    uint32_t max_probes = g_table_capacity < MI_STACK_MAX_PROBES ? g_table_capacity : MI_STACK_MAX_PROBES;

    for (uint32_t probe = 0; probe < max_probes; probe++, idx = (idx + 1) & mask) {
        mi_stack_entry_t *entry = &table[idx];
        uint32_t state = atomic_load_explicit(&entry->state, memory_order_acquire);

        if (state == 0) {
            uint32_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(&entry->state, &expected, 1,
                                                        memory_order_acq_rel,
                                                        memory_order_acquire)) {
                entry->hash = hash;
                entry->depth = depth;
                memcpy(entry->frames, frames, (size_t)depth * sizeof(void *));
                atomic_store_explicit(&entry->count, 1, memory_order_relaxed);
                atomic_store_explicit(&entry->bytes, size, memory_order_relaxed);
                uint32_t used = atomic_fetch_add_explicit(&g_used_count, 1, memory_order_relaxed);
                g_used_slots[used] = idx;
                atomic_store_explicit(&entry->state, 2, memory_order_release);
                return;
            }
            state = expected; // CAS lost; fall through with the observed state
        }

        if (state == 1) {
            // Another thread is writing this slot. Spin briefly for it to
            // become ready; if it doesn't, keep probing (worst case we insert
            // a duplicate entry for the same stack in a later slot; the
            // consumer aggregates by frame content, so that's benign).
            int spin = 1024;
            while (spin-- > 0 && atomic_load_explicit(&entry->state, memory_order_acquire) == 1) {}
            if (atomic_load_explicit(&entry->state, memory_order_acquire) != 2) continue;
        }

        if (entry->hash == hash && entry->depth == depth
            && memcmp(entry->frames, frames, (size_t)depth * sizeof(void *)) == 0) {
            atomic_fetch_add_explicit(&entry->count, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&entry->bytes, size, memory_order_relaxed);
            return;
        }
    }

    atomic_fetch_add_explicit(&g_stacks_dropped, 1, memory_order_relaxed);
}

// What an entry reports: everything committed from closed windows plus the
// delta of the window currently open (or, if mark/commit were never used,
// simply the running total).
static inline uint64_t mi_reported_count(const mi_stack_entry_t *entry) {
    return entry->committed_count
           + (atomic_load_explicit(&entry->count, memory_order_relaxed) - entry->mark_count);
}

static inline uint64_t mi_reported_bytes(const mi_stack_entry_t *entry) {
    return entry->committed_bytes
           + (atomic_load_explicit(&entry->bytes, memory_order_relaxed) - entry->mark_bytes);
}

// Public API ----------------------------------------------------------------

void malloc_interposer_stacks_enable(void) {
    if (g_stack_table == NULL) {
        void *mem = mmap(NULL, (size_t)g_table_capacity * sizeof(mi_stack_entry_t),
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return; // capture stays disabled; samples would be dropped anyway
        g_stack_table = (mi_stack_entry_t *)mem;
    }
    atomic_store_explicit(&malloc_interposer_stack_capture_enabled, true, memory_order_release);
}

void malloc_interposer_stacks_disable(void) {
    atomic_store_explicit(&malloc_interposer_stack_capture_enabled, false, memory_order_release);
}

void malloc_interposer_stacks_reset(void) {
    if (g_stack_table == NULL) {
        atomic_store_explicit(&g_stacks_dropped, 0, memory_order_relaxed);
        return;
    }
    // Caller must have capture disabled; clear only the occupied slots so the
    // resident footprint stays proportional to unique stacks seen so far.
    uint32_t used = atomic_load_explicit(&g_used_count, memory_order_acquire);
    for (uint32_t i = 0; i < used; i++) {
        memset(&g_stack_table[g_used_slots[i]], 0, sizeof(mi_stack_entry_t));
    }
    atomic_store_explicit(&g_used_count, 0, memory_order_release);
    atomic_store_explicit(&g_stacks_dropped, 0, memory_order_relaxed);
}

size_t malloc_interposer_stacks_count(void) {
    if (g_stack_table == NULL) return 0;
    uint32_t used = atomic_load_explicit(&g_used_count, memory_order_acquire);
    size_t count = 0;
    for (uint32_t i = 0; i < used; i++) {
        mi_stack_entry_t *entry = &g_stack_table[g_used_slots[i]];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) != 2) continue;
        if (mi_reported_count(entry) == 0) continue;
        count++;
    }
    return count;
}

size_t malloc_interposer_stacks_iterate(malloc_interposer_stack_visitor_t visitor, void *context) {
    if (g_stack_table == NULL || visitor == NULL) return 0;
    uint32_t used = atomic_load_explicit(&g_used_count, memory_order_acquire);
    size_t visited = 0;
    for (uint32_t i = 0; i < used; i++) {
        mi_stack_entry_t *entry = &g_stack_table[g_used_slots[i]];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) != 2) continue;
        uint64_t count = mi_reported_count(entry);
        if (count == 0) continue; // everything it recorded was discarded by a re-mark
        malloc_interposer_stack_t stack = {
            .frames = (const void *const *)entry->frames,
            .depth = entry->depth,
            .count = count,
            .bytes = mi_reported_bytes(entry),
        };
        visitor(&stack, context);
        visited++;
    }
    return visited;
}

void malloc_interposer_stacks_mark(void) {
    if (g_stack_table == NULL) return;
    uint32_t used = atomic_load_explicit(&g_used_count, memory_order_acquire);
    for (uint32_t i = 0; i < used; i++) {
        mi_stack_entry_t *entry = &g_stack_table[g_used_slots[i]];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) != 2) continue;
        entry->mark_count = atomic_load_explicit(&entry->count, memory_order_relaxed);
        entry->mark_bytes = atomic_load_explicit(&entry->bytes, memory_order_relaxed);
    }
}

void malloc_interposer_stacks_commit(void) {
    if (g_stack_table == NULL) return;
    uint32_t used = atomic_load_explicit(&g_used_count, memory_order_acquire);
    for (uint32_t i = 0; i < used; i++) {
        mi_stack_entry_t *entry = &g_stack_table[g_used_slots[i]];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) != 2) continue;
        uint64_t count = atomic_load_explicit(&entry->count, memory_order_relaxed);
        uint64_t bytes = atomic_load_explicit(&entry->bytes, memory_order_relaxed);
        entry->committed_count += count - entry->mark_count;
        entry->committed_bytes += bytes - entry->mark_bytes;
        entry->mark_count = count;
        entry->mark_bytes = bytes;
    }
}

uint64_t malloc_interposer_stacks_dropped(void) {
    return atomic_load_explicit(&g_stacks_dropped, memory_order_relaxed);
}

void malloc_interposer_stacks_test_set_capacity(uint32_t capacity) {
    // Testing only: power-of-two, bounded by the compile-time maximum
    // (g_used_slots is statically sized to it). Requires capture disabled;
    // discards any captured stacks by remapping the table lazily.
    if (capacity == 0 || capacity > MI_STACK_TABLE_CAPACITY) return;
    if ((capacity & (capacity - 1)) != 0) return;
    if (atomic_load_explicit(&malloc_interposer_stack_capture_enabled, memory_order_acquire)) return;
    if (g_stack_table != NULL) {
        munmap(g_stack_table, (size_t)g_table_capacity * sizeof(mi_stack_entry_t));
        g_stack_table = NULL;
    }
    atomic_store_explicit(&g_used_count, 0, memory_order_release);
    atomic_store_explicit(&g_stacks_dropped, 0, memory_order_relaxed);
    g_table_capacity = capacity;
}
