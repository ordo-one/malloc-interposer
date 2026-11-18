//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Probe for allocations made during thread teardown, after the interposer's
// own TSD destructor has freed the thread's counter block. Driven by
// ThreadTeardownTests, which runs it with the interposer dylib preloaded
// (DYLD_INSERT_LIBRARIES), as package-benchmark loads it. The bug does not
// reproduce with the C target linked statically, because then nothing else in
// the process is interposed.
//
// It deliberately links nothing from this package: the counter API is looked up
// in the preloaded dylib, so there is a single copy of the counters.
//
// Each thread makes one counted allocation, then exits; a TSD destructor that
// runs after the interposer's frees and allocates `late` more times. Before the
// fix those writes went into the freed block: counts were lost and the heap was
// corrupted (SIGTRAP "memory corruption of free block", SIGSEGV or SIGBUS).
//
// Exit codes: 0 counts complete; 1 late allocations missing from the counts;
// 2 interposer not preloaded. Termination by a signal is the crash.

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (*void_fn)(void);
typedef void (*stats_fn)(int64_t *, int64_t *, int64_t *, int64_t *, int64_t *, int64_t *);

static pthread_key_t g_late_key;
static int g_late = 100;

static void late_destructor(void *value) {
    (void)value;
    for (int i = 0; i < g_late; i++) {
        void *volatile p = malloc(32);
        free(p);
    }
}

static void *worker(void *arg) {
    (void)arg;
    void *volatile p = malloc(64); // creates this thread's counter block
    free(p);
    pthread_setspecific(g_late_key, (void *)1);
    return NULL;
}

int main(int argc, char **argv) {
    int threads = argc > 1 ? atoi(argv[1]) : 1;
    g_late = argc > 2 ? atoi(argv[2]) : 100;

    void_fn enable = (void_fn)dlsym(RTLD_DEFAULT, "malloc_interposer_enable");
    void_fn disable = (void_fn)dlsym(RTLD_DEFAULT, "malloc_interposer_disable");
    void_fn reset = (void_fn)dlsym(RTLD_DEFAULT, "malloc_interposer_reset");
    stats_fn stats = (stats_fn)dlsym(RTLD_DEFAULT, "malloc_interposer_get_stats");
    if (!enable || !disable || !reset || !stats) {
        fprintf(stderr, "malloc interposer not preloaded\n");
        return 2;
    }

    enable();
    // The first counted allocation creates the interposer's TSD key; ours is
    // created after it, so its destructor runs after the interposer's.
    void *volatile warm = malloc(16);
    free(warm);
    pthread_key_create(&g_late_key, late_destructor);
    reset();

    for (int t = 0; t < threads; t++) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, worker, NULL) != 0) return 3;
        pthread_join(thread, NULL);
    }
    disable();

    int64_t mc, mb, ms, ml, fc, fb;
    stats(&mc, &mb, &ms, &ml, &fc, &fb);
    long long expected = (long long)threads * (1 + g_late);
    printf("threads=%d late=%d counted mallocs=%lld frees=%lld expected>=%lld\n",
           threads, g_late, (long long)mc, (long long)fc, expected);
    return (mc >= expected && fc >= expected) ? 0 : 1;
}
