//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

import Dispatch
import Testing

import AllocationHookTestSupport
import MallocInterposerC

/// Per-thread state of the hook. The thread-local word holds a
/// pointer to it plus the in-hook guard bit.
private final class ThreadState {
    /// Allocations recorded by the hook on this thread, and their bytes.
    var allocations = 0
    var bytes = 0
    /// Allocations made by the hook itself on this thread.
    var nested = 0

    static let inHookFlag: UInt = 1

    /// The calling thread's state, created on first use. Safe to call while the
    /// hook is installed: the guard is held while creating it.
    static var current: ThreadState {
        let word = allocation_hook_state_get()
        if let state = from(word) {
            return state
        }
        allocation_hook_state_set(word | inHookFlag)
        let state = make()
        allocation_hook_state_set(pointer(to: state) | (word & inHookFlag))
        return state
    }

    static func from(_ word: UInt) -> ThreadState? {
        UnsafeRawPointer(bitPattern: word & ~inHookFlag).map {
            Unmanaged<ThreadState>.fromOpaque($0).takeUnretainedValue()
        }
    }

    static func pointer(to state: ThreadState) -> UInt {
        UInt(bitPattern: Unmanaged.passUnretained(state).toOpaque())
    }

    /// Thread states are never freed: a thread's word points at its state
    /// unretained. (A real hook would also register them to aggregate across
    /// threads; the tests only read the current thread's state.)
    private static func make() -> ThreadState {
        let state = ThreadState()
        _ = Unmanaged.passRetained(state)
        return state
    }

    func reset() {
        allocations = 0
        bytes = 0
        nested = 0
    }
}

private func handleAllocation(size: Int, allocatesItself: Bool) {
    let word = allocation_hook_state_get()
    if word & ThreadState.inHookFlag != 0 {
        // One of the hook's own allocations re-entering it: tally, don't record.
        ThreadState.from(word)?.nested += 1
        return
    }
    // Set the guard before anything can allocate, including creating the state.
    allocation_hook_state_set(word | ThreadState.inHookFlag)
    let state = ThreadState.from(word) ?? ThreadState.current
    state.allocations += 1
    state.bytes += size
    if allocatesItself {
        // Stands in for the hook's real work, e.g. capturing a backtrace.
        replacement_free(replacement_malloc(64))
    }
    allocation_hook_state_set(ThreadState.pointer(to: state))
}

private let recordingHook: malloc_interposer_allocation_hook_t = { size in
    handleAllocation(size: size, allocatesItself: false)
}

private let allocatingHook: malloc_interposer_allocation_hook_t = { size in
    handleAllocation(size: size, allocatesItself: true)
}

private final class Box {
    var value = 0
}

// Nested in `AlignedPointerSafetyTests` so it inherits `.serialized`.
extension AlignedPointerSafetyTests {
    /// Coverage for `malloc_interposer_set_allocation_hook`.
    @Suite
    struct AllocationHookTests {
        private let iterations = 1_000

        init() {
            allocation_hook_state_initialize()
        }

        /// The hook fires exactly once per counted allocation, with its size.
        @Test
        func hookFiresOncePerCountedAllocation() {
            let state = ThreadState.current
            state.reset()
            withHook(recordingHook, counting: true) {
                for _ in 0 ..< iterations {
                    replacement_free(replacement_malloc(64))
                }
            }
            #expect(state.allocations == iterations)
            #expect(state.bytes == 64 * iterations)
            #expect(state.nested == 0)
        }

        /// calloc and growing realloc are allocations too and must reach the hook.
        @Test
        func hookSeesCallocAndReallocGrowth() {
            let state = ThreadState.current
            state.reset()
            withHook(recordingHook, counting: true) {
                for _ in 0 ..< iterations {
                    replacement_free(replacement_calloc(4, 16))
                    let small = replacement_malloc(16)
                    replacement_free(replacement_realloc(small, 256))
                }
            }
            #expect(state.allocations == 3 * iterations)
            #expect(state.bytes == (64 + 16 + 256) * iterations)
        }

        /// The hook only runs while counting is enabled.
        @Test
        func hookDoesNotFireWhileCountingIsDisabled() {
            let state = ThreadState.current
            state.reset()
            withHook(recordingHook, counting: false) {
                for _ in 0 ..< iterations {
                    replacement_free(replacement_malloc(64))
                }
            }
            #expect(state.allocations == 0)
        }

        /// Installing NULL removes the hook.
        @Test
        func removingHookStopsCallbacks() {
            let state = ThreadState.current
            state.reset()
            malloc_interposer_set_allocation_hook(recordingHook)
            malloc_interposer_set_allocation_hook(nil)
            malloc_interposer_enable()
            for _ in 0 ..< iterations {
                replacement_free(replacement_malloc(64))
            }
            malloc_interposer_disable()
            #expect(state.allocations == 0)
        }

        /// A hook that allocates is re-entered once per own allocation; with its
        /// per-thread guard it terminates and records only the outer calls. The
        /// interposer still counts the nested allocations, so the caller has to
        /// subtract the tally.
        @Test
        func allocatingHookGuardsItselfAndNestedAllocationsAreCounted() {
            let state = ThreadState.current
            state.reset()
            malloc_interposer_reset()
            let before = mallocCount()
            withHook(allocatingHook, counting: true) {
                for _ in 0 ..< iterations {
                    replacement_free(replacement_malloc(64))
                }
            }
            let after = mallocCount()
            #expect(state.allocations == iterations)
            #expect(state.nested == iterations)
            #expect(after - before >= Int64(2 * iterations), "allocations made by the hook must be counted")
        }

        /// The hook runs on whichever thread allocates; with per-thread state,
        /// concurrent allocations on another thread don't mix into this one's.
        @Test
        func otherThreadsRecordIntoTheirOwnState() {
            let start = DispatchSemaphore(value: 0)
            let done = DispatchSemaphore(value: 0)
            let workerCount = Box()
            // Enqueue before hooking: scheduling the work allocates on this thread.
            DispatchQueue.global().async { [iterations] in
                start.wait()
                let workerState = ThreadState.current
                let before = workerState.allocations
                for _ in 0 ..< iterations {
                    replacement_free(replacement_malloc(64))
                }
                workerCount.value = workerState.allocations - before
                done.signal()
            }

            let state = ThreadState.current
            state.reset()
            withHook(recordingHook, counting: true) {
                start.signal()
                for _ in 0 ..< 2 * iterations {
                    replacement_free(replacement_malloc(64))
                }
                done.wait()
            }
            #expect(state.allocations == 2 * iterations)
            #expect(workerCount.value >= iterations)
        }

        private func withHook(
            _ hook: malloc_interposer_allocation_hook_t,
            counting: Bool,
            _ body: () -> Void
        ) {
            malloc_interposer_set_allocation_hook(hook)
            if counting {
                malloc_interposer_enable()
            }
            body()
            if counting {
                malloc_interposer_disable()
            }
            malloc_interposer_set_allocation_hook(nil)
        }

        private func mallocCount() -> Int64 {
            var mallocCount: Int64 = 0, mallocBytes: Int64 = 0
            var mallocSmall: Int64 = 0, mallocLarge: Int64 = 0
            var freeCount: Int64 = 0, freeBytes: Int64 = 0
            malloc_interposer_get_stats(
                &mallocCount, &mallocBytes, &mallocSmall, &mallocLarge, &freeCount, &freeBytes
            )
            return mallocCount
        }
    }
}
