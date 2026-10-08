// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "AllocationStacksConsumer",
    platforms: [.macOS(.v13)],
    dependencies: [
        .package(path: "../..")
    ],
    targets: [
        .executableTarget(
            name: "AllocationStacksConsumer",
            dependencies: [
                .product(name: "MallocInterposerSwift", package: "malloc-interposer")
            ]
        ),
    ]
)
