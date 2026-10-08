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

// Torture test for the allocation-stack capture path: many threads doing
// mixed malloc/free/realloc plus Swift-runtime allocation churn while
// capture toggles on/off around "measurement windows" the way
// BenchmarkExecutor brackets them. The pass criterion is simply "the
// process survives": heap corruption in the capture path manifests as
// libmalloc aborts (memory corruption of free block) or crashes in
// unrelated allocation-heavy code.

private final class Box {
    var value: Int
    init(_ value: Int) { self.value = value }
}

@inline(never)
private func swiftChurn(_ iteration: Int) -> Int {
    var boxes: [Box] = []
    boxes.reserveCapacity(16)
    for value in 0..<16 {
        boxes.append(Box(value &+ iteration))
    }
    let strings = (0..<8).map { "stress-\($0)-\(iteration)" }
    return boxes.reduce(0) { $0 &+ $1.value } &+ strings.joined().count
}

@inline(never)
private func mallocChurn(_ iteration: Int, buffer: UnsafeMutablePointer<UnsafeMutableRawPointer?>, capacity: Int) {
    // Deterministic per-thread pseudo-random sizes; a mix of size classes
    // including page-boundary-adjacent ones.
    var state = UInt64(truncatingIfNeeded: iteration &* 2654435761 &+ 1)
    func next() -> Int {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        return Int(truncatingIfNeeded: (state >> 33))
    }
    for slot in 0..<capacity {
        switch next() & 7 {
        case 0:
            buffer[slot] = replacement_malloc(1 << (4 + (next() % 12))) // 16B .. 32KiB
        case 1:
            buffer[slot] = replacement_calloc(1, 64 + (next() % 4096))
        case 2:
            buffer[slot] = replacement_malloc(16368 + (next() % 64)) // page-size adjacent
        default:
            buffer[slot] = replacement_malloc(16 + (next() % 512))
        }
    }
    for slot in 0..<capacity {
        if next() & 3 == 0, let ptr = buffer[slot] {
            buffer[slot] = replacement_realloc(ptr, 32 + (next() % 8192))
        }
    }
    for slot in 0..<capacity {
        replacement_free(buffer[slot])
        buffer[slot] = nil
    }
}

final class AllocationStacksStressTests: XCTestCase {
    func testConcurrentCaptureTorture() {
        let threadCount = 8
        let windows = 40
        let iterationsPerWindow = 200
        let slots = 64

        MallocInterposerSwift.initialize()
        MallocInterposerSwift.resetAllocationStacks()

        for window in 0..<windows {
            // Mimic BenchmarkExecutor: counting on for the whole run, capture
            // bracketed per window, snapshot read + reset between windows.
            MallocInterposerSwift.hook()
            MallocInterposerSwift.hookAllocationStacks()

            DispatchQueue.concurrentPerform(iterations: threadCount) { thread in
                let buffer = UnsafeMutablePointer<UnsafeMutableRawPointer?>.allocate(capacity: slots)
                buffer.initialize(repeating: nil, count: slots)
                defer { buffer.deallocate() }
                for iteration in 0..<iterationsPerWindow {
                    mallocChurn(window &* 100_000 &+ thread &* 10_000 &+ iteration, buffer: buffer, capacity: slots)
                    _ = swiftChurn(iteration)
                }
            }

            MallocInterposerSwift.unhookAllocationStacks()
            MallocInterposerSwift.unhook()

            // Read + reset like the executor's post-run phase; also churn the
            // heap hard with capture off (this is where the suite crashes:
            // allocation-heavy post-processing detects earlier corruption).
            let snapshot = MallocInterposerSwift.getAllocationStacks()
            XCTAssertGreaterThan(snapshot.stacks.count, 0, "window \(window) captured nothing")
            _ = (0..<64).map { swiftChurn($0) }
            MallocInterposerSwift.resetAllocationStacks()
        }
    }
}
