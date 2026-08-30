// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "Ammonia",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "Ammonia", targets: ["Ammonia"]),
    ],
    targets: [
        .executableTarget(
            name: "Ammonia",
            path: "Sources/Ammonia",
            resources: [.process("Resources")]
        ),
    ]
)
