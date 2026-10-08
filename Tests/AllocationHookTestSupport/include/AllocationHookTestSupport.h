//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ALLOCATION_HOOK_TEST_SUPPORT_H
#define ALLOCATION_HOOK_TEST_SUPPORT_H

#include <stdint.h>

// A per-thread word for allocation hooks: typically a recursion-guard bit plus
// a pointer to the thread's state. An allocation hook runs inside malloc, so
// accessing its thread-local must never allocate — otherwise it re-enters the
// hook before the guard is set. Call allocation_hook_state_initialize() before
// installing the hook.
void allocation_hook_state_initialize(void);
uintptr_t allocation_hook_state_get(void);
void allocation_hook_state_set(uintptr_t state);

#endif
