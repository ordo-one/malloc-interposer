//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

import Foundation
import Testing

#if canImport(Darwin)
    import Darwin
#elseif canImport(Glibc)
    import Glibc
#elseif canImport(Musl)
    import Musl
#endif

import MallocInterposerC

#if canImport(Darwin)
    // `Bundle(for:)` needs a class to locate the test bundle's directory.
    private final class BundleMarker {}
#endif

/// Regression coverage for how the interposer treats pointers it did not hand
/// out (aligned/legacy allocations that carry no size header).
///
/// Serialized because several tests share the interposer's process-global
/// counters. Skipped under AddressSanitizer/ThreadSanitizer, where the
/// classifier's read-before-pointer probe is incompatible with their bounds
/// checking so the hooks are compiled out.
@Suite(.serialized, .enabled(if: malloc_interposer_global_hooks_installed() != 0))
struct AlignedPointerSafetyTests {
    private var pageSize: Int {
        Int(sysconf(Int32(_SC_PAGESIZE)))
    }

    // MARK: Issue 1 — classifying page-aligned external pointers must not fault

    // `malloc_interposer_is_ours` probes the bytes immediately *before* the
    // pointer for its magic word. libc `valloc` / large `posix_memalign` return
    // page-aligned pointers whose preceding page is an unmapped guard page
    // (Darwin), so that probe reads unmapped memory and crashes. The classifier
    // must recognise such external pointers without dereferencing the guard
    // page. The check runs in a clean child process where libc reliably places a
    // large allocation against a guard page; this does not reproduce in the
    // populated test process. (Darwin-only: glibc keeps readable chunk metadata
    // before every allocation, so the probe never faults there.)
    #if canImport(Darwin)
        @Test
        func isOursDoesNotFaultOnPageAlignedExternalPointer() throws {
            try assertCrashProbeSucceeds(mode: "is_ours")
        }

        @Test
        func freeDoesNotFaultOnPageAlignedExternalPointer() throws {
            try assertCrashProbeSucceeds(mode: "free")
        }

        private func assertCrashProbeSucceeds(mode: String) throws {
            let process = Process()
            process.executableURL = try crashProbeURL()
            process.arguments = [mode]
            try process.run()
            process.waitUntilExit()
            #expect(
                process.terminationReason == .exit,
                "crash probe (\(mode)) was killed by a signal — the classifier dereferenced a guard page"
            )
            #expect(
                process.terminationStatus == 0,
                "crash probe (\(mode)) reported a classification error (status \(process.terminationStatus))"
            )
        }

        /// The crash-probe executable is built into the same directory as the test bundle.
        private func crashProbeURL() throws -> URL {
            let buildDirectory = Bundle(for: BundleMarker.self).bundleURL.deletingLastPathComponent()
            let url = buildDirectory.appendingPathComponent("InterposerCrashProbe")
            try #require(
                FileManager.default.isExecutableFile(atPath: url.path),
                "InterposerCrashProbe not found at \(url.path)"
            )
            return url
        }
    #endif

    // MARK: Issue 2 — aligned allocators must honour the requested alignment

    /// `posix_memalign` must return memory aligned to the requested boundary;
    /// the interposer previously discarded the alignment and returned only
    /// 16-byte-aligned memory.
    @Test
    func posixMemalignHonorsAlignment() {
        for alignment in [16, 32, 64, 128, 256, 512, 1_024] {
            var pointer: UnsafeMutableRawPointer?
            let result = replacement_posix_memalign(&pointer, alignment, 1_024)
            #expect(result == 0, "posix_memalign(alignment: \(alignment)) returned \(result)")
            let address = UInt(bitPattern: pointer)
            #expect(
                address.isMultiple(of: UInt(alignment)),
                "posix_memalign(alignment: \(alignment)) returned 0x\(String(address, radix: 16)), not aligned"
            )
            replacement_free(pointer)
        }
    }

    /// `valloc` must return page-aligned memory.
    @Test
    func vallocIsPageAligned() {
        let pointer = replacement_valloc(1_024)
        #expect(pointer != nil)
        #expect(
            UInt(bitPattern: pointer).isMultiple(of: UInt(pageSize)),
            "valloc returned memory that is not page-aligned"
        )
        replacement_free(pointer)
    }

    // MARK: Finding A — size queries must report usable capacity

    /// `malloc_size` / `malloc_usable_size` previously returned the *requested*
    /// size, so callers that grow in place (Swift Array/ManagedBuffer) never saw
    /// the spare capacity libc actually handed out. A size query must report the
    /// usable capacity, which for a deliberately rounded-up request exceeds it.
    @Test
    func sizeQueryReportsUsableCapacityNotRequested() throws {
        let requested = 1 // a 1-byte request always rounds up to a larger block
        let pointer = try #require(replacement_malloc(requested), "malloc failed")
        defer { replacement_free(pointer) }
        #if canImport(Darwin)
            let reported = replacement_malloc_size(pointer)
        #else
            let reported = replacement_malloc_usable_size(pointer)
        #endif
        #expect(reported > requested, "size query must report usable capacity, not the requested size")
    }

    // MARK: Issue 8 — aligned allocators must be counted

    /// `aligned_alloc` was not intercepted, so its allocations were invisible to
    /// the counters while their frees still counted — an unbalanced malloc/free
    /// delta. Each alloc/free pair must move both counters.
    @Test
    func alignedAllocIsCounted() {
        assertAllocationsAreCounted { replacement_aligned_alloc(64, 1_024) }
    }

    #if !canImport(Darwin)
        /// `memalign` (glibc) had the same gap as `aligned_alloc`.
        @Test
        func memalignIsCounted() {
            assertAllocationsAreCounted { replacement_memalign(64, 1_024) }
        }
    #endif

    // MARK: Item B — the magic word alone must not claim a foreign pointer

    /// A foreign block that merely happens to have our magic word in the bytes
    /// before it must not be mis-claimed (that would free the wrong address).
    /// The address-keyed tag closes the ~2⁻³² magic-only collision.
    @Test
    func foreignPointerWithMagicButWrongTagIsNotOurs() {
        withUnsafeTemporaryAllocation(byteCount: 64, alignment: 16) { buffer in
            let base = buffer.baseAddress!
            // Lay out a would-be 16-byte header before a fake user pointer:
            // tag at offset 8 (left 0, won't match addr_tag), magic at offset 12.
            (base + 8).storeBytes(of: UInt32(0), as: UInt32.self)
            (base + 12).storeBytes(of: UInt32(0xC0FF_EE5A), as: UInt32.self)
            let fakeUser = UnsafeRawPointer(base + 16)
            #expect(
                !malloc_interposer_is_ours(fakeUser),
                "a foreign pointer with only the magic (no matching address tag) must not be claimed"
            )
        }
    }

    // MARK: Item 7 — small/large split uses a fixed, architecture-independent boundary

    /// The small/large boundary is a fixed 16 KiB constant, not the page size, so
    /// an 8 KiB allocation is "small" on every architecture. With the page-size
    /// split it would be "large" on a 4 KiB-page system (e.g. x86_64 Linux).
    @Test
    func smallLargeSplitUsesFixedBoundary() {
        let iterations = 4_096
        malloc_interposer_reset()
        malloc_interposer_enable()
        let before = currentSizeClasses()
        for _ in 0 ..< iterations {
            if let pointer = replacement_malloc(8 * 1_024) { // > 4 KiB page, < 16 KiB threshold
                replacement_free(pointer)
            }
        }
        let after = currentSizeClasses()
        malloc_interposer_disable()
        #expect(
            after.small - before.small >= Int64(iterations),
            "8 KiB allocations must be classified small on every architecture"
        )
    }

    // MARK: Item C — calloc returns zeroed, counted memory

    /// Switching Linux calloc from malloc+memset to libc calloc must preserve
    /// behavior: zeroed memory, counted as one allocation. (Regression guard; the
    /// change itself is a page-fault/perf improvement with no visible delta.)
    @Test
    func callocReturnsZeroedCountedMemory() throws {
        let count = 256, size = 8
        malloc_interposer_reset()
        malloc_interposer_enable()
        let before = currentCounts()
        let pointer = try #require(replacement_calloc(count, size), "calloc failed")
        let after = currentCounts()
        var allZero = true
        for offset in 0 ..< (count * size) where pointer.load(fromByteOffset: offset, as: UInt8.self) != 0 {
            allZero = false
            break
        }
        replacement_free(pointer)
        malloc_interposer_disable()
        #expect(allZero, "calloc memory must be zeroed")
        #expect(after.malloc - before.malloc == 1, "calloc must count as one allocation")
    }

    /// Runs `allocate` in a loop with counting enabled and asserts both the
    /// malloc and free counters advanced by at least the iteration count.
    /// Counting is process-global, so background allocations only ever *add* to
    /// the deltas — a large loop keeps the signal well clear of that noise.
    private func assertAllocationsAreCounted(_ allocate: () -> UnsafeMutableRawPointer?) {
        let iterations = 4_096
        malloc_interposer_reset()
        malloc_interposer_enable()
        let before = currentCounts()
        for _ in 0 ..< iterations {
            if let pointer = allocate() {
                replacement_free(pointer)
            }
        }
        let after = currentCounts()
        malloc_interposer_disable()

        #expect(after.malloc - before.malloc >= Int64(iterations), "allocations were not counted")
        #expect(after.free - before.free >= Int64(iterations), "frees were not counted")
    }

    private func currentCounts() -> (malloc: Int64, free: Int64) {
        var mallocCount: Int64 = 0, mallocBytes: Int64 = 0
        var mallocSmall: Int64 = 0, mallocLarge: Int64 = 0
        var freeCount: Int64 = 0, freeBytes: Int64 = 0
        malloc_interposer_get_stats(
            &mallocCount, &mallocBytes, &mallocSmall, &mallocLarge, &freeCount, &freeBytes
        )
        return (mallocCount, freeCount)
    }

    private func currentSizeClasses() -> (small: Int64, large: Int64) {
        var mallocCount: Int64 = 0, mallocBytes: Int64 = 0
        var mallocSmall: Int64 = 0, mallocLarge: Int64 = 0
        var freeCount: Int64 = 0, freeBytes: Int64 = 0
        malloc_interposer_get_stats(
            &mallocCount, &mallocBytes, &mallocSmall, &mallocLarge, &freeCount, &freeBytes
        )
        return (mallocSmall, mallocLarge)
    }
}
