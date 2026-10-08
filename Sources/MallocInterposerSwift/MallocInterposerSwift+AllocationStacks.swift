//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

import MallocInterposerC

public extension MallocInterposerSwift {
    /// One unique allocation call stack with its aggregated counts.
    struct AllocationStack: Sendable {
        /// Leaf-first raw program counters (return addresses). Only
        /// meaningful inside the process that captured them; symbolicate
        /// before crossing a process boundary.
        public let frames: [UInt]

        /// Number of allocations that produced exactly this stack.
        public let count: Int

        /// Sum of requested sizes across those allocations.
        public let bytes: Int

        public init(frames: [UInt], count: Int, bytes: Int) {
            self.frames = frames
            self.count = count
            self.bytes = bytes
        }
    }

    /// Everything captured since the last ``resetAllocationStacks()``.
    struct AllocationStackSnapshot: Sendable {
        /// Unique stacks in capture-insertion order.
        public let stacks: [AllocationStack]

        /// Allocations whose stack could not be recorded (frame-pointer walk
        /// failed or the capture table was full). Their allocation *counts*
        /// in ``MallocInterposerSwift/Statistics`` are still exact.
        public let droppedAllocations: Int

        public init(stacks: [AllocationStack], droppedAllocations: Int) {
            self.stacks = stacks
            self.droppedAllocations = droppedAllocations
        }
    }

    /// Enables allocation call-stack capture.
    ///
    /// Capture only happens while counting is also enabled (``hook()``);
    /// the capture hook lives on the counting path. The first call maps the
    /// fixed-capacity capture table. Idempotent.
    static func hookAllocationStacks() {
        malloc_interposer_stacks_enable()
    }

    /// Disables allocation call-stack capture. Captured stacks remain
    /// readable via ``getAllocationStacks()``. Idempotent.
    static func unhookAllocationStacks() {
        malloc_interposer_stacks_disable()
    }

    /// Opens a measurement window: notes every captured stack's running
    /// totals so that only growth from here on is reported. Calling it again
    /// before ``commitAllocationStacks()`` discards what was recorded since
    /// the previous mark (a benchmark's setup before an explicit
    /// `startMeasurement()`), mirroring how the allocation counters re-read
    /// their start value. Allocation-free; safe while capture is enabled.
    static func markAllocationStacks() {
        malloc_interposer_stacks_mark()
    }

    /// Closes the measurement window opened by ``markAllocationStacks()``:
    /// adds the growth since that mark to each stack's reported totals.
    /// Allocation-free. Without mark/commit, reports are plain running
    /// totals since the last reset.
    static func commitAllocationStacks() {
        malloc_interposer_stacks_commit()
    }

    /// Clears all captured stacks and the dropped-sample counter.
    ///
    /// Must be called while capture is disabled.
    static func resetAllocationStacks() {
        malloc_interposer_stacks_reset()
    }

    /// Reads all captured unique stacks.
    ///
    /// Must be called while capture is disabled (allocations made by this
    /// method itself would otherwise race the table it is reading).
    static func getAllocationStacks() -> AllocationStackSnapshot {
        final class Collector {
            var stacks: [AllocationStack] = []
        }
        let collector = Collector()
        // Avoid growing/freeing the outer snapshot buffer from inside the C
        // visitor; optimized builds can otherwise trip over the old backing
        // buffer while unwinding the callback-heavy report path.
        collector.stacks.reserveCapacity(malloc_interposer_stacks_count())
        // The C function only sees an opaque raw pointer, so ARC cannot tell
        // that the callback keeps using `collector` for the duration of the
        // call; in optimized builds the object could be released while the
        // callback is still appending into it (use-after-free heap
        // corruption). withExtendedLifetime pins it across the call.
        withExtendedLifetime(collector) {
            let context = Unmanaged.passUnretained(collector).toOpaque()
            malloc_interposer_stacks_iterate({ stack, context in
                guard let stack, let context else { return }
                let collector = Unmanaged<Collector>.fromOpaque(context).takeUnretainedValue()
                let entry = stack.pointee
                var frames = [UInt]()
                frames.reserveCapacity(Int(entry.depth))
                if let framePointers = entry.frames {
                    for index in 0..<Int(entry.depth) {
                        frames.append(UInt(bitPattern: framePointers[index]))
                    }
                }
                collector.stacks.append(
                    AllocationStack(frames: frames, count: Int(entry.count), bytes: Int(entry.bytes))
                )
            }, context)
        }
        return AllocationStackSnapshot(
            stacks: collector.stacks,
            droppedAllocations: Int(malloc_interposer_stacks_dropped())
        )
    }
}
