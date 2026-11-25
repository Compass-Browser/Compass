// Records a USB-connected iPhone's screen, like QuickTime's New Movie Recording.
// usage: record_iphone <out.mov> [device-name-substring]
// Stops on SIGINT/SIGTERM or when <out.mov>.stop exists.
import AVFoundation
import CoreMediaIO
import Foundation

var prop = CMIOObjectPropertyAddress(
  mSelector: CMIOObjectPropertySelector(kCMIOHardwarePropertyAllowScreenCaptureDevices),
  mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
  mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
var allow: UInt32 = 1
CMIOObjectSetPropertyData(CMIOObjectID(kCMIOObjectSystemObject), &prop, 0, nil, UInt32(MemoryLayout<UInt32>.size), &allow)

let args = CommandLine.arguments
guard args.count >= 2 else { print("usage: record_iphone out.mov [name]"); exit(2) }
let out = URL(fileURLWithPath: args[1])
let want = args.count > 2 ? args[2] : "iPhone"
try? FileManager.default.removeItem(at: out)

func findDevice() -> AVCaptureDevice? {
  AVCaptureDevice.DiscoverySession(deviceTypes: [.external], mediaType: nil, position: .unspecified)
    .devices.first { $0.modelID == "iOS Device" && $0.localizedName.localizedCaseInsensitiveContains(want) }
}
var device: AVCaptureDevice?
for _ in 0..<40 { // devices appear a moment after allowing screen capture
  device = findDevice(); if device != nil { break }
  RunLoop.main.run(until: Date().addingTimeInterval(0.5))
}
guard let device else {
  print("no device matching \(want); seen:", AVCaptureDevice.DiscoverySession(deviceTypes: [.external], mediaType: nil, position: .unspecified).devices.map(\.localizedName))
  exit(1)
}
print("recording", device.localizedName)

final class Delegate: NSObject, AVCaptureFileOutputRecordingDelegate {
  func fileOutput(_ o: AVCaptureFileOutput, didFinishRecordingTo url: URL, from c: [AVCaptureConnection], error: Error?) {
    if let error { print("finished with:", error.localizedDescription) }
    print("saved", url.path); exit(0)
  }
}
let session = AVCaptureSession()
do { session.addInput(try AVCaptureDeviceInput(device: device)) } catch { print("input:", error); exit(1) }
let output = AVCaptureMovieFileOutput()
session.addOutput(output)
session.startRunning()
let delegate = Delegate()
RunLoop.main.run(until: Date().addingTimeInterval(1))
output.startRecording(to: out, recordingDelegate: delegate)

var sources: [DispatchSourceSignal] = []
for sig in [SIGINT, SIGTERM] {
  signal(sig, SIG_IGN)
  let s = DispatchSource.makeSignalSource(signal: sig, queue: .main)
  s.setEventHandler { output.stopRecording() }
  s.resume()
  sources.append(s)
}
Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
  if FileManager.default.fileExists(atPath: out.path + ".stop") { output.stopRecording() }
}
RunLoop.main.run()
