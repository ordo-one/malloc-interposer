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
}
