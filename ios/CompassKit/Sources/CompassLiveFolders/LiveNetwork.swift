// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CryptoKit
import Foundation

/// `compass.fetch` on the network: an ephemeral URLSession with no cookie
/// store or cache of its own. Cookies come only from `cookies` (the space's
/// website data, for granted hosts), each redirect hop is checked against
/// the allowlist, and bodies stop at the size limit.
public struct URLSessionLiveFetcher: LiveFetcher {
  /// The cookie header fields to send to a URL
  /// (`HTTPCookie.requestHeaderFields(with:)`); asked only for granted
  /// cookie hosts.
  public var cookies: @Sendable (URL) async -> [String: String]

  public init(cookies: @escaping @Sendable (URL) async -> [String: String] = { _ in [:] }) {
    self.cookies = cookies
  }

  public func fetch(_ request: LiveFetchRequest, allowed: LiveAllowedHosts, limits: LiveLimits) async throws
    -> LiveFetchResponse
  {
    let configuration = URLSessionConfiguration.ephemeral
    configuration.httpCookieStorage = nil
    configuration.httpShouldSetCookies = false
    configuration.httpCookieAcceptPolicy = .never
    configuration.urlCache = nil
    configuration.timeoutIntervalForRequest = limits.timeout
    let task = FetchTask(request: request, allowed: allowed, limits: limits, cookies: cookies)
    let session = URLSession(configuration: configuration, delegate: task, delegateQueue: nil)
    defer { session.finishTasksAndInvalidate() }
    var urlRequest = URLRequest(url: request.url)
    urlRequest.httpMethod = request.method
    urlRequest.httpShouldHandleCookies = false
    for (name, value) in request.headers { urlRequest.setValue(value, forHTTPHeaderField: name) }
    if let body = request.body, request.method == "POST" { urlRequest.httpBody = Data(body.utf8) }
    try await task.addCookies(to: &urlRequest)
    return try await withTaskCancellationHandler {
      try await withCheckedThrowingContinuation { continuation in
        task.start(urlRequest, in: session, continuation)
      }
    } onCancel: {
      session.invalidateAndCancel()
    }
  }
}

/// One fetch's delegate: redirects, the size limit, the body.
private final class FetchTask: NSObject, URLSessionDataDelegate, @unchecked Sendable {
  let request: LiveFetchRequest
  let allowed: LiveAllowedHosts
  let limits: LiveLimits
  let cookies: @Sendable (URL) async -> [String: String]
  private let lock = NSLock()
  private var continuation: CheckedContinuation<LiveFetchResponse, Error>?
  private var body = Data()
  private var redirects = 0
  private var failure: LiveError?

  init(
    request: LiveFetchRequest, allowed: LiveAllowedHosts, limits: LiveLimits,
    cookies: @escaping @Sendable (URL) async -> [String: String]
  ) {
    self.request = request
    self.allowed = allowed
    self.limits = limits
    self.cookies = cookies
  }

  func start(_ urlRequest: URLRequest, in session: URLSession, _ continuation: CheckedContinuation<LiveFetchResponse, Error>) {
    lock.withLock { self.continuation = continuation }
    session.dataTask(with: urlRequest).resume()
  }

  /// The Cookie header for a hop, when cookies go there.
  func addCookies(to urlRequest: inout URLRequest) async throws {
    urlRequest.setValue(nil, forHTTPHeaderField: "Cookie")
    guard request.withCookies, let url = urlRequest.url, allowed.allowsCookies(url) else { return }
    for (name, value) in await cookies(url) { urlRequest.setValue(value, forHTTPHeaderField: name) }
  }

  private func complete(_ result: Result<LiveFetchResponse, Error>) {
    let continuation = lock.withLock {
      let c = self.continuation
      self.continuation = nil
      return c
    }
    continuation?.resume(with: result)
  }

  func urlSession(
    _ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
    newRequest: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void
  ) {
    redirects += 1
    guard let url = newRequest.url else { return completionHandler(nil) }
    if redirects > limits.maxRedirects {
      failure = LiveError("network", "Too many redirects")
    } else if !allowed.allows(url) {
      failure = LiveError("host_not_allowed", "Redirect to \(url.host() ?? url.absoluteString) is not allowed")
    }
    if failure != nil {
      task.cancel()
      return completionHandler(nil)
    }
    let next = newRequest
    Task {
      var hop = next
      hop.httpShouldHandleCookies = false
      try? await self.addCookies(to: &hop)
      completionHandler(hop)
    }
  }

  func urlSession(
    _ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
    completionHandler: @escaping @Sendable (URLSession.ResponseDisposition) -> Void
  ) {
    if response.expectedContentLength > Int64(limits.maxResponseBytes) {
      failure = tooLarge(response.url)
      return completionHandler(.cancel)
    }
    completionHandler(.allow)
  }

  func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
    body.append(data)
    if body.count > limits.maxResponseBytes {
      failure = tooLarge(dataTask.currentRequest?.url)
      dataTask.cancel()
    }
  }

  private func tooLarge(_ url: URL?) -> LiveError {
    LiveError("response_too_large", "Response from \(url?.host() ?? "the server") is too large")
  }

  func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
    if let failure { return complete(.failure(failure)) }
    if let error { return complete(.failure(LiveError("network", error.localizedDescription))) }
    guard let http = task.response as? HTTPURLResponse, let url = http.url else {
      return complete(.failure(LiveError("network", "No response")))
    }
    var headers: [String: String] = [:]
    for (name, value) in http.allHeaderFields {
      if let name = name as? String { headers[name.lowercased()] = "\(value)" }
    }
    complete(.success(LiveFetchResponse(status: http.statusCode, url: url, headers: headers, body: body)))
  }
}

/// An installable plugin as fetched from its manifest's URL (the desktop's
/// LivePluginPackage): the files, and their hash, which syncs with the
/// plugin's folders so another device installs the same files.
public struct LivePluginPackage: Sendable, Equatable {
  public var manifest: LiveManifest
  public var manifestJSON: Data
  public var entry: Data
  /// Empty if it has none.
  public var icon: Data
  /// The manifest's URL.
  public var source: URL
  public var sha256: String

  static let maxManifestBytes = 256 * 1024
  static let maxEntryBytes = 2 * 1024 * 1024
  static let maxIconBytes = 256 * 1024

  /// Lowercase hex SHA-256 of the manifest, the entry and the icon, each
  /// preceded by its length as 8 bytes, little-endian.
  public static func hash(manifest: Data, entry: Data, icon: Data) -> String {
    var sha = SHA256()
    for part in [manifest, entry, icon] {
      withUnsafeBytes(of: UInt64(part.count).littleEndian) { sha.update(bufferPointer: $0) }
      sha.update(data: part)
    }
    return sha.finalize().map { String(format: "%02x", $0) }.joined()
  }

  /// Checks fetched files: a valid manifest, not claiming a built-in's ID.
  public init(source: URL, manifestJSON: Data, entry: Data, icon: Data, rules: LiveRules) throws {
    let manifest = try rules.manifest(manifestJSON)
    guard !manifest.id.hasPrefix("compass.") else {
      throw LiveError("bad_manifest", "\"compass.\" IDs are for built-in plugins.")
    }
    guard String(data: entry, encoding: .utf8) != nil else {
      throw LiveError("bad_plugin", "The plugin's script isn't UTF-8 text.")
    }
    self.manifest = manifest
    self.manifestJSON = manifestJSON
    self.entry = entry
    self.icon = icon
    self.source = source
    sha256 = Self.hash(manifest: manifestJSON, entry: entry, icon: icon)
  }

  /// https, or http on localhost.
  public static func isAllowedSource(_ url: URL) -> Bool {
    switch url.scheme?.lowercased() {
    case "https": return url.host() != nil
    case "http": return ["localhost", "127.0.0.1", "::1", "[::1]"].contains(url.host()?.lowercased() ?? "")
    default: return false
    }
  }

  /// Fetches the manifest at `url`, then its entry and icon from beside it.
  /// Redirects must stay on the manifest's origin. Throws a LiveError whose
  /// message is for the user.
  public static func fetch(_ url: URL, rules: LiveRules, session: URLSession = .shared) async throws
    -> LivePluginPackage
  {
    guard isAllowedSource(url) else { throw LiveError("bad_request", "Plugins install from an https:// address.") }
    func load(_ target: URL, max: Int) async -> Data? {
      var request = URLRequest(url: target)
      request.httpShouldHandleCookies = false
      guard let (data, response) = try? await session.data(for: request),
        let http = response as? HTTPURLResponse, http.statusCode == 200, data.count <= max,
        let final = http.url, final.scheme == url.scheme, final.host() == url.host(), final.port == url.port
      else { return nil }
      return data
    }
    guard let manifestJSON = await load(url, max: maxManifestBytes) else {
      throw LiveError("network", "The plugin's manifest couldn't be downloaded.")
    }
    let manifest: LiveManifest
    do {
      manifest = try rules.manifest(manifestJSON)
    } catch let error as LiveError {
      throw LiveError("bad_manifest", "That isn't a live folder plugin's manifest: \(error.message).")
    }
    guard let entryURL = URL(string: manifest.entry, relativeTo: url),
      let entry = await load(entryURL.absoluteURL, max: maxEntryBytes)
    else { throw LiveError("network", "The plugin's script couldn't be downloaded.") }
    // A plugin without its icon still works.
    var icon = Data()
    if let name = manifest.icon, let iconURL = URL(string: name, relativeTo: url) {
      icon = await load(iconURL.absoluteURL, max: maxIconBytes) ?? Data()
    }
    return try LivePluginPackage(source: url, manifestJSON: manifestJSON, entry: entry, icon: icon, rules: rules)
  }

  /// Writes the package into `directory`/<id>/ (replacing what was there),
  /// with where it came from beside it.
  public func install(in directory: URL) throws -> URL {
    let folder = directory.appendingPathComponent(manifest.id, isDirectory: true)
    let fm = FileManager.default
    let staging = directory.appendingPathComponent(".\(manifest.id)-\(UUID().uuidString)", isDirectory: true)
    try fm.createDirectory(at: staging, withIntermediateDirectories: true)
    try manifestJSON.write(to: staging.appendingPathComponent("manifest.json"))
    try entry.write(to: staging.appendingPathComponent(manifest.entry))
    if let name = manifest.icon, !icon.isEmpty { try icon.write(to: staging.appendingPathComponent(name)) }
    let origin = try JSONSerialization.data(withJSONObject: ["source": source.absoluteString, "sha256": sha256])
    try origin.write(to: staging.appendingPathComponent(Self.sourceFile))
    if fm.fileExists(atPath: folder.path) { try fm.removeItem(at: folder) }
    try fm.moveItem(at: staging, to: folder)
    return folder
  }

  static let sourceFile = "compass-source.json"

  /// The plugins installed in `directory` (each in a folder of its own),
  /// checked again against their hash.
  public static func installed(in directory: URL, rules: LiveRules) -> [LivePlugin] {
    let fm = FileManager.default
    let names = (try? fm.contentsOfDirectory(atPath: directory.path)) ?? []
    return names.sorted().filter { !$0.hasPrefix(".") }.compactMap { name in
      let folder = directory.appendingPathComponent(name, isDirectory: true)
      guard let origin = try? Data(contentsOf: folder.appendingPathComponent(sourceFile)),
        let info = try? JSONSerialization.jsonObject(with: origin) as? [String: String],
        let source = info["source"].flatMap(URL.init(string:)),
        let manifestJSON = try? Data(contentsOf: folder.appendingPathComponent("manifest.json")),
        let manifest = try? rules.manifest(manifestJSON),
        let entry = try? Data(contentsOf: folder.appendingPathComponent(manifest.entry))
      else { return nil }
      let icon = manifest.icon.flatMap { try? Data(contentsOf: folder.appendingPathComponent($0)) } ?? Data()
      guard
        let package = try? LivePluginPackage(
          source: source, manifestJSON: manifestJSON, entry: entry, icon: icon, rules: rules)
      else { return nil }
      return package.plugin
    }
  }

  /// Plugins the user copied in by hand (the Files app's "Live Folder
  /// Plugins" folder): each folder with a manifest.json, its entry and its
  /// icon beside it, as a plugin's own files are. Their source is the
  /// folder's file URL, so other devices syncing their folders can't fetch
  /// them. Folders that aren't a valid plugin come back in `problems`, by
  /// folder name.
  public static func manual(in directory: URL, rules: LiveRules)
    -> (plugins: [LivePlugin], problems: [String])
  {
    let fm = FileManager.default
    let names = (try? fm.contentsOfDirectory(atPath: directory.path)) ?? []
    var plugins: [LivePlugin] = []
    var problems: [String] = []
    for name in names.sorted() where !name.hasPrefix(".") {
      let folder = directory.appendingPathComponent(name, isDirectory: true)
      var isFolder: ObjCBool = false
      guard fm.fileExists(atPath: folder.path, isDirectory: &isFolder), isFolder.boolValue else { continue }
      do {
        guard let manifestJSON = try? Data(contentsOf: folder.appendingPathComponent("manifest.json")) else {
          throw LiveError("bad_plugin", "no manifest.json")
        }
        guard manifestJSON.count <= maxManifestBytes else { throw LiveError("bad_plugin", "manifest.json is too large") }
        let manifest: LiveManifest
        do {
          manifest = try rules.manifest(manifestJSON)
        } catch let error as LiveError {
          throw LiveError("bad_manifest", error.message)
        }
        guard let entry = try? Data(contentsOf: folder.appendingPathComponent(manifest.entry)) else {
          throw LiveError("bad_plugin", "no \(manifest.entry) beside manifest.json")
        }
        guard entry.count <= maxEntryBytes else { throw LiveError("bad_plugin", "\(manifest.entry) is too large") }
        var icon = manifest.icon.flatMap { try? Data(contentsOf: folder.appendingPathComponent($0)) } ?? Data()
        if icon.count > maxIconBytes { icon = Data() }
        let package = try LivePluginPackage(
          source: folder, manifestJSON: manifestJSON, entry: entry, icon: icon, rules: rules)
        plugins.append(package.plugin)
      } catch let error as LiveError {
        problems.append("\(name): \(error.message)")
      } catch {
        problems.append("\(name): \(error.localizedDescription)")
      }
    }
    return (plugins, problems)
  }

  public var plugin: LivePlugin {
    LivePlugin(
      manifest: manifest, source: String(decoding: entry, as: UTF8.self), icon: icon.isEmpty ? nil : icon,
      sourceURL: source.absoluteString, sha256: sha256)
  }
}
