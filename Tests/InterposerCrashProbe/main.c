//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

// Standalone crash probe for the interposer's pointer classifier, driven by
// the AlignedPointerSafety death tests. It runs in its *own* minimal process
// on purpose: only in a sparsely-mapped address space does libc reliably place
// a large allocation against an unmapped guard page, which is the condition
// that makes `malloc_interposer_is_ours` read unmapped memory. The parent test
// process is too populated for that to reproduce in-process.
//
//   argv[1] == "free" -> exercise replacement_free (the production free path)
//   argv[1] == "tagged" -> exercise malloc_interposer_is_ours with a synthetic
//                           Objective-C tagged pointer
//   otherwise         -> exercise malloc_interposer_is_ours directly
//
// Exit codes:
//   0 : the classifier ran without faulting (and classified the external
//       allocation as "not ours")
//   1 : the classifier misclassified an external pointer as ours
//   3 : allocation failed (inconclusive)
// A SIGSEGV/SIGBUS termination means the classifier dereferenced an
// unmapped/invalid address — the bug under test.

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <interposer.h>

int main(int argc, char **argv) {
    size_t pageSize = (size_t)getpagesize();
    int freePath = (argc > 1 && strcmp(argv[1], "free") == 0);
    int taggedPath = (argc > 1 && strcmp(argv[1], "tagged") == 0);

    if (taggedPath) {
        // Set both TAG_MASK bits used by objc4: bit 0 on x86_64 and bit 63 on
        // arm64. This keeps the synthetic probe platform-independent.
        const uintptr_t taggedPointerBits = ((uintptr_t)1 << 63) | (uintptr_t)1;
        const void *taggedPointer = (const void *)taggedPointerBits;
        return replacement_malloc_size(taggedPointer) ? 1 : 0;
    }

    // A few large sizes so at least one lands in its own fresh mmap with an
    // unmapped preceding page.
    const size_t sizes[] = { 64UL << 20, 128UL << 20, 256UL << 20 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void *ptr = NULL;
        if (posix_memalign(&ptr, pageSize, sizes[i]) != 0 || ptr == NULL) {
            return 3;
        }
        if (freePath) {
            replacement_free(ptr); // classifies via is_ours; faults on the guard page
        } else {
            if (malloc_interposer_is_ours(ptr)) {
                free(ptr);
                return 1;
            }
            free(ptr);
        }
    }
    return 0;
}
