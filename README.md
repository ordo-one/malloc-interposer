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

When counting is enabled the interposer increments six 64-bit atomic
counters per call:

| counter | meaning |
| --- | --- |
| `malloc_count` | total allocation calls |
| `malloc_bytes` | total requested bytes allocated |
| `malloc_small` | allocations with requested size ≤ page size |
| `malloc_large` | allocations with requested size  > page size |
| `free_count`   | total `free` calls |
| `free_bytes`   | total bytes freed |

Counting is toggled at runtime — bracket the region you want to measure
with `hook()` / `unhook()` and read the totals with `getStatistics()`.

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
library — `libMallocInterposerSwift.dylib` / `.so`. The C interposer
keeps its counters in `_Atomic int64_t` globals; for the Swift API's
read to see the writes performed by the interposed `malloc`/`free`, the
two must refer to the same memory, i.e. live in the same image. Splitting
the C interposer into its own SwiftPM product would cause the C target
to be statically embedded into the Swift dylib, producing two disconnected
copies of the counters. Keeping everything in one library avoids that.

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

All counters are `_Atomic int64_t` updated with `memory_order_relaxed`,
so the hot path per counted call is one branch on the enabled flag plus
a handful of atomic adds. The Swift wrapper is purely a façade — no
dispatch happens between user code and the C symbols.

## Requirements

- Swift 5.10+
- macOS 13+ or Linux

## License

Apache 2.0 — see [`LICENSE`](LICENSE).
