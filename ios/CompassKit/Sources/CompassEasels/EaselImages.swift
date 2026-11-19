// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// An image ready for `EaselStore.addAsset`.
public struct EaselPreparedImage: Sendable, Equatable {
  public var data: Data
  public var mimeType: String
  public var width: Int
  public var height: Int
}

/// Images made fit for a board, as the desktop's easel_images.cc: at most
/// 2048 px a side and 512 KiB, JPEG at quality 85 (lower, then smaller,
/// until it fits), or PNG when it has transparency. Pure ImageIO, so it
/// runs off the main thread and in tests on the Mac.
public enum EaselImages {
  static let maxInputBytes = 32 * 1024 * 1024
  static let jpegQuality = 85

  /// Decodes `data` (any format ImageIO reads, turned upright) and fits it.
  public static func fit(_ data: Data) -> Result<EaselPreparedImage, EaselError> {
    guard !data.isEmpty, data.count <= maxInputBytes else { return .failure(EaselError("The image is too large.")) }
    guard let source = CGImageSourceCreateWithData(data as CFData, nil),
      let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
      let w = props[kCGImagePropertyPixelWidth] as? Int, let h = props[kCGImagePropertyPixelHeight] as? Int, w > 0, h > 0
    else { return .failure(EaselError("The image couldn't be read.")) }
    // Decoded at most at the largest size a board keeps, upright.
    let options: [CFString: Any] = [
      kCGImageSourceCreateThumbnailFromImageAlways: true,
      kCGImageSourceCreateThumbnailWithTransform: true,
      kCGImageSourceThumbnailMaxPixelSize: min(max(w, h), EaselLimits.maxAssetPixels),
      kCGImageSourceShouldCacheImmediately: true,
    ]
    guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else {
      return .failure(EaselError("The image couldn't be read."))
    }
    return fit(image)
  }

  /// Fits a decoded image (a snapshot of a page).
  public static func fit(_ input: CGImage) -> Result<EaselPreparedImage, EaselError> {
    guard input.width > 0, input.height > 0, let bitmap = rgba(input) else {
      return .failure(EaselError("The image couldn't be read."))
    }
    let png = hasTransparency(bitmap)
    var side = EaselLimits.maxAssetPixels
    var quality = jpegQuality
    // Smaller until it fits: a PNG gets smaller, a JPEG first loses quality.
    for _ in 0..<12 {
      guard let scaled = scaled(bitmap, toFit: side), let encoded = encode(scaled, png: png, quality: quality) else {
        return .failure(EaselError("The image couldn't be encoded."))
      }
      if encoded.count <= EaselLimits.maxAssetBytes {
        return .success(
          EaselPreparedImage(
            data: encoded, mimeType: png ? "image/png" : "image/jpeg", width: scaled.width, height: scaled.height))
      }
      if !png && quality > 60 {
        quality -= 10
      } else {
        side = max(64, side * 3 / 4)
      }
    }
    return .failure(EaselError("The image is too large."))
  }

  /// The image drawn into 8-bit RGBA (premultiplied), sRGB.
  static func rgba(_ image: CGImage) -> CGImage? {
    guard
      let context = CGContext(
        data: nil, width: image.width, height: image.height, bitsPerComponent: 8, bytesPerRow: 0,
        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    else { return nil }
    context.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
    return context.makeImage()
  }

  /// Whether any pixel isn't fully opaque.
  static func hasTransparency(_ image: CGImage) -> Bool {
    switch image.alphaInfo {
    case .none, .noneSkipFirst, .noneSkipLast: return false
    default: break
    }
    guard let data = image.dataProvider?.data, let bytes = CFDataGetBytePtr(data) else { return false }
    let length = CFDataGetLength(data)
    let row = image.bytesPerRow
    // RGBA, alpha last.
    for y in 0..<image.height {
      var i = y * row + 3
      for _ in 0..<image.width {
        if i < length, bytes[i] != 0xff { return true }
        i += 4
      }
    }
    return false
  }

  static func scaled(_ image: CGImage, toFit side: Int) -> CGImage? {
    let longest = max(image.width, image.height)
    guard longest > side else { return image }
    let scale = Double(side) / Double(longest)
    let width = max(1, Int(Double(image.width) * scale))
    let height = max(1, Int(Double(image.height) * scale))
    guard
      let context = CGContext(
        data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    else { return nil }
    context.interpolationQuality = .high
    context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
    return context.makeImage()
  }

  static func encode(_ image: CGImage, png: Bool, quality: Int) -> Data? {
    let out = NSMutableData()
    let type = (png ? UTType.png : UTType.jpeg).identifier as CFString
    guard let destination = CGImageDestinationCreateWithData(out, type, 1, nil) else { return nil }
    var properties: [CFString: Any] = [:]
    if !png { properties[kCGImageDestinationLossyCompressionQuality] = Double(quality) / 100 }
    CGImageDestinationAddImage(destination, image, properties as CFDictionary)
    guard CGImageDestinationFinalize(destination) else { return nil }
    return out as Data
  }
}
