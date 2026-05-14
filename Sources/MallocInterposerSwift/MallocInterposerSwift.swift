//
// Copyright (c) 2022 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

import MallocInterposerC

/// A Swift façade over the C malloc interposer.
///
/// `MallocInterposerSwift` exposes the small public surface of the C
/// interposer — enable/disable counting, reset counters, and read a snapshot
/// of the current statistics — as static methods so call sites don't have to
/// import or touch the C symbols directly.
///
/// ## Loading the interposer
///
/// Counting only happens when the interposer's dynamic library is actually
/// loaded into the process *before* `main`, so that its `malloc`/`free`
/// replacements are reachable from the loader-resolved PLT entries (Linux)
/// or via DYLD's `__interpose` section (macOS). Use the platform-appropriate
/// mechanism to inject `libMallocInterposerC.dylib` / `libMallocInterposerC.so`:
///
/// - **macOS:** `DYLD_INSERT_LIBRARIES=/path/to/libMallocInterposerC.dylib`
/// - **Linux:** `LD_PRELOAD=/path/to/libMallocInterposerC.so`
///
/// Linking against `MallocInterposerSwift` alone is not enough — the C
/// library must be present and loaded for interposition to take effect.
///
/// ## Typical use
///
/// ```swift
/// import MallocInterposerSwift
///
/// MallocInterposerSwift.initialize()
/// MallocInterposerSwift.hook()
///
/// // ... code whose allocations you want to measure ...
///
/// MallocInterposerSwift.unhook()
/// let stats = MallocInterposerSwift.getStatistics()
/// print("malloc count: \(stats.mallocCount), bytes: \(stats.mallocBytesCount)")
/// ```
///
/// ## Performance
///
/// All counters live in the C library as `_Atomic int64_t` globals updated
/// with relaxed memory order. There is no Swift dispatch on the malloc hot
/// path — every counted call costs one inline atomic add — so the Swift
/// wrapper is just a convenience over the same C symbols the C runtime is
/// already calling.
public class MallocInterposerSwift: @unchecked Sendable {
    private init() {}

    /// Resets all counters to zero without enabling counting.
    ///
    /// Call once before ``hook()`` to start from a known state. Safe to call
    /// repeatedly; it does not change whether counting is enabled.
    public static func initialize() {
        malloc_interposer_reset()
    }

    /// Resets counters to zero and enables counting.
    ///
    /// After this call returns, every `malloc`, `calloc`, `realloc`, `free`
    /// (and friends) routed through the interposer is reflected in the
    /// counters. Pair with ``unhook()`` to bracket a measured region.
    public static func hook() {
        malloc_interposer_reset()
        malloc_interposer_enable()
    }

    /// Disables counting.
    ///
    /// The current counter values are preserved and remain readable via
    /// ``getStatistics()``. Calls made after this point do not contribute
    /// to the totals.
    public static func unhook() {
        malloc_interposer_disable()
    }

    /// Resets all counters to zero while leaving the enabled/disabled state
    /// unchanged.
    ///
    /// Use this when you want to start a fresh measurement window without
    /// toggling the counting flag (e.g., between warmup and measured runs).
    public static func reset() {
        malloc_interposer_reset()
    }

    /// Reads a consistent snapshot of all counters.
    ///
    /// Each field is read with `memory_order_relaxed`, so the individual
    /// values are not guaranteed to be mutually consistent with each other
    /// under concurrent allocation traffic — they are best-effort
    /// instantaneous reads. Bracket measured regions with ``hook()`` /
    /// ``unhook()`` to avoid this.
    ///
    /// - Returns: A ``Statistics`` value with the current counter state.
    public static func getStatistics() -> Statistics {
        var mallocCount: Int64 = 0
        var mallocBytes: Int64 = 0
        var mallocSmall: Int64 = 0
        var mallocLarge: Int64 = 0
        var freeCount: Int64 = 0
        var freeBytes: Int64 = 0
        malloc_interposer_get_stats(&mallocCount, &mallocBytes, &mallocSmall, &mallocLarge, &freeCount, &freeBytes)
        return Statistics(
            mallocCount: Int(mallocCount),
            mallocBytesCount: Int(mallocBytes),
            mallocSmallCount: Int(mallocSmall),
            mallocLargeCount: Int(mallocLarge),
            freeCount: Int(freeCount),
            freeBytesCount: Int(freeBytes)
        )
    }
}

public extension MallocInterposerSwift {
    /// A snapshot of the interposer's allocation counters.
    ///
    /// All counts are cumulative since the most recent ``MallocInterposerSwift/reset()``
    /// (or ``MallocInterposerSwift/hook()``, which also resets). Sizes are
    /// the *requested* allocation size in bytes — not the libc usable size —
    /// for allocations that went through the header-prefixed fast path.
    /// Allocations on legacy/aligned paths (e.g. `posix_memalign`, `valloc`)
    /// are accounted using libc's usable-size query.
    struct Statistics {
        /// Total number of allocation calls (malloc + calloc + realloc-grow + …).
        public let mallocCount: Int

        /// Total requested bytes across all counted allocations.
        public let mallocBytesCount: Int

        /// Allocations whose requested size is `<=` system page size.
        ///
        /// `mallocSmallCount + mallocLargeCount == mallocCount`.
        public let mallocSmallCount: Int

        /// Allocations whose requested size is greater than the system page size.
        public let mallocLargeCount: Int

        /// Total number of `free` calls observed.
        public let freeCount: Int

        /// Total bytes freed across all counted `free` calls.
        public let freeBytesCount: Int

        /// Creates a snapshot. Primarily used internally — most callers will
        /// receive an instance from ``MallocInterposerSwift/getStatistics()``.
        public init(
            mallocCount: Int = 0,
            mallocBytesCount: Int = 0,
            mallocSmallCount: Int = 0,
            mallocLargeCount: Int = 0,
            freeCount: Int = 0,
            freeBytesCount: Int = 0
        ) {
            self.mallocCount = mallocCount
            self.mallocBytesCount = mallocBytesCount
            self.mallocSmallCount = mallocSmallCount
            self.mallocLargeCount = mallocLargeCount
            self.freeCount = freeCount
            self.freeBytesCount = freeBytesCount
        }
    }
}
