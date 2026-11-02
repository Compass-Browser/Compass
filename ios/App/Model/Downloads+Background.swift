// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import WebKit

// Downloads that go on in the background. A page's download that is a plain
// GET over http(s) leaves WebKit (whose downloads stop when the app is
// suspended) for a background URLSession, with the tab's cookies, user agent
// and referrer. iOS runs it out of process, wakes the app when it ends, and
// keeps it across the app being quit by the system. POSTs, blob: and data:
// downloads stay with WebKit.

extension Downloads {
  static let sessionID = "it.cipi1965.compass.downloads"

  static func canRunInBackground(_ request: URLRequest) -> Bool {
    let scheme = request.url?.scheme?.lowercased()
    let method = request.httpMethod?.uppercased() ?? "GET"
    return (scheme == "http" || scheme == "https") && method == "GET" && request.httpBody == nil
      && request.httpBodyStream == nil
  }

  /// Opens the background session; downloads it ran while the app was gone
  /// come back to the list.
  func startSession() {
    let configuration = URLSessionConfiguration.background(withIdentifier: Self.sessionID)
    configuration.sessionSendsLaunchEvents = true
    configuration.isDiscretionary = false
    configuration.httpShouldSetCookies = false
    configuration.httpCookieAcceptPolicy = .never
    let session = URLSession(configuration: configuration, delegate: self, delegateQueue: nil)
    self.session = session
    session.getAllTasks { tasks in
      let found = tasks.compactMap { task -> (UUID, String, URLSessionDownloadTask)? in
        guard let task = task as? URLSessionDownloadTask, task.state == .running || task.state == .suspended,
          let (id, name) = Self.parse(task.taskDescription)
        else { return nil }
        return (id, name, task)
      }
      Task { @MainActor [weak self] in
        for (id, name, task) in found {
          self?.sessionTasks[id] = task
          self?.insertRunning(Item(id: id, name: name, progress: Self.fraction(task), background: true))
        }
      }
    }
  }

  /// Starts `request` in the background session, as the tab would send it.
  func startInBackground(_ request: URLRequest, from webView: WKWebView) {
    guard let url = request.url, let session else { return }
    let id = UUID()
    let name = url.lastPathComponent.isEmpty || url.lastPathComponent == "/" ? (url.host() ?? "Download") : url.lastPathComponent
    insertRunning(Item(id: id, name: name, progress: 0, background: true))
    let referrer = webView.url
    let agent = webView.customUserAgent
    let cookies = webView.configuration.websiteDataStore.httpCookieStore
    Task {
      var outgoing = request
      outgoing.httpShouldHandleCookies = false
      let jar = await cookies.allCookies().filter { Self.cookie($0, matches: url) }
      for (field, value) in HTTPCookie.requestHeaderFields(with: jar) { outgoing.setValue(value, forHTTPHeaderField: field) }
      if outgoing.value(forHTTPHeaderField: "User-Agent") == nil {
        var ua = agent?.isEmpty == false ? agent : nil
        if ua == nil { ua = try? await webView.evaluateJavaScript("navigator.userAgent") as? String }
        if let ua { outgoing.setValue(ua, forHTTPHeaderField: "User-Agent") }
      }
      if outgoing.value(forHTTPHeaderField: "Referer") == nil, let referrer, referrer.scheme?.hasPrefix("http") == true,
        !(referrer.scheme == "https" && url.scheme == "http")
      {
        outgoing.setValue(referrer.absoluteString, forHTTPHeaderField: "Referer")
      }
      let task = session.downloadTask(with: outgoing)
      task.taskDescription = Self.describe(id, name)
      sessionTasks[id] = task
      task.resume()
    }
  }

  /// Whether a cookie goes with a request to `url` (domain, path, secure).
  nonisolated static func cookie(_ cookie: HTTPCookie, matches url: URL) -> Bool {
    guard let host = url.host()?.lowercased() else { return false }
    if cookie.isSecure && url.scheme?.lowercased() != "https" { return false }
    let domain = cookie.domain.lowercased()
    let domainMatches =
      domain.hasPrefix(".")
      ? host == domain.dropFirst() || host.hasSuffix(domain)
      : host == domain
    let path = url.path().isEmpty ? "/" : url.path()
    return domainMatches && (cookie.path == "/" || path.hasPrefix(cookie.path))
  }

  /// The task's description: the item's ID and its name so far.
  nonisolated static func describe(_ id: UUID, _ name: String) -> String { id.uuidString + "\n" + name }

  nonisolated static func parse(_ description: String?) -> (UUID, String)? {
    guard let parts = description?.split(separator: "\n", maxSplits: 1), let first = parts.first,
      let id = UUID(uuidString: String(first))
    else { return nil }
    return (id, parts.count > 1 ? String(parts[1]) : "Download")
  }

  nonisolated static func fraction(_ task: URLSessionTask) -> Double? {
    task.countOfBytesExpectedToReceive > 0
      ? Double(task.countOfBytesReceived) / Double(task.countOfBytesExpectedToReceive) : nil
  }
}

extension Downloads: URLSessionDownloadDelegate {
  nonisolated func urlSession(
    _ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64
  ) {
    guard let (id, name) = Self.parse(downloadTask.taskDescription) else { return }
    let fraction = totalBytesExpectedToWrite > 0 ? Double(totalBytesWritten) / Double(totalBytesExpectedToWrite) : nil
    let suggested = downloadTask.response?.suggestedFilename
    Task { @MainActor in
      // Back after a relaunch: list it again.
      if !self.items.contains(where: { $0.id == id }) {
        self.sessionTasks[id] = downloadTask
        self.insertRunning(Item(id: id, name: name, progress: fraction, background: true))
      }
      self.update(id) {
        $0.progress = fraction
        $0.bytes = totalBytesWritten
        if let suggested { $0.name = suggested }
      }
    }
  }

  /// The file must leave `location` before this returns.
  nonisolated func urlSession(
    _ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL
  ) {
    guard let (id, name) = Self.parse(downloadTask.taskDescription) else { return }
    if let http = downloadTask.response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
      let status = HTTPURLResponse.localizedString(forStatusCode: http.statusCode).capitalized
      Task { @MainActor in self.finish(id, name: name, file: nil, failed: "\(http.statusCode) \(status)") }
      return
    }
    let suggested = downloadTask.response?.suggestedFilename ?? name
    try? FileManager.default.createDirectory(at: Self.folderURL, withIntermediateDirectories: true)
    let target = Self.destination(suggested, in: Self.folderURL)
    do {
      try FileManager.default.moveItem(at: location, to: target)
      Task { @MainActor in self.finish(id, name: target.lastPathComponent, file: target, failed: nil) }
    } catch {
      let message = error.localizedDescription
      Task { @MainActor in self.finish(id, name: name, file: nil, failed: message) }
    }
  }

  nonisolated func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: (any Error)?) {
    guard let error, let (id, name) = Self.parse(task.taskDescription) else { return }
    if (error as? URLError)?.code == .cancelled { return }
    let message = error.localizedDescription
    Task { @MainActor in self.finish(id, name: name, file: nil, failed: message) }
  }

  nonisolated func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
    Task { @MainActor in
      let completion = self.backgroundCompletion
      self.backgroundCompletion = nil
      completion?()
    }
  }

  private func finish(_ id: UUID, name: String, file: URL?, failed: String?) {
    sessionTasks[id] = nil
    if !items.contains(where: { $0.id == id }) {
      insertRunning(Item(id: id, name: name, progress: 0, background: true))
    }
    update(id) {
      $0.name = name
      $0.file = file
      $0.failed = failed
      $0.date = Date()
      if let file {
        $0.progress = 1
        if let size = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize { $0.bytes = Int64(size) }
      }
    }
  }
}
