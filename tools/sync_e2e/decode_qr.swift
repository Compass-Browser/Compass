// Prints the text of every QR code in an image, one per line (CoreImage's
// detector, no extra tools). Used to check the "Add a device" QR on
// chrome://compass-sync from a screenshot: swift tools/sync_e2e/decode_qr.swift shot.png
import CoreImage
import Foundation

guard CommandLine.arguments.count == 2,
  let image = CIImage(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
else {
  FileHandle.standardError.write("usage: decode_qr.swift IMAGE\n".data(using: .utf8)!)
  exit(2)
}
let detector = CIDetector(
  ofType: CIDetectorTypeQRCode, context: nil,
  options: [CIDetectorAccuracy: CIDetectorAccuracyHigh])!
let found = detector.features(in: image).compactMap { ($0 as? CIQRCodeFeature)?.messageString }
for text in found { print(text) }
exit(found.isEmpty ? 1 : 0)
