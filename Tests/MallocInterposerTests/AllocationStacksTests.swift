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
import MallocInterposerC
import MallocInterposerSwift
import XCTest

// Prevents tail-call optimization of the recursive allocator so every
// recursion level contributes a real stack frame.
private var recursionSideEffect = 0

// Distinct @inline(never) call sites so their captured stacks differ.
// Allocations use replacement_malloc directly, no preload needed, and
// pointers land in a pre-allocated buffer so the site itself performs no
// Swift-runtime allocations between the measured calls.

@inline(never)
private func allocateSiteOne(count: Int, size: Int, into buffer: UnsafeMutablePointer<UnsafeMutableRawPointer?>) {
    for index in 0..<count {
        buffer[index] = replacement_malloc(size)
    }
}

@inline(never)
private func allocateSiteTwo(count: Int, size: Int, into buffer: UnsafeMutablePointer<UnsafeMutableRawPointer?>) {
    for index in 0..<count {
        buffer[index] = replacement_malloc(size)
    }
}

@inline(never)
private func allocateAtRecursionDepth(_ depth: Int, size: Int, into buffer: UnsafeMutablePointer<UnsafeMutableRawPointer?>, slot: Int) {
    if depth <= 0 {
        buffer[slot] = replacement_malloc(size)
    } else {
        allocateAtRecursionDepth(depth - 1, size: size, into: buffer, slot: slot)
    }
    recursionSideEffect += 1
}

final class AllocationStacksTests: XCTestCase {
    private let defaultCapacity: UInt32 = 1 << 16

    override func setUp() {
        super.setUp()
        MallocInterposerSwift.unhookAllocationStacks()
        MallocInterposerSwift.unhook()
        malloc_interposer_stacks_test_set_capacity(defaultCapacity)
        MallocInterposerSwift.initialize()
        MallocInterposerSwift.resetAllocationStacks()
    }

    override func tearDown() {
        MallocInterposerSwift.unhookAllocationStacks()
        MallocInterposerSwift.unhook()
        MallocInterposerSwift.resetAllocationStacks()
        super.tearDown()
    }

    private func freeAll(_ buffer: UnsafeMutablePointer<UnsafeMutableRawPointer?>, count: Int) {
        for index in 0..<count where buffer[index] != nil {
            replacement_free(buffer[index])
        }
    }

    // Finds the captured stack matching an allocation site by its unique
    // (count, bytes) signature, robust against unrelated runtime
    // allocations that are also captured. Match a [size, 2*size) band per
    // allocation rather than the exact requested bytes so the lookup does
    // not depend on how the platform interposer accounts bytes.
    private func entry(
        in snapshot: MallocInterposerSwift.AllocationStackSnapshot, count: Int, size: Int
    ) -> MallocInterposerSwift.AllocationStack? {
        snapshot.stacks.first {
            $0.count == count && $0.bytes >= count * size && $0.bytes < count * size * 2
        }
    }

    func testExternalConsumerSwiftAllocationsAreCapturedThroughPublicAPI() throws {
        let sourceRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let packageURL = sourceRoot.appendingPathComponent("Tests/AllocationStacksConsumer")
        let scratchURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("malloc-interposer-consumer-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: scratchURL) }

        let swiftURL = URL(
            fileURLWithPath: ProcessInfo.processInfo.environment["SWIFT_EXEC"] ?? "/usr/bin/swift"
        )
        try XCTSkipUnless(
            FileManager.default.isExecutableFile(atPath: swiftURL.path),
            "swift executable not found at \(swiftURL.path)"
        )

        let output = Pipe()
        let error = Pipe()
        let process = Process()
        process.executableURL = swiftURL
        process.currentDirectoryURL = packageURL
        process.arguments = [
            "run", "-c", "release",
            "--scratch-path", scratchURL.path,
            "AllocationStacksConsumer", "32",
        ]
        process.standardOutput = output
        process.standardError = error

        try process.run()
        process.waitUntilExit()

        let stdout = String(data: output.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        let stderr = String(data: error.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""

        XCTAssertEqual(
            process.terminationReason,
            .exit,
            "consumer was killed unexpectedly\nstdout:\n\(stdout)\nstderr:\n\(stderr)"
        )
        XCTAssertEqual(
            process.terminationStatus,
            0,
            "consumer failed\nstdout:\n\(stdout)\nstderr:\n\(stderr)"
        )
        XCTAssertTrue(stdout.contains("CONSUMER_OK"), "consumer did not report success\nstdout:\n\(stdout)")
    }

    func testDistinctCallSitesProduceDistinctStacksWithExactCounts() {
        let count = 10
        let sizeOne = 12345
        let sizeTwo = 23456
        let buffer = UnsafeMutablePointer<UnsafeMutableRawPointer?>.allocate(capacity: count * 2)
        defer { buffer.deallocate() }

        MallocInterposerSwift.hook()
        MallocInterposerSwift.hookAllocationStacks()
        allocateSiteOne(count: count, size: sizeOne, into: buffer)
        allocateSiteTwo(count: count, size: sizeTwo, into: buffer + count)
        MallocInterposerSwift.unhookAllocationStacks()
        MallocInterposerSwift.unhook()

        let capturedStackCount = malloc_interposer_stacks_count()
        let snapshot = MallocInterposerSwift.getAllocationStacks()
        freeAll(buffer, count: count * 2)

        XCTAssertEqual(snapshot.stacks.count, capturedStackCount)
        let one = entry(in: snapshot, count: count, size: sizeOne)
        let two = entry(in: snapshot, count: count, size: sizeTwo)
        XCTAssertNotNil(one, "expected an aggregated stack for site one")
        XCTAssertNotNil(two, "expected an aggregated stack for site two")
        guard let one, let two else { return }
        XCTAssertFalse(one.frames.isEmpty)
        XCTAssertFalse(two.frames.isEmpty)
        XCTAssertNotEqual(one.frames, two.frames, "different call sites must aggregate separately")
    }

    func testResetClearsStacksAndDroppedCounter() {
        let count = 5
        let size = 34567
        let buffer = UnsafeMutablePointer<UnsafeMutableRawPointer?>.allocate(capacity: count)
        defer { buffer.deallocate() }

        MallocInterposerSwift.hook()
        MallocInterposerSwift.hookAllocationStacks()
        allocateSiteOne(count: count, size: size, into: buffer)
        MallocInterposerSwift.unhookAllocationStacks()
        MallocInterposerSwift.unhook()
        freeAll(buffer, count: count)

        XCTAssertNotNil(entry(in: MallocInterposerSwift.getAllocationStacks(), count: count, size: size))

        MallocInterposerSwift.resetAllocationStacks()
        let snapshot = MallocInterposerSwift.getAllocationStacks()
        XCTAssertTrue(snapshot.stacks.isEmpty)
        XCTAssertEqual(snapshot.droppedAllocations, 0)
    }

    func testNoCaptureWhileDisabled() {
        let count = 5
        let size = 45678
        let buffer = UnsafeMutablePointer<UnsafeMutableRawPointer?>.allocate(capacity: count)
        defer { buffer.deallocate() }

        // Counting on, capture off: the counting path must not record stacks.
        MallocInterposerSwift.hook()
        allocateSiteOne(count: count, size: size, into: buffer)
        MallocInterposerSwift.unhook()
        freeAll(buffer, count: count)

        let snapshot = MallocInterposerSwift.getAllocationStacks()
        XCTAssertNil(entry(in: snapshot, count: count, size: size))
    }

    func testDeepRecursionIsCappedAtMaxDepth() {
        let size = 56789
        let buffer = UnsafeMutablePointer<UnsafeMutableRawPointer?>.allocate(capacity: 1)
        defer { buffer.deallocate() }

        MallocInterposerSwift.hook()
        MallocInterposerSwift.hookAllocationStacks()
        allocateAtRecursionDepth(100, size: size, into: buffer, slot: 0)
        MallocInterposerSwift.unhookAllocationStacks()
        MallocInterposerSwift.unhook()
        freeAll(buffer, count: 1)

        let snapshot = MallocInterposerSwift.getAllocationStacks()
        guard let capped = entry(in: snapshot, count: 1, size: size) else {
            XCTFail("expected a captured stack for the recursive allocation")
            return
        }
        XCTAssertEqual(capped.frames.count, 64, "stacks deeper than the cap must be truncated to it")
    }

    func testMarkCommitReportsOnlyMeasuredWindows() {
        let count = 5
        let sizeSetup = 11111
        let sizeMeasured = 22222
        let buffer = UnsafeMutablePointer<UnsafeMutableRawPointer?>.allocate(capacity: count * 3)
        defer { buffer.deallocate() }

        MallocInterposerSwift.hook()
        MallocInterposerSwift.hookAllocationStacks()

        // Two windows driven the way BenchmarkExecutor does it: implicit
        // mark, setup allocations, explicit re-mark (discarding the setup),
        // measured allocations, commit. One loop so the measured site is the
        // same call site (hence the same stack) in both windows and must
        // accumulate across them.
        for window in 0..<2 {
            MallocInterposerSwift.markAllocationStacks()
            if window == 0 {
                allocateSiteOne(count: count, size: sizeSetup, into: buffer)
            }
            MallocInterposerSwift.markAllocationStacks()
            allocateSiteTwo(count: count, size: sizeMeasured, into: buffer + (1 + window) * count)
            MallocInterposerSwift.commitAllocationStacks()
        }

        MallocInterposerSwift.unhookAllocationStacks()
        MallocInterposerSwift.unhook()

        let snapshot = MallocInterposerSwift.getAllocationStacks()
        freeAll(buffer, count: count * 3)

        XCTAssertNil(entry(in: snapshot, count: count, size: sizeSetup), "setup before the re-mark must be discarded")
        XCTAssertNotNil(entry(in: snapshot, count: count * 2, size: sizeMeasured), "both windows must accumulate")
        XCTAssertEqual(snapshot.stacks.count, malloc_interposer_stacks_count())
    }

    func testTableOverflowDropsSamplesButKeepsCountersExact() {
        malloc_interposer_stacks_test_set_capacity(8)
        defer { malloc_interposer_stacks_test_set_capacity(defaultCapacity) }

        let uniqueStacks = 64
        let size = 67890
        let buffer = UnsafeMutablePointer<UnsafeMutableRawPointer?>.allocate(capacity: uniqueStacks)
        defer { buffer.deallocate() }

        MallocInterposerSwift.initialize()
        MallocInterposerSwift.hook()
        MallocInterposerSwift.hookAllocationStacks()
        // Each recursion depth yields a distinct stack: far more unique
        // stacks than the 8-slot table can hold.
        for depth in 0..<uniqueStacks {
            allocateAtRecursionDepth(depth, size: size, into: buffer, slot: depth)
        }
        MallocInterposerSwift.unhookAllocationStacks()
        MallocInterposerSwift.unhook()

        let snapshot = MallocInterposerSwift.getAllocationStacks()
        let statistics = MallocInterposerSwift.getStatistics()
        freeAll(buffer, count: uniqueStacks)

        XCTAssertGreaterThan(snapshot.droppedAllocations, 0, "overflow must be reported, not silent")
        XCTAssertLessThanOrEqual(snapshot.stacks.count, 8)
        // The allocation counters must stay exact even when stacks are dropped.
        XCTAssertGreaterThanOrEqual(statistics.mallocCount, uniqueStacks)
    }
}
