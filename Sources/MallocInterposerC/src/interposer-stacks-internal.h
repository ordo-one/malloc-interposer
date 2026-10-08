//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

// Internal interface between the platform interposer files and the shared
// allocation-stack capture core (interposer-stacks.c). Not installed as a
// public header.

#ifndef INTERPOSER_STACKS_INTERNAL_H
#define INTERPOSER_STACKS_INTERNAL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

// Read with relaxed ordering on the allocation hot path. Capture only
// happens while BOTH this flag and g_counting_enabled are set; the hook
// lives inside count_malloc, which is unreachable when counting is disabled.
extern _Atomic bool malloc_interposer_stack_capture_enabled;

// Walks the caller's frame-pointer chain and aggregates the stack into the
// global table. Allocation-free; safe to call from inside the interposed
// malloc path. noinline so the capture prologue itself stays out of the
// (flattened) replacement functions.
void malloc_interposer_record_alloc_stack(size_t size);

#endif
