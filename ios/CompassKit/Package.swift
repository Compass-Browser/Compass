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
  // User-facing strings: String(localized:bundle: .module) and each target's
  // Localizable.xcstrings (English and Italian).
  defaultLocalization: "en",
  platforms: [.iOS(.v17), .macOS(.v14)],
  products: [
    .library(name: "CompassCrypto", targets: ["CompassCrypto"]),
    .library(name: "CompassSync", targets: ["CompassSync"]),
    .library(name: "CompassModel", targets: ["CompassModel"]),
    // What the app shares with its extensions (App Group files, links).
    .library(name: "CompassShared", targets: ["CompassShared"]),
    // Claude chat's request, stream and conversation (docs/design/claude.md).
    .library(name: "CompassClaude", targets: ["CompassClaude"]),
    // Apple Intelligence for the sidebar's small requests, shared with the
    // Mac's compass-on-device helper (tools/on_device/).
    .library(name: "CompassOnDevice", targets: ["CompassOnDevice"]),
    // Live folder plugins in JavaScriptCore (docs/design/live-folders.md).
    .library(name: "CompassLiveFolders", targets: ["CompassLiveFolders"]),
    // Per-site CSS and JavaScript (docs/design/boosts.md).
    .library(name: "CompassBoosts", targets: ["CompassBoosts"]),
    // Canvas boards (docs/design/easels.md).
    .library(name: "CompassEasels", targets: ["CompassEasels"]),
    // Shared spaces, folders and easels: room crypto and invite links
    // (docs/design/sync.md, "Rooms: crypto and links").
    .library(name: "CompassRooms", targets: ["CompassRooms"]),
    // Page translation's rules: languages, HTML for text-only engines,
    // the user's choices (docs/design/translation.md, "iOS").
    .library(name: "CompassTranslate", targets: ["CompassTranslate"]),
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
    .target(name: "CompassShared", resources: [.process("Localizable.xcstrings")]),
    .target(name: "CompassModel", dependencies: ["CompassShared"], resources: [.process("Localizable.xcstrings")]),
    // No dependencies: the Mac's helper builds it with swiftc alone
    // (tools/on_device/BUILD.gn).
    .target(name: "CompassOnDevice"),
    .target(
      name: "CompassClaude", dependencies: ["CompassShared", "CompassModel", "CompassOnDevice"],
      resources: [.process("Localizable.xcstrings")]),
    .target(
      name: "CompassLiveFolders",
      resources: [.copy("Resources/jsc_globals.js"), .process("Localizable.xcstrings")]),
    .target(name: "CompassTranslate"),
    .target(name: "CompassBoosts", resources: [.process("Localizable.xcstrings")]),
    .target(name: "CompassEasels", dependencies: ["SyncProto"], resources: [.process("Localizable.xcstrings")]),
    // Sharing with other people: the client, the room crypto, and the sync
    // of shared spaces, folders and easels into the sidebar.
    .target(
      name: "CompassRooms",
      dependencies: ["CompassCrypto", "CompassModel", "CompassEasels", "CompassSync", "SyncProto"],
      resources: [.process("Localizable.xcstrings")]),
    .target(
      name: "CompassSync",
      dependencies: ["CompassCrypto", "SyncProto", "CompassModel", "CompassShared", "CompassClaude", "CompassBoosts", "CompassEasels"],
      resources: [.process("Localizable.xcstrings")]),
    .testTarget(name: "CompassCryptoTests", dependencies: ["CompassCrypto"]),
    .testTarget(name: "CompassSyncTests", dependencies: ["CompassSync"]),
    .testTarget(name: "CompassModelTests", dependencies: ["CompassModel"]),
    .testTarget(name: "CompassSharedTests", dependencies: ["CompassShared"]),
    .testTarget(name: "CompassClaudeTests", dependencies: ["CompassClaude", "CompassModel", "CompassOnDevice"]),
    .testTarget(name: "CompassOnDeviceTests", dependencies: ["CompassOnDevice"]),
    .testTarget(name: "CompassLiveFoldersTests", dependencies: ["CompassLiveFolders"]),
    .testTarget(name: "CompassBoostsTests", dependencies: ["CompassBoosts"]),
    .testTarget(name: "CompassTranslateTests", dependencies: ["CompassTranslate"]),
    .testTarget(name: "CompassEaselsTests", dependencies: ["CompassEasels"]),
    .testTarget(
      name: "CompassRoomsTests",
      dependencies: ["CompassRooms", "CompassCrypto", "CompassModel", "CompassEasels", "CompassSync"]),
  ],
  // BoringSSL's C++ needs C++17 (swift-crypto sets the same).
  cxxLanguageStandard: .cxx17
)
