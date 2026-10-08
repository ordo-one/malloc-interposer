//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

#include "AllocationHookTestSupport.h"

// Darwin lazily allocates _Thread_local storage with malloc on a thread's first
// access, which would re-enter the hook before its guard is set, so use a
// pthread key there (pthread_getspecific/setspecific use static slots). On
// Linux __thread in the executable is static TLS and never allocates.
#if __APPLE__
#include <pthread.h>

static pthread_key_t g_state_key;
static pthread_once_t g_state_once = PTHREAD_ONCE_INIT;

static void create_state_key(void) {
    pthread_key_create(&g_state_key, NULL);
}

void allocation_hook_state_initialize(void) {
    pthread_once(&g_state_once, create_state_key);
}

uintptr_t allocation_hook_state_get(void) {
    return (uintptr_t)pthread_getspecific(g_state_key);
}

void allocation_hook_state_set(uintptr_t state) {
    pthread_setspecific(g_state_key, (const void *)state);
}
#else
static __thread uintptr_t t_state = 0;

void allocation_hook_state_initialize(void) {}

uintptr_t allocation_hook_state_get(void) {
    return t_state;
}

void allocation_hook_state_set(uintptr_t state) {
    t_state = state;
}
#endif
