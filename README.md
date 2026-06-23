# malloc-interposer

A small, low-overhead malloc/free interposer for macOS and Linux that
counts allocations and bytes for the host process and exposes the
counters through a thin Swift wrapper (and a plain C header for
non-Swift consumers).

It was extracted from
[ordo-one/package-benchmark](https://github.com/ordo-one/package-benchmark),
where it backs the framework's `mallocCountSmall`, `mallocCountLarge`,
`mallocBytesCount`, and related metrics. It is published separately so
other Swift projects can depend on it directly without pulling in the
full benchmark stack.

## What it does

Every call to `malloc`, `calloc`, `realloc`, `reallocf`, `free`,
`posix_memalign`, `valloc` (and the macOS `malloc_zone_*` variants) is
routed through the interposer:

- on **macOS** via `DYLD_INTERPOSE` in `libMallocInterposerSwift.dylib`,
- on **Linux** by `LD_PRELOAD`ing `libMallocInterposerSwift.so`, which
  defines the symbols directly and resolves the real libc entries
  through `dlsym(RTLD_NEXT, …)`.

When counting is enabled the interposer updates a small bundle of
per-thread counters on every intercepted call. `getStatistics()` returns
six fields:

| field | meaning |
| --- | --- |
| `mallocCount`      | total allocation calls (= small + large) |
| `mallocBytesCount` | total requested bytes allocated |
| `mallocSmallCount` | allocations with requested size ≤ page size |
| `mallocLargeCount` | allocations with requested size  > page size |
| `freeCount`        | total `free` calls |
| `freeBytesCount`   | total bytes freed |

Counting is toggled at runtime — bracket the region you want to measure
with `hook()` / `unhook()` and read the totals with `getStatistics()`.
Snapshots are best-effort: under concurrent allocation traffic the six
fields are not guaranteed to be mutually consistent. Bracketing with
`hook()` / `unhook()` around a paused workload gives you a clean read.

## Header-prefix size tracking

Every allocation the interposer hands out is prefixed with a 16-byte
header that records the requested size and a magic word. On `free` /
`realloc` the size comes from the header instead of `malloc_size` /
`malloc_usable_size`, saving a libc round-trip per call. The 16-byte
size preserves the libc 16-byte alignment guarantee for the user
pointer. Pointers that didn't go through the interposer (rare —
typically allocations from before the dylib was loaded, or from
alignment-sensitive paths like `posix_memalign` that bypass the header)
are detected by a failing magic check and fall back to libc bookkeeping.

## Why a single combined dylib

The interposer and the Swift wrapper ship in a single dynamic
library — `libMallocInterposerSwift.dylib` / `.so`. The C interposer's
shared state — the linked list of per-thread counter blocks, the mutex
that guards it, the dead-thread aggregate, and the `pthread_key_t`
destructor — must all live in one image so the Swift API's read sees
the writes performed by the interposed `malloc`/`free`. Splitting the
C target into its own SwiftPM product would cause it to be statically
embedded into the Swift dylib, producing two disconnected copies of all
that state. Keeping everything in one library avoids that.

## Using it from Swift

Add the package as a dependency:

```swift
.package(url: "https://github.com/ordo-one/malloc-interposer.git", from: "1.0.0")
```

…and depend on `MallocInterposerSwift` from your target:

```swift
.target(
    name: "MyTarget",
    dependencies: [
        .product(name: "MallocInterposerSwift", package: "malloc-interposer"),
    ]
)
```

Then in your code:

```swift
import MallocInterposerSwift

MallocInterposerSwift.initialize()
MallocInterposerSwift.hook()

// ... code you want to measure ...

MallocInterposerSwift.unhook()
let stats = MallocInterposerSwift.getStatistics()
print("mallocs: \(stats.mallocCount), bytes: \(stats.mallocBytesCount)")
print("small: \(stats.mallocSmallCount), large: \(stats.mallocLargeCount)")
print("frees:  \(stats.freeCount), bytes: \(stats.freeBytesCount)")
```

The full DocC reference for the Swift surface lives on the
`MallocInterposerSwift` class and its `Statistics` struct.

## Loading the dylib

Linking against `MallocInterposerSwift` makes the API available, but the
dylib must actually be injected into the process for interposition to
take effect. SwiftPM produces `libMallocInterposerSwift.dylib` (macOS) /
`libMallocInterposerSwift.so` (Linux) under `.build/<config>/`.

### macOS

```sh
DYLD_INSERT_LIBRARIES=.build/release/libMallocInterposerSwift.dylib \
    .build/release/MyExecutable
```

`DYLD_INSERT_LIBRARIES` is stripped from `posix_spawn` calls into
system-protected binaries (SIP), so this works for your own binaries
but not, e.g., `/usr/bin/...` targets.

### Linux

```sh
LD_PRELOAD=.build/release/libMallocInterposerSwift.so \
    .build/release/MyExecutable
```

On Linux, defining `malloc` / `free` in a preloaded shared object
overrides them globally for the process via standard ELF symbol
resolution.

## Using it from C

The C interposer header `interposer.h` is also exposed as part of the
package. Pure C consumers can call the same C API directly:

```c
#include <interposer.h>

malloc_interposer_reset();
malloc_interposer_enable();

// ... allocation traffic ...

malloc_interposer_disable();

int64_t mallocs, bytes, small, large, frees, freed;
malloc_interposer_get_stats(&mallocs, &bytes, &small, &large,
                            &frees, &freed);
```

The same dylib applies — preload `libMallocInterposerSwift.dylib` /
`libMallocInterposerSwift.so` to enable interposition, and link against
it for the public symbols.

## Performance

The hot path per counted call is one relaxed load of the enabled flag,
one thread-local pointer load, and a handful of plain (non-atomic)
stores into the calling thread's counter block. Each thread allocates
its own block on first use and registers it with a `pthread_key_t`
destructor, so thread exit folds the counts into a global aggregate
rather than losing them. `getStatistics()` walks the live thread blocks
under a mutex and sums in the dead-thread aggregate, so the read side
is more expensive than the write side — call it outside the measured
region.

Avoiding global atomics on the hot path is most visible on glibc Linux,
where `__thread` access compiles to a single TPIDR-relative load. On
macOS `_Thread_local` still goes through `_tlv_get_addr` and the win
over relaxed LSE atomics is smaller (a few ns/call), but the design
still scales cleanly under multi-threaded contention because each
writer thread touches only its own cache line.

The Swift wrapper is purely a façade — no dispatch happens between
user code and the C symbols.

## Requirements

- Swift 5.10+
- macOS 13+ or Linux

## Acknowledgments

The libc interposition technique this package is built on — `DYLD_INTERPOSE`
on macOS and `dlsym(RTLD_NEXT, …)` symbol resolution (with the
recursive-malloc-during-`dlsym` workaround) on Linux — is derived from the
[SwiftNIO](https://github.com/apple/swift-nio) project's allocation-counter
test framework, by Apple Inc. and the SwiftNIO project authors, used under the
Apache 2.0 license. The per-thread counting model and the header-prefix size
tracking layered on top are original to this package. See [`NOTICE`](NOTICE)
for the full attribution.

## License

Apache 2.0 — see [`LICENSE`](LICENSE).
