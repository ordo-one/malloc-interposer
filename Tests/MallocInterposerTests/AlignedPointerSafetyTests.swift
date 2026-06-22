//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

import XCTest

#if canImport(Darwin)
    import Darwin
#elseif canImport(Glibc)
    import Glibc
#elseif canImport(Musl)
    import Musl
#endif

import MallocInterposerC

/// Regression coverage for how the interposer treats pointers it did not hand
/// out (aligned/legacy allocations that carry no size header).
final class AlignedPointerSafetyTests: XCTestCase {
    private var pageSize: Int {
        Int(sysconf(Int32(_SC_PAGESIZE)))
    }

    override func setUpWithError() throws {
        // These tests drive the classifier's "read the word before the pointer"
        // probe directly, which AddressSanitizer/ThreadSanitizer reject; the
        // hooks are compiled out there, so there is nothing meaningful to test.
        try XCTSkipUnless(
            malloc_interposer_global_hooks_installed() != 0,
            "interposer hooks are disabled under sanitizers"
        )
    }

    // MARK: Issue 1 — classifying page-aligned external pointers must not fault

    // `malloc_interposer_is_ours` probes the four bytes immediately *before* the
    // pointer for its magic word. libc `valloc` / large `posix_memalign` return
    // page-aligned pointers whose preceding page is an unmapped guard page
    // (Darwin), so that probe reads unmapped memory and crashes. The classifier
    // must recognise such external pointers without dereferencing the guard
    // page. The check runs in a clean child process where libc reliably places
    // a large allocation against a guard page; this does not reproduce in the
    // populated test process. (Darwin-only: glibc keeps readable chunk metadata
    // before every allocation, so the probe never faults there.)
    #if canImport(Darwin)
        func testIsOursDoesNotFaultOnPageAlignedExternalPointer() throws {
            try assertCrashProbeSucceeds(mode: "is_ours")
        }

        func testFreeDoesNotFaultOnPageAlignedExternalPointer() throws {
            try assertCrashProbeSucceeds(mode: "free")
        }

        private func assertCrashProbeSucceeds(mode: String, file: StaticString = #filePath, line: UInt = #line) throws {
            let process = Process()
            process.executableURL = try crashProbeURL()
            process.arguments = [mode]
            try process.run()
            process.waitUntilExit()
            XCTAssertEqual(
                process.terminationReason, .exit,
                "crash probe (\(mode)) was killed by a signal — the classifier dereferenced a guard page",
                file: file, line: line
            )
            XCTAssertEqual(
                process.terminationStatus, 0,
                "crash probe (\(mode)) reported a classification error (status \(process.terminationStatus))",
                file: file, line: line
            )
        }

        /// The crash-probe executable is built into the same directory as the test
        /// bundle.
        private func crashProbeURL() throws -> URL {
            let buildDirectory = Bundle(for: Self.self).bundleURL.deletingLastPathComponent()
            let url = buildDirectory.appendingPathComponent("InterposerCrashProbe")
            try XCTSkipUnless(
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
    func testReplacementPosixMemalignHonorsAlignment() {
        for alignment in [16, 32, 64, 128, 256, 512, 1_024] {
            var pointer: UnsafeMutableRawPointer?
            let result = replacement_posix_memalign(&pointer, alignment, 1_024)
            XCTAssertEqual(result, 0, "posix_memalign(alignment: \(alignment)) returned \(result)")
            let address = UInt(bitPattern: pointer)
            XCTAssertEqual(
                address % UInt(alignment), 0,
                "posix_memalign(alignment: \(alignment)) returned 0x\(String(address, radix: 16)), not aligned"
            )
            replacement_free(pointer)
        }
    }

    /// `valloc` must return page-aligned memory.
    func testReplacementVallocIsPageAligned() {
        let pointer = replacement_valloc(1_024)
        XCTAssertNotNil(pointer)
        XCTAssertEqual(
            UInt(bitPattern: pointer) % UInt(pageSize), 0,
            "valloc returned memory that is not page-aligned"
        )
        replacement_free(pointer)
    }

    // MARK: Finding A — size queries must report usable capacity

    /// `malloc_size` / `malloc_usable_size` previously returned the *requested*
    /// size, so callers that grow in place (Swift Array/ManagedBuffer) never saw
    /// the spare capacity libc actually handed out. A size query must report the
    /// usable capacity, which for a deliberately rounded-up request exceeds it.
    func testSizeQueryReportsUsableCapacityNotRequested() {
        let requested = 1 // a 1-byte request always rounds up to a larger block
        guard let pointer = replacement_malloc(requested) else {
            return XCTFail("malloc failed")
        }
        defer { replacement_free(pointer) }
        #if canImport(Darwin)
            let reported = replacement_malloc_size(pointer)
        #else
            let reported = replacement_malloc_usable_size(pointer)
        #endif
        XCTAssertGreaterThan(
            reported, requested,
            "size query must report usable capacity, not the requested size"
        )
    }

    // MARK: Issue 8 — aligned allocators must be counted

    /// `aligned_alloc` was not intercepted, so its allocations were invisible to
    /// the counters while their frees still counted — an unbalanced malloc/free
    /// delta. Each alloc/free pair must move both counters.
    func testAlignedAllocIsCounted() {
        assertAllocationsAreCounted {
            replacement_aligned_alloc(64, 1_024)
        }
    }

    #if !canImport(Darwin)
        /// `memalign` (glibc) had the same gap as `aligned_alloc`.
        func testMemalignIsCounted() {
            assertAllocationsAreCounted {
                replacement_memalign(64, 1_024)
            }
        }
    #endif

    /// Runs `allocate` in a loop with counting enabled and asserts both the
    /// malloc and free counters advanced by at least the iteration count.
    /// Counting is process-global, so background allocations only ever *add* to
    /// the deltas — a large loop keeps the signal well clear of that noise.
    private func assertAllocationsAreCounted(
        _ allocate: () -> UnsafeMutableRawPointer?,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
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

        XCTAssertGreaterThanOrEqual(
            after.malloc - before.malloc, Int64(iterations),
            "allocations were not counted", file: file, line: line
        )
        XCTAssertGreaterThanOrEqual(
            after.free - before.free, Int64(iterations),
            "frees were not counted", file: file, line: line
        )
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
}
