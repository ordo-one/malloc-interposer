//
// Copyright (c) 2026 Ordo One AB.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//

import MallocInterposerSwift

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#endif

private struct Entry {
    var frames: [UInt]
    var count: Int
    var bytes: Int
}

@inline(never)
private func consumerWork(iterations: Int) -> Int {
    var checksum = 0
    for iteration in 0..<iterations {
        var entries: [Entry] = []
        entries.reserveCapacity(32)
        for value in 0..<32 {
            entries.append(
                Entry(
                    frames: [UInt(value), UInt(iteration), 0xCAFE],
                    count: value,
                    bytes: 128 + value
                )
            )
        }

        let bytes = [UInt8](
            repeating: UInt8(truncatingIfNeeded: iteration),
            count: 256 + (iteration & 31)
        )
        let string = String(repeating: "allocation-stack-consumer", count: 4 + (iteration & 3))

        checksum &+= entries.reduce(0) { $0 &+ $1.frames.count &+ $1.count &+ $1.bytes }
        checksum &+= bytes.count
        checksum &+= string.utf8.count
    }
    return checksum
}

let iterations = CommandLine.arguments.dropFirst().first.flatMap(Int.init) ?? 32

MallocInterposerSwift.initialize()
MallocInterposerSwift.resetAllocationStacks()
MallocInterposerSwift.hook()
MallocInterposerSwift.hookAllocationStacks()
let checksum = consumerWork(iterations: max(1, iterations))
MallocInterposerSwift.unhookAllocationStacks()
MallocInterposerSwift.unhook()

let statistics = MallocInterposerSwift.getStatistics()
let snapshot = MallocInterposerSwift.getAllocationStacks()

guard checksum > 0 else {
    fputs("consumer checksum was zero\n", stderr)
    exit(2)
}

guard statistics.mallocCount > 0 else {
    fputs("consumer saw no interposed allocations\n", stderr)
    exit(3)
}

guard !snapshot.stacks.isEmpty else {
    fputs("consumer captured no allocation stacks\n", stderr)
    exit(4)
}

guard snapshot.stacks.contains(where: { !$0.frames.isEmpty && $0.count > 0 && $0.bytes > 0 }) else {
    fputs("consumer captured only empty stack records\n", stderr)
    exit(5)
}

print(
    "CONSUMER_OK checksum=\(checksum) mallocs=\(statistics.mallocCount) "
        + "stacks=\(snapshot.stacks.count) dropped=\(snapshot.droppedAllocations)"
)
