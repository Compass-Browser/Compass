// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Observation
import WebKit

/// Files downloaded from pages, saved in Documents/Downloads, which the
/// Files app shows under Compass Browser (UIFileSharingEnabled). The list is
/// the downloads running now plus the files in the folder, newest first.
@MainActor @Observable
final class Downloads: NSObject {
  struct Item: Identifiable, Equatable {
    let id: UUID
    var name: String
    var file: URL?
    /// 0...1 while running; nil when the size isn't known.
    var progress: Double?
    var bytes: Int64 = 0
    var date = Date()
    var failed: String?
    var running: Bool { file == nil && failed == nil }
  }

  let folder: URL
  private(set) var items: [Item] = []
  /// Bumped when a download starts: the browser flies an arrow into the
  /// menu button.
  private(set) var started = 0
  @ObservationIgnored private var running: [ObjectIdentifier: UUID] = [:]
  @ObservationIgnored private var progress: [UUID: NSKeyValueObservation] = [:]
  @ObservationIgnored private var pending: [ObjectIdentifier: URL] = [:]
  /// Running downloads by item, for Cancel.
  @ObservationIgnored private var tasks: [UUID: WKDownload] = [:]
  /// The Lock Screen's download activities (App/System/DownloadActivity.swift).
  @ObservationIgnored var activities: DownloadActivities?

  override init() {
    folder = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Downloads", isDirectory: true)
    try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    super.init()
    reload()
  }

  /// The fraction done over every running download, for the menu button's
  /// ring; nil when none runs.
  var overallProgress: Double? {
    let active = items.filter(\.running)
    guard !active.isEmpty else { return nil }
    return active.map { $0.progress ?? 0 }.reduce(0, +) / Double(active.count)
  }

  /// A download a tab started.
  func track(_ download: WKDownload) {
    let id = UUID()
    download.delegate = self
    running[ObjectIdentifier(download)] = id
    tasks[id] = download
    items.insert(Item(id: id, name: download.originalRequest?.url?.lastPathComponent ?? "Download", progress: 0), at: 0)
    started += 1
    if let item = items.first { activities?.update(item) }
    progress[id] = download.progress.observe(\.fractionCompleted) { [weak self] p, _ in
      let fraction = p.totalUnitCount > 0 ? p.fractionCompleted : nil
      let bytes = p.completedUnitCount
      Task { @MainActor in self?.update(id) { $0.progress = fraction; $0.bytes = bytes } }
    }
  }

  /// Stops a running download (Cancel on its Lock Screen activity).
  func cancel(_ id: UUID) {
    guard let download = tasks[id] else { return }
    download.cancel { _ in }
    let key = ObjectIdentifier(download)
    running[key] = nil
    pending[key] = nil
    progress[id] = nil
    tasks[id] = nil
    update(id) { $0.failed = "Cancelled" }
  }

  func delete(_ item: Item) {
    if let file = item.file { try? FileManager.default.removeItem(at: file) }
    items.removeAll { $0.id == item.id }
  }

  /// The finished files, from the folder (also what the Files app changed).
  func reload() {
    let keys: [URLResourceKey] = [.fileSizeKey, .contentModificationDateKey]
    let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: keys)) ?? []
    let finished = files.filter { !$0.lastPathComponent.hasPrefix(".") }.map { url in
      let values = try? url.resourceValues(forKeys: Set(keys))
      return Item(
        id: UUID(), name: url.lastPathComponent, file: url, progress: 1,
        bytes: Int64(values?.fileSize ?? 0), date: values?.contentModificationDate ?? .distantPast)
    }
    items = items.filter { $0.file == nil } + finished.sorted { $0.date > $1.date }
  }

  private func update(_ id: UUID, _ change: (inout Item) -> Void) {
    guard let i = items.firstIndex(where: { $0.id == id }) else { return }
    change(&items[i])
    activities?.update(items[i])
  }

  /// A name that isn't taken: "report.pdf", then "report 2.pdf".
  private func destination(_ name: String) -> URL {
    let clean = name.replacingOccurrences(of: "/", with: "-")
    let base = (clean as NSString).deletingPathExtension
    let ext = (clean as NSString).pathExtension
    var url = folder.appendingPathComponent(clean.isEmpty ? "Download" : clean)
    var n = 2
    while FileManager.default.fileExists(atPath: url.path) {
      url = folder.appendingPathComponent(ext.isEmpty ? "\(base) \(n)" : "\(base) \(n).\(ext)")
      n += 1
    }
    return url
  }
}

extension Downloads: WKDownloadDelegate {
  nonisolated func download(
    _ download: WKDownload, decideDestinationUsing response: URLResponse, suggestedFilename: String
  ) async -> URL? {
    await MainActor.run {
      let url = destination(suggestedFilename)
      if let id = running[ObjectIdentifier(download)] {
        update(id) { $0.name = url.lastPathComponent; $0.date = Date() }
      }
      // Remembered for downloadDidFinish.
      pending[ObjectIdentifier(download)] = url
      return url
    }
  }

  nonisolated func downloadDidFinish(_ download: WKDownload) {
    MainActor.assumeIsolated {
      let key = ObjectIdentifier(download)
      guard let id = running.removeValue(forKey: key) else { return }
      let file = pending.removeValue(forKey: key)
      progress[id] = nil
      tasks[id] = nil
      update(id) {
        $0.file = file
        $0.progress = 1
        if let file, let size = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize { $0.bytes = Int64(size) }
      }
    }
  }

  nonisolated func download(_ download: WKDownload, didFailWithError error: any Error, resumeData: Data?) {
    MainActor.assumeIsolated {
      let key = ObjectIdentifier(download)
      guard let id = running.removeValue(forKey: key) else { return }
      pending[key] = nil
      progress[id] = nil
      tasks[id] = nil
      update(id) { $0.failed = error.localizedDescription }
    }
  }
}
