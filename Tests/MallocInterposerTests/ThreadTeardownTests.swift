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
    private final class TeardownBundleMarker {}

    /// Nested in `AlignedPointerSafetyTests` so it inherits `.serialized`.
    extension AlignedPointerSafetyTests {
        /// Allocations made during thread teardown, after the interposer's TSD
        /// destructor has freed the thread's counter block (other TSD
        /// destructors, the runtime's per-thread cleanup), must be counted and
        /// must not write into the freed block. Runs `ThreadTeardownProbe` with
        /// the interposer dylib preloaded, as package-benchmark loads it.
        struct ThreadTeardownTests {
            @Test
            func lateAllocationsAreCounted() throws {
                try runProbe(threads: 1, late: 100)
            }

            @Test
            func lateAllocationsDoNotCorruptTheHeap() throws {
                try runProbe(threads: 2_000, late: 16)
            }

            private func runProbe(threads: Int, late: Int) throws {
                let buildDirectory = Bundle(for: TeardownBundleMarker.self).bundleURL.deletingLastPathComponent()
                let probe = buildDirectory.appendingPathComponent("ThreadTeardownProbe")
                let dylib = buildDirectory.appendingPathComponent("libMallocInterposerSwift.dylib")
                try #require(FileManager.default.isExecutableFile(atPath: probe.path), "probe not found at \(probe.path)")
                try #require(FileManager.default.fileExists(atPath: dylib.path), "dylib not found at \(dylib.path)")

                let process = Process()
                process.executableURL = probe
                process.arguments = [String(threads), String(late)]
                process.environment = ["DYLD_INSERT_LIBRARIES": dylib.path]
                try process.run()
                process.waitUntilExit()

                #expect(
                    process.terminationReason == .exit,
                    "probe (\(threads) threads, \(late) late allocations) was killed by signal \(process.terminationStatus)"
                )
                #expect(
                    process.terminationStatus == 0,
                    "probe (\(threads) threads, \(late) late allocations) exited with \(process.terminationStatus)"
                )
            }
        }
    }
#endif
