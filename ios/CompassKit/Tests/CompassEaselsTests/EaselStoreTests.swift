// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassEasels
import CoreGraphics
import Foundation
import SwiftProtobuf
import Testing

/// The desktop's easel_store_unittest.cc, on the phone's store.
@MainActor
final class EaselStoreTests {
  let dir: URL
  var store: EaselStore

  init() {
    dir = FileManager.default.temporaryDirectory.appendingPathComponent("easels-\(UUID().uuidString)")
    store = EaselStore(directory: dir)
  }

  deinit { try? FileManager.default.removeItem(at: dir) }

  /// Writes everything and reads it back in a new store.
  func reopen() {
    store.flush()
    store = EaselStore(directory: dir)
  }

  func text(_ easel: String, _ text: String) -> EaselObject {
    var o = EaselObject()
    o.easelUuid = easel
    o.x = 10
    o.y = 20
    o.width = 100
    o.height = 40
    o.text.text = text
    return o
  }

  func board(_ title: String) throws -> EaselBoard { try store.createEasel(title: title).get() }

  @Test func boardsAndObjectsPersist() throws {
    let easel = try board("  Trip  ")
    #expect(easel.title == "Trip")
    let saved = try store.save(text(easel.uuid, "Pack")).get()
    #expect(!saved.uuid.isEmpty)
    #expect(saved.createTime > 0)

    reopen()
    #expect(store.easels.count == 1)
    #expect(store.easels.first?.title == "Trip")
    let objects = store.objects(of: easel.uuid)
    #expect(objects.count == 1)
    #expect(objects.first?.text.text == "Pack")
  }

  @Test func objectsNeedOneKindOfContentAndABoard() throws {
    let easel = try board("B")
    var none = text(easel.uuid, "x")
    none.clearText()
    #expect(throws: EaselError.self) { try self.store.save(none).get() }

    var two = text(easel.uuid, "x")
    two.shape.kind = EaselShapeKind.rectangle.rawValue
    #expect(throws: EaselError.self) { try self.store.save(two).get() }

    #expect(throws: EaselError.self) { try self.store.save(self.text("00000000-0000-4000-8000-000000000000", "x")).get() }

    var arrow = text(easel.uuid, "")
    arrow.clearText()
    arrow.shape.kind = EaselShapeKind.arrow.rawValue
    arrow.shape.points = [0, 0]
    #expect(throws: EaselError.self) { try self.store.save(arrow).get() }
    arrow.shape.points += [5, 5]
    #expect(throws: Never.self) { try self.store.save(arrow).get() }
  }

  @Test func longStrokesAreSimplified() throws {
    let easel = try board("B")
    var stroke = text(easel.uuid, "")
    stroke.clearText()
    stroke.shape.kind = EaselShapeKind.freehand.rawValue
    for i in 0..<10000 { stroke.shape.points += [Double(i), Double(-i)] }
    let saved = try store.save(stroke).get()
    #expect(saved.shape.points.count == EaselLimits.maxFreehandPoints * 2)
    // The ends stay.
    #expect(saved.shape.points[0] == 0)
    #expect(saved.shape.points[saved.shape.points.count - 2] == 9999)
  }

  @Test func longTextIsCut() throws {
    let easel = try board("B")
    let saved = try store.save(text(easel.uuid, String(repeating: "é", count: 20001))).get()
    #expect(saved.text.text.utf16.count == EaselLimits.maxTextChars)
  }

  @Test func imagesAreKeptOnceAndPersist() throws {
    let bytes = Data("png-bytes".utf8)
    let sha = try store.addAsset(bytes, mimeType: "image/png").get()
    #expect(sha == Easels.sha256Hex(bytes))
    #expect(try store.addAsset(bytes, mimeType: "image/png").get() == sha)
    #expect(throws: EaselError.self) { try self.store.addAsset(Data("x".utf8), mimeType: "image/gif").get() }
    #expect(throws: EaselError.self) {
      try self.store.addAsset(Data(count: EaselLimits.maxAssetBytes + 1), mimeType: "image/png").get()
    }

    let easel = try board("B")
    var image = text(easel.uuid, "")
    image.clearText()
    image.image.asset = sha
    _ = try store.save(image).get()

    reopen()
    #expect(store.assetData(sha) == bytes)
    #expect(store.asset(sha)?.uuid == Easels.assetUUID(sha256: sha))
    #expect(store.asset(sha)?.hasData == false)
  }

  @Test func anObjectNeedsItsImages() throws {
    let easel = try board("B")
    var image = text(easel.uuid, "")
    image.clearText()
    image.image.asset = Easels.sha256Hex(Data("missing".utf8))
    #expect(throws: EaselError.self) { try self.store.save(image).get() }
  }

  @Test func deletingABoardDeletesItsObjects() throws {
    let a = try board("A")
    let b = try board("B")
    _ = try store.save(text(a.uuid, "1")).get()
    _ = try store.save(text(b.uuid, "2")).get()
    #expect(store.deleteEasel(a.uuid))
    #expect(store.objects(of: a.uuid).isEmpty)
    #expect(store.objects(of: b.uuid).count == 1)
    #expect(!store.deleteEasel(a.uuid))
  }

  @Test func remoteChangesTheLaterEditWins() throws {
    let easel = try board("Mine")
    var older = easel
    older.title = "Older"
    older.updateTime = easel.updateTime - 1
    store.applyRemote(EaselEntities(easels: [older]), deleted: [])
    #expect(store.easel(easel.uuid)?.title == "Mine")

    var newer = easel
    newer.title = "Theirs"
    newer.updateTime = easel.updateTime + 1
    store.applyRemote(EaselEntities(easels: [newer]), deleted: [])
    #expect(store.easel(easel.uuid)?.title == "Theirs")

    store.applyRemote(EaselEntities(), deleted: [easel.uuid])
    #expect(store.easel(easel.uuid) == nil)
  }

  @Test func remoteImagesMustMatchTheirHash() {
    var good = EaselAsset()
    good.sha256 = Easels.sha256Hex(Data("abc".utf8))
    good.uuid = Easels.assetUUID(sha256: good.sha256)
    good.mimeType = "image/png"
    good.data = Data("abc".utf8)
    var bad = good
    bad.sha256 = Easels.sha256Hex(Data("other".utf8))
    bad.uuid = Easels.assetUUID(sha256: bad.sha256)
    store.applyRemote(EaselEntities(assets: [good, bad]), deleted: [])
    #expect(store.asset(good.sha256) != nil)
    #expect(store.asset(bad.sha256) == nil)
    // Deleted by its uuid, as sync names it.
    store.applyRemote(EaselEntities(), deleted: [good.uuid])
    #expect(store.asset(good.sha256) == nil)
  }

  @Test func unknownContentFromSyncIsKept() throws {
    let easel = try board("B")
    // A content kind from a newer version: field 99, a length-delimited
    // value, which this version keeps as an unknown field.
    var object = EaselObject()
    object.uuid = "11111111-1111-4111-8111-111111111111"
    object.easelUuid = easel.uuid
    var bytes: Data = try object.serializedBytes()
    bytes += Data([0x9a, 0x06, 0x02, 0x68, 0x69])  // field 99, wire type 2, "hi".
    let future = try EaselObject(serializedBytes: bytes)
    store.applyRemote(EaselEntities(objects: [future]), deleted: [])
    reopen()
    let kept = try #require(store.object(object.uuid))
    #expect(try kept.serializedBytes() as Data == bytes)
    // It can be moved: the unknown content stays.
    var moved = kept
    moved.x = 50
    let saved = try store.save(moved).get()
    #expect(!saved.unknownFields.data.isEmpty)
  }

  @Test func unusedImagesGoAfterThirtyDays() throws {
    let start = Date()
    store = EaselStore(directory: dir, now: { start })
    let used = try store.addAsset(Data("used".utf8), mimeType: "image/png").get()
    let unused = try store.addAsset(Data("unused".utf8), mimeType: "image/png").get()
    let easel = try board("B")
    var image = text(easel.uuid, "")
    image.clearText()
    image.image.asset = used
    _ = try store.save(image).get()
    store.flush()
    store = EaselStore(directory: dir, now: { start.addingTimeInterval(31 * 24 * 60 * 60) })
    #expect(store.asset(used) != nil)
    #expect(store.asset(unused) == nil)
  }

  @Test func urls() {
    let uuid = "0f8fad5b-d9cb-469f-a165-70867728950e"
    #expect(EaselURL.url(uuid) == "compass://easel/" + uuid)
    #expect(EaselURL.uuid(from: "compass://easel/" + uuid) == uuid)
    #expect(EaselURL.uuid(from: "chrome://easel/" + uuid + "?x#y") == uuid)
    #expect(EaselURL.uuid(from: "COMPASS://EASEL/0F8FAD5B-D9CB-469F-A165-70867728950E") == uuid)
    #expect(EaselURL.uuid(from: "compass://easel/") == nil)
    #expect(EaselURL.uuid(from: "https://easel/" + uuid) == nil)
    #expect(EaselURL.isList("compass://easel/") && EaselURL.isList("compass://easel"))
    #expect(!EaselURL.isList("compass://easel/" + uuid))
  }

  /// SHA-256("png-bytes") and its asset uuid, computed apart with Python's
  /// hashlib:
  ///   sha = sha256(b"png-bytes").hexdigest()
  ///   b = bytearray(sha256(("compass-easel-asset:" + sha).encode()).digest()[:16])
  ///   b[6] = b[6] & 0x0f | 0x50; b[8] = b[8] & 0x3f | 0x80
  @Test func assetUUIDVector() {
    let sha = Easels.sha256Hex(Data("png-bytes".utf8))
    #expect(sha == "ea80334363eed145dfeee51ebae7dc3f1cd7d0c7879f8bfd2070c061d3c33f56")
    #expect(Easels.assetUUID(sha256: sha) == "80c68774-1b4f-54b0-9378-46d8ef167d65")
  }

  @Test func theFileIsTheDesktopsRecords() throws {
    let easel = try board("B")
    store.flush()
    let data = try Data(contentsOf: dir.appendingPathComponent(EaselStore.fileName))
    var specifics = SyncPb_CompassSpacesSpecifics()
    specifics.easel = easel
    let record: Data = try specifics.serializedBytes()
    #expect(data == Data([UInt8(record.count)]) + record)
    // CompassSpacesSpecifics field 8 (length-delimited): tag 0x42.
    #expect(record.first == 0x42)
  }

  @Test func fitsImages() throws {
    // A 3000 x 1000 opaque image: scaled to 2048 wide, a JPEG.
    let opaque = try #require(Self.image(width: 3000, height: 1000, alpha: false))
    let jpeg = try EaselImages.fit(opaque).get()
    #expect(jpeg.mimeType == "image/jpeg" && jpeg.width == 2048 && jpeg.height == 682)
    #expect(jpeg.data.count <= EaselLimits.maxAssetBytes)
    // Transparent: a PNG.
    let clear = try #require(Self.image(width: 100, height: 50, alpha: true))
    let png = try EaselImages.fit(clear).get()
    #expect(png.mimeType == "image/png" && png.width == 100)
    // And back through the decoder.
    let again = try EaselImages.fit(png.data).get()
    #expect(again.width == 100 && again.height == 50)
    #expect(throws: EaselError.self) { try EaselImages.fit(Data("not an image".utf8)).get() }
  }

  static func image(width: Int, height: Int, alpha: Bool) -> CGImage? {
    let context = CGContext(
      data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
      space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    context?.setFillColor(CGColor(red: 0.2, green: 0.4, blue: 0.8, alpha: alpha ? 0.5 : 1))
    context?.fill(CGRect(x: 0, y: 0, width: width, height: height))
    return context?.makeImage()
  }

  @Test func linkPreviews() {
    let html = """
      <html><head><title>Plain &amp; simple</title>
      <meta property="og:title" content="The &quot;Real&quot; Title">
      <meta name='description' content='About it'>
      <meta content="/img/card.png" property="og:image" />
      </head></html>
      """
    let p = EaselLinkPreview.parse(html: html, baseURL: URL(string: "https://example.com/a/b")!)
    #expect(p.title == "The \"Real\" Title")
    #expect(p.description == "About it")
    #expect(p.imageURL?.absoluteString == "https://example.com/img/card.png")
    let bare = EaselLinkPreview.parse(html: "<title>\n Hi &#8212; there </title>", baseURL: URL(string: "https://e.com")!)
    #expect(bare.title == "Hi \u{2014} there" && bare.imageURL == nil)
  }
}
