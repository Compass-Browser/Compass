// swift-tools-version: 6.0
// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Compass for iOS's logic, kept out of the app target so it builds and tests
// on the Mac with `swift test` (docs/design/ios.md). Before the first build,
// run ios/scripts/fetch_boringssl.sh (`compass ios test` does it for you).

import PackageDescription

let package = Package(
  name: "CompassKit",
  platforms: [.iOS(.v17), .macOS(.v14)],
  products: [
    .library(name: "CompassCrypto", targets: ["CompassCrypto"]),
    .library(name: "CompassSync", targets: ["CompassSync"]),
    .library(name: "CompassModel", targets: ["CompassModel"]),
    // What the app shares with its extensions (App Group files, links).
    .library(name: "CompassShared", targets: ["CompassShared"]),
    // Claude chat's request, stream and conversation (docs/design/claude.md).
    .library(name: "CompassClaude", targets: ["CompassClaude"]),
    // Live folder plugins in JavaScriptCore (docs/design/live-folders.md).
    .library(name: "CompassLiveFolders", targets: ["CompassLiveFolders"]),
  ],
  dependencies: [
    // Must match the protoc-gen-swift that generated Sources/SyncProto
    // (see its README.md).
    .package(url: "https://github.com/apple/swift-protobuf.git", exact: "1.39.0"),
  ],
  targets: [
    // swift-crypto's prefixed BoringSSL, fetched by ios/scripts/fetch_boringssl.sh.
    .target(
      name: "CCryptoBoringSSL",
      exclude: ["hash.txt", "PrivacyInfo.xcprivacy", "SWIFT_CRYPTO_LICENSE.txt"],
      cSettings: [.unsafeFlags(["-Wno-shorten-64-to-32"])]
    ),
    .target(name: "CompassCrypto", dependencies: ["CCryptoBoringSSL"]),
    .target(
      name: "SyncProto",
      dependencies: [.product(name: "SwiftProtobuf", package: "swift-protobuf")],
      exclude: ["README.md"]
    ),
    .target(name: "CompassShared"),
    .target(name: "CompassModel", dependencies: ["CompassShared"]),
    .target(name: "CompassClaude", dependencies: ["CompassShared", "CompassModel"]),
    .target(name: "CompassLiveFolders", resources: [.copy("Resources/jsc_globals.js")]),
    .target(
      name: "CompassSync", dependencies: ["CompassCrypto", "SyncProto", "CompassModel", "CompassShared", "CompassClaude"]),
    .testTarget(name: "CompassCryptoTests", dependencies: ["CompassCrypto"]),
    .testTarget(name: "CompassSyncTests", dependencies: ["CompassSync"]),
    .testTarget(name: "CompassModelTests", dependencies: ["CompassModel"]),
    .testTarget(name: "CompassSharedTests", dependencies: ["CompassShared"]),
    .testTarget(name: "CompassClaudeTests", dependencies: ["CompassClaude", "CompassModel"]),
    .testTarget(name: "CompassLiveFoldersTests", dependencies: ["CompassLiveFolders"]),
  ],
  // BoringSSL's C++ needs C++17 (swift-crypto sets the same).
  cxxLanguageStandard: .cxx17
)
