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
            cSettings: [
                // Force aggressive inlining of the malloc hot path. At the
                // default optimisation level clang outlines the counting
                // branch into a cold helper, eating ~5 ns per malloc in
                // extra call overhead. The `flatten` attribute on the
                // replacement_* entry points keeps the inlined body intact.
                .unsafeFlags([
                    "-O3",
                    "-fno-stack-protector",
                    // Pass-through to LLVM: disable the late machine
                    // outliner, which otherwise re-extracts shared code
                    // blocks back into calls.
                    "-mllvm", "-enable-machine-outliner=never",
                ], .when(configuration: .release)),
            ],
            linkerSettings: [
                .linkedLibrary("dl", .when(platforms: [.linux])),
            ]
        ),
        .target(
            name: "MallocInterposerSwift",
            dependencies: ["MallocInterposerC"],
            path: "Sources/MallocInterposerSwift"
        ),
    ]
)
