// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
@testable import CompassSync
import Foundation
import SyncProto
import Testing

/// Space themes on the wire, as the desktop's compass_spaces_sync_bridge.cc
/// reads and writes them.
@MainActor
struct SpaceThemeSyncTests {
  @Test func paletteColorsSaveAnIndexOnly() {
    var s = SyncPb_CompassSpace()
    s.setTheme(SpaceTheme(.pink))
    #expect(s.color == SpaceColor.pink.rawValue && !s.hasColorRgb && s.gradient.isEmpty)
    #expect(!s.hasIntensity && !s.hasGrain && !s.hasAppearance)
    s.setTheme(SpaceTheme(color: nil))
    #expect(s.color == SpaceColor.grey.rawValue && !s.hasColorRgb)
  }

  @Test func anyColorSavesItsClosestIndexToo() {
    var s = SyncPb_CompassSpace()
    let rgb = RGB(10, 200, 90)
    s.setTheme(SpaceTheme(color: rgb))
    #expect(s.colorRgb == rgb.hex && s.color == SpaceColor.green.rawValue)
    #expect(s.theme(over: .blue).color == rgb)
  }

  /// A client that only knows the palette picked another color since.
  @Test func aStaleRgbIsIgnored() {
    var s = SyncPb_CompassSpace()
    s.setTheme(SpaceTheme(color: RGB(10, 200, 90)))
    s.color = SpaceColor.red.rawValue
    #expect(s.theme(over: .blue).color == SpaceColor.red.rgb)
  }

  @Test func roundTripsAGradientAndModifiers() {
    let theme = SpaceTheme(
      color: RGB(139, 92, 246), gradient: [RGB(236, 72, 153), RGB(1, 2, 3)], intensity: 0.8, grain: 0.5,
      appearance: .dark)
    var s = SyncPb_CompassSpace()
    s.setTheme(theme)
    #expect(s.gradient.map(\.hasPalette) == [true, false] && s.gradient[1].rgb == 0x010203)
    #expect(s.intensity == 0.8 && s.grain == 0.5 && s.appearance == 3)
    #expect(s.theme(over: .blue) == theme)
    // Back to the defaults: left out again.
    s.setTheme(SpaceTheme(.purple))
    #expect(s.gradient.isEmpty && !s.hasIntensity && !s.hasGrain && !s.hasAppearance)
  }

  /// An equivalent encoding isn't rewritten.
  @Test func equivalentEncodingsStay() {
    var s = SyncPb_CompassSpace()
    var c = SyncPb_CompassColor()
    c.rgb = SpaceColor.pink.rgb!.hex  // a palette color sent as rgb
    s.color = SpaceColor.blue.rawValue
    s.gradient = [c]
    s.appearance = 9  // unknown: auto
    let before = s
    s.setTheme(s.theme(over: .blue))
    #expect(s == before)
  }

  @Test func unknownColorsKeepOurs() {
    var s = SyncPb_CompassSpace()
    s.color = 42
    #expect(s.theme(over: SpaceTheme(.green)).color == SpaceColor.green.rgb)
  }

  @Test func greyHasNoGradientAndValuesStayInRange() {
    var theme = SpaceTheme(color: RGB(1, 2, 3), gradient: [RGB(4, 5, 6), RGB(7, 8, 9), RGB(1, 1, 1)])
    #expect(theme.gradient.count == 2)
    theme.intensity = 4
    theme.grain = .nan
    #expect(theme.intensity == 1 && theme.grain == 0)
    theme.color = nil
    #expect(theme.gradient.isEmpty && theme.stops.isEmpty)
  }

  @Test func stopsAreEvenlySpaced() {
    let two = SpaceTheme(color: RGB(0, 0, 0), gradient: [RGB(200, 100, 0)])
    #expect(two.stops == [RGB(0, 0, 0), RGB(100, 50, 0), RGB(200, 100, 0)])
    #expect(SpaceTheme(.blue).stops == Array(repeating: SpaceColor.blue.rgb!, count: 3))
  }

  @Test func savedThemesLoadBack() throws {
    let store = SpaceStore()
    let space = store.addSpace(name: "A")
    let theme = SpaceTheme(color: RGB(12, 34, 56), gradient: [RGB(7, 8, 9)], intensity: 0.6, grain: 0.2, appearance: .light)
    store.setTheme(space, theme)
    let grey = store.addSpace(name: "B", theme: SpaceTheme(color: nil))
    let copy = SpaceStore()
    try copy.load(snapshot: store.encodedSnapshot())
    #expect(copy.space(id: space.id)?.theme == theme)
    #expect(copy.space(id: grey.id)?.theme.color == nil)
  }

  @Test func remoteThemesApplyAndAreNotCommittedBack() async {
    let store = SpaceStore()
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let bridge = CompassSpacesBridge(store: store, engine: engine, mirrorURL: nil)
    _ = bridge.mergeWithChain()
    let id = UUID().uuidString.lowercased()
    var specifics = SyncPb_CompassSpacesSpecifics()
    specifics.space.uuid = id
    specifics.space.name = "Arc"
    specifics.space.setTheme(SpaceTheme(color: RGB(10, 200, 90), gradient: [RGB(1, 2, 3)], grain: 0.3))
    var e = SyncPb_EntitySpecifics()
    e.compassSpaces = specifics
    bridge.applyRemote([
      RemoteChange(
        clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: id), specifics: e, serverID: id, version: 1,
        mtime: 0, ctime: 0, originatorCacheGUID: "mac")
    ])
    let space = store.space(id: id)!
    #expect(space.theme.color == RGB(10, 200, 90) && space.theme.gradient == [RGB(1, 2, 3)] && space.theme.grain == 0.3)
    await bridge.flush()
    #expect(await bridge.engine.hasPendingChanges == false)
    // A change here goes out.
    store.setTheme(space, SpaceTheme(.yellow))
    #expect(bridge.desired(space).space.color == SpaceColor.yellow.rawValue)
    #expect(!bridge.desired(space).space.hasColorRgb && bridge.desired(space).space.gradient.isEmpty)
  }
}
