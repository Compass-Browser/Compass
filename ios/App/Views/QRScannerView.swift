// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AVFoundation
import SwiftUI

/// Scans one QR code with the back camera.
struct QRScannerView: UIViewControllerRepresentable {
  let onCode: (String) -> Void

  func makeUIViewController(context: Context) -> ScannerController {
    let controller = ScannerController()
    controller.onCode = onCode
    return controller
  }

  func updateUIViewController(_ controller: ScannerController, context: Context) {}

  final class ScannerController: UIViewController, AVCaptureMetadataOutputObjectsDelegate {
    var onCode: ((String) -> Void)?
    private let session = AVCaptureSession()
    private var done = false

    override func viewDidLoad() {
      super.viewDidLoad()
      view.backgroundColor = .black
      guard let device = AVCaptureDevice.default(for: .video), let input = try? AVCaptureDeviceInput(device: device),
        session.canAddInput(input)
      else {
        let label = UILabel()
        label.text = "No camera available."
        label.textColor = .white
        label.frame = view.bounds
        label.textAlignment = .center
        view.addSubview(label)
        return
      }
      session.addInput(input)
      let output = AVCaptureMetadataOutput()
      guard session.canAddOutput(output) else { return }
      session.addOutput(output)
      output.setMetadataObjectsDelegate(self, queue: .main)
      output.metadataObjectTypes = [.qr]
      let preview = AVCaptureVideoPreviewLayer(session: session)
      preview.videoGravity = .resizeAspectFill
      preview.frame = view.layer.bounds
      view.layer.addSublayer(preview)
      DispatchQueue.global(qos: .userInitiated).async { [session] in session.startRunning() }
    }

    override func viewWillDisappear(_ animated: Bool) {
      super.viewWillDisappear(animated)
      session.stopRunning()
    }

    nonisolated func metadataOutput(
      _ output: AVCaptureMetadataOutput, didOutput objects: [AVMetadataObject], from connection: AVCaptureConnection
    ) {
      let code = (objects.first as? AVMetadataMachineReadableCodeObject)?.stringValue
      MainActor.assumeIsolated {
        guard !done, let code else { return }
        done = true
        onCode?(code)
      }
    }
  }
}
