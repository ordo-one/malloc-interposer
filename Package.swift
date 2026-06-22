// swift-tools-version: 5.10
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription


// The C interposer and the Swift wrapper ship in a single dynamic library so
// the Swift API reads the same counters the C interposer writes. If we
// split them across two dylibs in the same package, SwiftPM statically
// embeds the C target into the Swift dylib, and the two copies of the
// counters disconnect — preloading the C-only dylib increments one set
// while the Swift API reads the other. Bundling them avoids that whole
// class of footgun.
let package = Package(
    name: "malloc-interposer",
    // aligned_alloc (C11) requires macOS 10.15 / iOS 13; set the floor so the
    // interposer's aligned_alloc interception doesn't warn on the default target.
    platforms: [
        .macOS(.v10_15),
        .iOS(.v13),
    ],
    products: [
        // The dylib to preload (DYLD_INSERT_LIBRARIES / LD_PRELOAD) and to
        // link against from Swift code. Combines the C interposer with the
        // Swift API in one image.
        .library(
            name: "MallocInterposerSwift",
            type: .dynamic,
            targets: ["MallocInterposerSwift"]
        ),
    ],
    targets: [
        // C interposer — gets statically linked into the MallocInterposerSwift
        // dylib. Public headers under `include/` are also reachable by C
        // consumers via the standard SwiftPM module path.
        .target(
            name: "MallocInterposerC",
            path: "Sources/MallocInterposerC",
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedLibrary("dl", .when(platforms: [.linux])),
            ]
        ),
        .target(
            name: "MallocInterposerSwift",
            dependencies: ["MallocInterposerC"],
            path: "Sources/MallocInterposerSwift"
        ),
        // Standalone helper the death tests spawn in a clean process — the
        // guard-page fault only reproduces in a sparsely-mapped address space.
        .executableTarget(
            name: "InterposerCrashProbe",
            dependencies: ["MallocInterposerC"],
            path: "Tests/InterposerCrashProbe"
        ),
        // Unit tests exercise the C interposer's replacement_* functions and
        // pointer classifier directly, so they depend on the C target. The
        // dependency on the crash probe ensures it is built before tests run.
        .testTarget(
            name: "MallocInterposerTests",
            dependencies: ["MallocInterposerC", "InterposerCrashProbe"],
            path: "Tests/MallocInterposerTests"
        ),
    ]
)
