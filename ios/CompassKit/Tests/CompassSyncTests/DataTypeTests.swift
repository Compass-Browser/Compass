// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassSync
import Foundation
import SwiftProtobuf
import SyncProto
import Testing

struct DataTypeTests {
  // An empty specifics serializes to its field's tag and a zero length, so
  // the field number read back from the wire must be the type's ID.
  @Test(arguments: DataType.allCases)
  func fieldNumberMatches(type: DataType) throws {
    let bytes = try type.emptySpecifics.serializedBytes() as [UInt8]
    var value: UInt64 = 0
    var shift: UInt64 = 0
    for byte in bytes {
      value |= UInt64(byte & 0x7f) << shift
      shift += 7
      if byte & 0x80 == 0 { break }
    }
    #expect(Int32(value >> 3) == type.rawValue)
  }

  // Chromium's ClientTagHashTest.ShouldGenerateFromUnhashed.
  @Test func clientTagHashMatchesChromium() {
    var preference = SyncPb_EntitySpecifics()
    preference.preference = .init()
    var autofill = SyncPb_EntitySpecifics()
    autofill.autofill = .init()
    #expect(ClientTagHash.make(emptySpecifics: preference, tag: "tag1") == "iNFQtRFQb+IZcn1kKUJEZDDkLs4=")
    #expect(ClientTagHash.make(emptySpecifics: autofill, tag: "tag1") == "gO1cPZQXaM73sHOvSA+tKCKFs58=")
    #expect(ClientTagHash.make(emptySpecifics: preference, tag: "tag2") == "XYxkF7bhS4eItStFgiOIAU23swI=")
    #expect(ClientTagHash.make(emptySpecifics: autofill, tag: "tag2") == "GFiWzo5NGhjLlN+OyCfhy28DJTQ=")
  }

  @Test func commandURL() {
    #expect(HTTPSyncConnection.commandURL(URL(string: "https://h/v2")!, cacheGUID: "a+b/c=").absoluteString
      == "https://h/v2/command/?client=Compass%20iOS&client_id=a+b/c%3D")
  }

  @Test func compassNodeRoundTrips() throws {
    var node = SyncPb_CompassNode()
    node.uuid = "2b5c3e1a-0000-4000-8000-000000000001"
    node.section = 1
    node.type = 1
    node.url = "https://example.com/"
    var specifics = SyncPb_EntitySpecifics()
    specifics.compassSpaces.node = node
    let decoded = try SyncPb_EntitySpecifics(serializedBytes: try specifics.serializedBytes() as [UInt8])
    #expect(decoded.compassSpaces.node.url == "https://example.com/")
  }
}
