// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Draws Compass's logo in place of every Chromium logo image upstream ships.
//
//   swift tools/branding/render.swift <repo> <chromium src> <out dir>
//
// Writes each image under <out dir> at the path it replaces below src/
// (tools/branding/make_patches.py turns them into patches). The art is
// assets/compass_glyph.svg (the two ribbons, for small and in-line logos)
// and assets/compass_card.svg (the app icon's card, for app icons).

import AppKit

let args = CommandLine.arguments
guard args.count == 4 else {
  FileHandle.standardError.write(
      "usage: render.swift <repo> <chromium src> <out dir>\n".data(using: .utf8)!)
  exit(2)
}
let repo = URL(fileURLWithPath: args[1])
let src = URL(fileURLWithPath: args[2])
let out = URL(fileURLWithPath: args[3])

func load(_ path: URL) -> NSImage {
  guard let image = NSImage(contentsOf: path) else {
    fatalError("can't load \(path.path)")
  }
  return image
}

let glyph = load(repo.appendingPathComponent("assets/compass_glyph.svg"))
let card = load(repo.appendingPathComponent("assets/compass_card.svg"))

func bitmap(_ width: Int, _ height: Int) -> NSBitmapImageRep {
  let rep = NSBitmapImageRep(
      bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height,
      bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
      colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
  rep.size = NSSize(width: width, height: height)
  return rep
}

func draw(into rep: NSBitmapImageRep, _ body: () -> Void) {
  NSGraphicsContext.saveGraphicsState()
  let context = NSGraphicsContext(bitmapImageRep: rep)!
  context.imageInterpolation = .high
  NSGraphicsContext.current = context
  body()
  context.flushGraphics()
  NSGraphicsContext.restoreGraphicsState()
}

// The glyph's bounds in its image, found from a large rendering.
func opaqueBounds(of image: NSImage) -> NSRect {
  let size = 1024
  let rep = bitmap(size, size)
  draw(into: rep) {
    image.draw(in: NSRect(x: 0, y: 0, width: size, height: size))
  }
  var minX = size, minY = size, maxX = -1, maxY = -1
  for y in 0..<size {
    for x in 0..<size where rep.colorAt(x: x, y: y)!.alphaComponent > 0.02 {
      minX = min(minX, x); maxX = max(maxX, x)
      minY = min(minY, y); maxY = max(maxY, y)
    }
  }
  // colorAt's y runs down; image coordinates run up.
  let scale = image.size.width / CGFloat(size)
  return NSRect(x: CGFloat(minX) * scale,
                y: CGFloat(size - 1 - maxY) * scale,
                width: CGFloat(maxX - minX + 1) * scale,
                height: CGFloat(maxY - minY + 1) * scale)
}

let glyphBounds = opaqueBounds(of: glyph)

// A square around the glyph with |padding| (a fraction of its side) around.
func glyphSquare(padding: CGFloat) -> NSRect {
  let side = max(glyphBounds.width, glyphBounds.height) * (1 + 2 * padding)
  return NSRect(x: glyphBounds.midX - side / 2, y: glyphBounds.midY - side / 2,
                width: side, height: side)
}

func drawGlyph(in rect: NSRect, padding: CGFloat = 0.04) {
  glyph.draw(in: rect, from: glyphSquare(padding: padding),
             operation: .sourceOver, fraction: 1)
}

func drawCard(in rect: NSRect) {
  card.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1)
}

func write(_ rep: NSBitmapImageRep, _ path: String) {
  let url = out.appendingPathComponent(path)
  try! FileManager.default.createDirectory(
      at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
  try! rep.representation(using: .png, properties: [:])!.write(to: url)
  print("wrote \(path)")
}

func square(_ px: Int, _ path: String, _ body: (NSRect) -> Void) {
  let rep = bitmap(px, px)
  draw(into: rep) { body(NSRect(x: 0, y: 0, width: px, height: px)) }
  write(rep, path)
}

// Turns every pixel black, keeping its alpha (macOS template images).
func monochrome(_ rep: NSBitmapImageRep) {
  for y in 0..<rep.pixelsHigh {
    for x in 0..<rep.pixelsWide {
      let alpha = rep.colorAt(x: x, y: y)!.alphaComponent
      rep.setColor(NSColor(deviceRed: 0, green: 0, blue: 0, alpha: alpha),
                   atX: x, y: y)
    }
  }
}

// App icons: the card, edge to edge, as Chromium's circle is.
let theme = "chrome/app/theme"
for px in [16, 24, 48, 64, 128, 256] {
  square(px, "\(theme)/chromium/product_logo_\(px).png") { drawCard(in: $0) }
}
for px in [24, 48, 64, 128, 256] {
  square(px, "\(theme)/chromium/linux/product_logo_\(px).png") {
    drawCard(in: $0)
  }
}
// Windows' .ico frames (tools/branding/make_patches.py packs them).
for px in [16, 20, 24, 32, 40, 48, 64, 128, 256] {
  square(px, "ico/chromium/\(px).png") { drawCard(in: $0) }
}
// Start menu tiles: the logo in the middle 37% of the tile.
for (px, name) in [(600, "Logo"), (176, "SmallLogo")] {
  square(px, "\(theme)/chromium/win/tiles/\(name).png") { rect in
    drawCard(in: rect.insetBy(dx: rect.width * 0.315, dy: rect.height * 0.315))
  }
}

// In-line logos (window and page icons, favicons of built-in pages): the
// glyph alone, which reads at 16 px where the card doesn't.
for (scale, dir) in [(1, "default_100_percent"), (2, "default_200_percent")] {
  for px in [16, 32] {
    square(px * scale, "\(theme)/\(dir)/chromium/product_logo_\(px).png") {
      drawGlyph(in: $0)
    }
  }
  square(16 * scale,
         "components/resources/\(dir)/chromium/favicon_product.png") {
    drawGlyph(in: $0)
  }
}
for px in [16, 32] {
  square(px, "\(theme)/default_100_percent/chromium/linux/product_logo_\(px).png") {
    drawGlyph(in: $0)
  }
}

// The menu bar icon (background mode): a template image.
do {
  let rep = bitmap(22, 22)
  draw(into: rep) { drawGlyph(in: NSRect(x: 1, y: 1, width: 20, height: 20)) }
  monochrome(rep)
  write(rep, "\(theme)/chromium/product_logo_22_mono.png")
}

// Logo and name, as upstream's "[logo] chromium": chrome://version and
// IDR_PRODUCT_LOGO_NAME_22. Same sizes as the images they replace.
func wordmark(width: Int, height: Int, scale: Int, white: Bool, path: String) {
  let rep = bitmap(width * scale, height * scale)
  draw(into: rep) {
    let h = CGFloat(height * scale)
    drawGlyph(in: NSRect(x: 0, y: 0, width: h, height: h), padding: 0.02)
    let color = white ? NSColor.white
                      : NSColor(srgbRed: 0x5F / 255.0, green: 0x63 / 255.0,
                                blue: 0x68 / 255.0, alpha: 1)
    let left = h * 1.22
    let room = CGFloat(width * scale) - left
    var size = h * 0.78
    var text: NSAttributedString
    repeat {
      text = NSAttributedString(string: "compass", attributes: [
        .font: NSFont.systemFont(ofSize: size, weight: .regular),
        .foregroundColor: color,
        .kern: size * -0.01,
      ])
      size -= 0.25
    } while text.size().width > room
    let bounds = text.boundingRect(with: .zero, options: [.usesFontLeading])
    let font = text.attribute(.font, at: 0, effectiveRange: nil) as! NSFont
    // Center the x-height on the glyph.
    let baseline = (h - font.xHeight) / 2
    text.draw(at: NSPoint(x: left, y: baseline + font.descender - bounds.minY
                                       + (bounds.minY - font.descender)))
  }
  write(rep, path)
}
for (scale, dir) in [(1, "default_100_percent"), (2, "default_200_percent")] {
  for white in [false, true] {
    let suffix = white ? "_white" : ""
    wordmark(width: 171, height: 32, scale: scale, white: white,
             path: "components/resources/\(dir)/chromium/product_logo\(suffix).png")
    wordmark(width: 97, height: 22, scale: scale, white: white,
             path: "\(theme)/\(dir)/chromium/product_logo_name_22\(suffix).png")
  }
}

// The intro (first run) illustration has the logo drawn on a card: the
// Compass card goes over it.
do {
  let path = "chrome/browser/resources/intro/images/refresh_showcase_illustration_chromium.png"
  let original = NSBitmapImageRep(data: try! Data(contentsOf: src.appendingPathComponent(path)))!
  let w = original.pixelsWide, h = original.pixelsHigh
  let rep = bitmap(w, h)
  draw(into: rep) {
    let full = NSRect(x: 0, y: 0, width: w, height: h)
    original.draw(in: full)
    // Upstream's logo: a 240 px circle centered at (400, 595) from the top.
    let side: CGFloat = 262
    let center = NSPoint(x: 400 * CGFloat(w) / 800,
                         y: CGFloat(h) - 595 * CGFloat(h) / 800)
    drawCard(in: NSRect(x: center.x - side / 2, y: center.y - side / 2,
                        width: side, height: side))
  }
  write(rep, path)
}
