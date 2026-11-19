// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassEasels
import SwiftUI

/// One object of a board, drawn at `zoom` in a box of its size times the
/// zoom: text with its font and style, shapes as vectors, images fitted,
/// link and capture cards; content this version doesn't know as a grey box.
struct EaselObjectView: View {
  let object: EaselObject
  let palette: EaselPalette
  let zoom: CGFloat
  /// The box while it's being resized (board points); else the object's.
  var size: CGSize?

  private var box: CGSize { size ?? CGSize(width: object.width, height: object.height) }

  var body: some View {
    Group {
      if object.hasText {
        text
      } else if object.hasShape {
        EaselShapeView(shape: object.shape, original: CGSize(width: object.width, height: object.height), box: box,
          zoom: zoom, palette: palette)
      } else if object.hasImage {
        image(object.image.asset)
      } else if object.hasLink {
        card { link }
      } else if object.hasCapture {
        card { capture }
      } else {
        card { unknown }
      }
    }
    .frame(width: max(box.width * zoom, 1), height: max(box.height * zoom, 1))
  }

  static func fontSize(_ style: String) -> CGFloat {
    switch style {
    case "title": 32
    case "heading": 22
    case "caption": 12
    default: 16
    }
  }

  static func font(_ text: SyncPb_CompassEaselText, zoom: CGFloat) -> Font {
    let size = fontSize(text.style) * zoom
    let weight: Font.Weight = text.style == "title" || text.style == "heading" ? .semibold : .regular
    switch text.font {
    case "serif": return .system(size: size, weight: weight, design: .serif)
    case "mono": return .system(size: size, weight: weight, design: .monospaced)
    case "hand": return .custom("Noteworthy-Light", size: size)
    default: return .system(size: size, weight: weight)
    }
  }

  private var text: some View {
    Text(object.text.text)
      .font(Self.font(object.text, zoom: zoom))
      .foregroundStyle(EaselColor.color(object.text.color, on: palette))
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
  }

  @ViewBuilder private func image(_ sha: String) -> some View {
    if let image = EaselImageCache.image(sha) {
      Image(uiImage: image).resizable().scaledToFit()
    } else {
      // Not here yet (still syncing).
      Rectangle().fill(.gray.opacity(0.2))
        .overlay { Image(systemName: "photo").font(.system(size: 20 * zoom)).foregroundStyle(.secondary) }
    }
  }

  /// A card drawn at the board's size, then zoomed.
  private func card(@ViewBuilder _ content: () -> some View) -> some View {
    content()
      .frame(width: max(box.width, 1), height: max(box.height, 1))
      .background(palette.isDark ? Color(white: 0.18) : .white)
      .clipShape(RoundedRectangle(cornerRadius: 10))
      .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(palette.lines, lineWidth: 1))
      .shadow(color: .black.opacity(0.12), radius: 3, y: 1)
      .scaleEffect(zoom)
      .frame(width: max(box.width * zoom, 1), height: max(box.height * zoom, 1))
  }

  private var link: some View {
    let link = object.link
    let host = URL(string: link.url)?.host()?.replacingOccurrences(of: "www.", with: "") ?? link.url
    return VStack(alignment: .leading, spacing: 0) {
      if link.hasImageAsset, let image = EaselImageCache.image(link.imageAsset) {
        Image(uiImage: image).resizable().scaledToFill()
          .frame(maxWidth: .infinity, minHeight: 0, maxHeight: box.height * 0.55)
          .clipped()
      }
      VStack(alignment: .leading, spacing: 3) {
        Text(link.title.isEmpty ? host : link.title).font(.system(size: 14, weight: .semibold)).lineLimit(2)
        if !link.description_p.isEmpty {
          Text(link.description_p).font(.system(size: 12)).foregroundStyle(.secondary).lineLimit(2)
        }
        HStack(spacing: 5) {
          SiteIcon(url: link.url, size: 12)
          Text(host).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1)
        }
      }
      .padding(10)
      .frame(maxWidth: .infinity, alignment: .leading)
      Spacer(minLength: 0)
    }
    .foregroundStyle(palette.isDark ? .white : .black)
  }

  private var capture: some View {
    let c = object.capture
    return ZStack(alignment: .bottomLeading) {
      if let image = EaselImageCache.image(c.imageAsset) {
        Image(uiImage: image).resizable().scaledToFill()
          .frame(width: box.width, height: box.height, alignment: .topLeading)
          .clipped()
      } else {
        Color.gray.opacity(0.2)
      }
      HStack(spacing: 4) {
        if c.hasFaviconAsset, let icon = EaselImageCache.image(c.faviconAsset) {
          Image(uiImage: icon).resizable().frame(width: 11, height: 11)
        }
        Text(URL(string: c.url)?.host()?.replacingOccurrences(of: "www.", with: "") ?? c.url)
          .font(.system(size: 10, weight: .medium)).lineLimit(1)
      }
      .padding(.horizontal, 6)
      .padding(.vertical, 3)
      .background(.ultraThinMaterial, in: Capsule())
      .padding(6)
    }
  }

  private var unknown: some View {
    VStack(spacing: 6) {
      Image(systemName: "questionmark.square.dashed").font(.system(size: 22))
      Text("Made with a newer Compass").font(.system(size: 11)).multilineTextAlignment(.center)
    }
    .foregroundStyle(.secondary)
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(Color.gray.opacity(0.25))
  }
}

/// A shape: rectangle, ellipse, arrow or freehand stroke. Points are kept
/// for the box the object was saved with (`original`) and stretched to
/// `box` while it's resized.
struct EaselShapeView: View {
  let shape: SyncPb_CompassEaselShape
  let original: CGSize
  let box: CGSize
  let zoom: CGFloat
  let palette: EaselPalette

  var body: some View {
    Canvas { context, size in
      let color = EaselColor.color(shape.color, on: palette)
      let width = (shape.hasStrokeWidth ? shape.strokeWidth : 3) * zoom
      let sx = original.width > 0 ? box.width / original.width : 1
      let sy = original.height > 0 ? box.height / original.height : 1
      func point(_ i: Int) -> CGPoint {
        CGPoint(x: shape.points[i * 2] * sx * zoom, y: shape.points[i * 2 + 1] * sy * zoom)
      }
      let rect = CGRect(origin: .zero, size: size).insetBy(dx: width / 2, dy: width / 2)
      let style = StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round)
      switch EaselShapeKind(rawValue: shape.kind) {
      case .rectangle:
        let path = Path(roundedRect: rect, cornerRadius: 2 * zoom)
        if shape.filled { context.fill(path, with: .color(color)) } else { context.stroke(path, with: .color(color), style: style) }
      case .circle:
        let path = Path(ellipseIn: rect)
        if shape.filled { context.fill(path, with: .color(color)) } else { context.stroke(path, with: .color(color), style: style) }
      case .arrow where shape.points.count == 4:
        let a = point(0), b = point(1)
        var path = Path()
        path.move(to: a)
        path.addLine(to: b)
        // The head: two strokes back from the end.
        let angle = atan2(b.y - a.y, b.x - a.x)
        let head = max(10 * zoom, width * 3.5)
        for side in [-1.0, 1.0] {
          path.move(to: b)
          path.addLine(to: CGPoint(x: b.x - head * cos(angle + side * .pi / 7), y: b.y - head * sin(angle + side * .pi / 7)))
        }
        context.stroke(path, with: .color(color), style: style)
      case .freehand where shape.points.count >= 2:
        var path = Path()
        path.move(to: point(0))
        let count = shape.points.count / 2
        if count == 1 {
          path.addLine(to: point(0))
        } else {
          for i in 1..<count { path.addLine(to: point(i)) }
        }
        context.stroke(path, with: .color(color), style: style)
      default:
        // A shape this version doesn't know.
        context.fill(Path(rect), with: .color(.gray.opacity(0.25)))
      }
    }
  }
}
