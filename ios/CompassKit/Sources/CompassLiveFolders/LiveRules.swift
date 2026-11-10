// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import JavaScriptCore

/// The scripts a refresh loads, read from a `plugins/` folder (the repo's,
/// or the copy in the app's bundle) plus the web globals a bare JSContext
/// lacks (Resources/jsc_globals.js).
public struct LiveScripts: Sendable {
  /// sdk/text.js, xml.js, html.js, ics.js, then runtime/prelude.js: what
  /// every plugin context loads before the plugin (docs/design/live-folders.md,
  /// "Runtime").
  public let context: [(name: String, source: String)]
  public let globals: String
  public let manifest: String
  public let result: String
  public let text: String
  /// The `plugins/` folder.
  public let directory: URL

  public init(directory: URL) throws {
    func read(_ path: String) throws -> String {
      try String(contentsOf: directory.appendingPathComponent(path), encoding: .utf8)
    }
    self.directory = directory
    context = try ["sdk/text.js", "sdk/xml.js", "sdk/html.js", "sdk/ics.js", "runtime/prelude.js"].map {
      ($0, try read($0))
    }
    text = context[0].source
    manifest = try read("runtime/manifest.js")
    result = try read("runtime/result.js")
    guard let url = Bundle.module.url(forResource: "jsc_globals", withExtension: "js") else {
      throw LiveError("bad_plugin", "jsc_globals.js is missing")
    }
    globals = try String(contentsOf: url, encoding: .utf8)
  }

  /// The built-in plugins (`builtin/*`), by ID.
  public func builtins() -> [LivePlugin] {
    let folder = directory.appendingPathComponent("builtin")
    let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
    let rules = LiveRules(scripts: self)
    return names.sorted().compactMap { try? rules.loadPlugin(at: folder.appendingPathComponent($0)) }
  }
}

/// The host's own context, apart from any plugin's: manifest and config
/// validation, the allowlist and result normalization, by the reference
/// scripts (`plugins/runtime/manifest.js`, `result.js`), so the rules are
/// the same as every other runtime's. Its own virtual machine, so a
/// plugin stuck in a loop doesn't hold it.
public final class LiveRules: @unchecked Sendable {
  private let context: JSContext
  private let lock = NSLock()
  private var exception: JSValue?

  public init(scripts: LiveScripts) {
    context = JSContext(virtualMachine: JSVirtualMachine())!
    context.name = "Compass live folder host"
    context.exceptionHandler = { [unowned self] _, e in self.exception = e }
    for (name, source) in [
      ("jsc_globals.js", scripts.globals), ("sdk/text.js", scripts.text), ("runtime/manifest.js", scripts.manifest),
      ("runtime/result.js", scripts.result), ("rules", Self.glue),
    ] {
      context.evaluateScript(source, withSourceURL: URL(string: "compass-live:///\(name)"))
    }
    if let exception { assertionFailure("live folder host scripts: \(exception)") }
  }

  private static let glue = """
    var __rules = (function () {
      'use strict';
      const m = CompassLive.manifest, r = CompassLive.result;
      return {
        manifest: (s) => JSON.stringify(m.validateManifest(JSON.parse(s))),
        config: (man, c) => JSON.stringify(m.validateConfig(JSON.parse(man), JSON.parse(c))),
        hosts: (man, c, s) => JSON.stringify(m.resolveHosts(JSON.parse(man), JSON.parse(c), JSON.parse(s))),
        href: (u) => { try { return new URL(u).href; } catch (e) { return null; } },
        allowed: (u, p) => m.urlAllowed(u, JSON.parse(p)),
        result: (s, max) => JSON.stringify(r.normalizeResult(JSON.parse(s), {maxItems: max})),
      };
    })();
    """

  /// Calls `__rules[name]`; a thrown error becomes a LiveError with its
  /// `code` (or `fallback`).
  private func call(_ name: String, _ args: [Any], fallback: String) throws -> JSValue {
    lock.lock()
    defer { lock.unlock() }
    exception = nil
    let value = context.objectForKeyedSubscript("__rules").invokeMethod(name, withArguments: args)
    if let e = exception {
      exception = nil
      let code = e.forProperty("code").flatMap { $0.isString ? $0.toString() : nil } ?? fallback
      let message = e.forProperty("message").flatMap { $0.isString ? $0.toString() : nil } ?? e.toString() ?? code
      throw LiveError(code, message)
    }
    return value ?? JSValue(undefinedIn: context)
  }

  private func string(_ name: String, _ args: [Any], fallback: String) throws -> String {
    let value = try call(name, args, fallback: fallback)
    guard value.isString, let s = value.toString() else { throw LiveError(fallback, "\(name) returned nothing") }
    return s
  }

  static func json(_ object: Any) -> String {
    guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.fragmentsAllowed]) else {
      return "null"
    }
    return String(decoding: data, as: UTF8.self)
  }

  static func encode<T: Encodable>(_ value: T) -> String {
    (try? JSONEncoder().encode(value)).map { String(decoding: $0, as: UTF8.self) } ?? "null"
  }

  // MARK: Manifests and config

  /// Validates and normalizes a manifest (bad_manifest).
  public func manifest(_ data: Data) throws -> LiveManifest {
    let normalized = try string("manifest", [String(decoding: data, as: UTF8.self)], fallback: "bad_manifest")
    do {
      return try JSONDecoder().decode(LiveManifest.self, from: Data(normalized.utf8))
    } catch {
      throw LiveError("bad_manifest", "The manifest can't be read: \(error)")
    }
  }

  /// A plugin folder: manifest.json, its entry script and its icon.
  public func loadPlugin(at folder: URL, sourceURL: String = "", sha256: String = "") throws -> LivePlugin {
    let manifest = try self.manifest(Data(contentsOf: folder.appendingPathComponent("manifest.json")))
    guard let source = try? String(contentsOf: folder.appendingPathComponent(manifest.entry), encoding: .utf8) else {
      throw LiveError("bad_plugin", "\(manifest.entry) is missing")
    }
    let icon = manifest.icon.flatMap { try? Data(contentsOf: folder.appendingPathComponent($0)) }
    return LivePlugin(manifest: manifest, source: source, icon: icon, sourceURL: sourceURL, sha256: sha256)
  }

  /// The folder's config checked against the manifest, defaults filled in,
  /// as JSON (bad_config).
  public func config(_ manifest: LiveManifest, _ configJSON: String) throws -> String {
    try string("config", [Self.encode(manifest), configJSON], fallback: "bad_config")
  }

  /// The allowlist with {config:key} and {secret:key} resolved.
  public func hosts(_ manifest: LiveManifest, config configJSON: String, secrets: [String: String]) throws
    -> LiveAllowedHosts
  {
    let s = try string("hosts", [Self.encode(manifest), configJSON, Self.json(secrets)], fallback: "bad_manifest")
    let lists = (try? JSONDecoder().decode([String: [String]].self, from: Data(s.utf8))) ?? [:]
    return LiveAllowedHosts(hosts: lists["hosts"] ?? [], cookies: lists["cookies"] ?? [])
  }

  // MARK: Requests and results

  /// A URL as the web parses it (`new URL(s).href`), or nil.
  public func href(_ url: String) -> String? {
    guard let value = try? call("href", [url], fallback: "bad_request"), value.isString else { return nil }
    return value.toString()
  }

  /// `manifest.js`'s urlAllowed: http(s), no credentials, an allowed host.
  public func allowed(_ url: String, _ patterns: [String]) -> Bool {
    (try? call("allowed", [url, Self.json(patterns)], fallback: "host_not_allowed"))?.toBool() ?? false
  }

  /// Normalizes what fetchItems returned, as JSON (bad_result).
  public func result(_ json: String, maxItems: Int) throws -> LiveResult {
    let s = try string("result", [json, maxItems], fallback: "bad_result")
    do {
      return try JSONDecoder().decode(LiveResult.self, from: Data(s.utf8))
    } catch {
      throw LiveError("bad_result", "The result can't be read: \(error)")
    }
  }
}
