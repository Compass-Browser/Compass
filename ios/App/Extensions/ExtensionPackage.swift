// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Compression
import Foundation

/// An extension's files on disk: its ZIP unpacked, and its manifest made
/// runnable here.
///
/// WKWebView runs service workers only in apps with the default-browser
/// entitlement (or App-Bound Domains), so a Chrome extension's background
/// service worker never starts in Compass. WebKit runs a background page
/// as well, though, and the APIs a service worker uses are the same there,
/// so `background.service_worker` becomes `background.scripts` (an ES
/// module stays one). Files a classic worker loads with importScripts()
/// (literal names) are listed before it, and importScripts() becomes a
/// no-op.
///
/// WebKit on iOS also lacks some namespaces desktop extensions take for
/// granted (chrome.notifications, idle, management, …): touching one at
/// startup throws and the background never starts (1Password). A polyfill
/// runs first in the background, popup and options pages and stubs each
/// missing one (docs/design/ios.md, "Web extensions").
enum ExtensionPackage {
  static let polyfillName = "compass-polyfill.js"
  static let debugName = "compass-debug.js"
  static let entryName = "compass-background-entry.js"

  /// Unpacks `zip` into `directory` (made new) and adapts its manifest.
  static func unpack(_ zip: Data, to directory: URL) throws {
    let files = FileManager.default
    try? files.removeItem(at: directory)
    try files.createDirectory(at: directory, withIntermediateDirectories: true)
    do {
      try unzip(zip, to: directory)
      try adaptManifest(in: directory)
    } catch {
      try? files.removeItem(at: directory)
      throw error
    }
  }

  // MARK: ZIP

  /// Stored and deflated entries, from the central directory (sizes are
  /// there even when the local headers defer them). No ZIP64: extension
  /// packages are far below 4 GB.
  static func unzip(_ data: Data, to directory: URL) throws {
    let bytes = [UInt8](data)
    func u16(_ at: Int) -> Int { Int(bytes[at]) | Int(bytes[at + 1]) << 8 }
    func u32(_ at: Int) -> Int { u16(at) | u16(at + 2) << 16 }
    guard bytes.count >= 22 else { throw ExtensionError.badPackage }
    // The end of central directory record, within the last 64 KB + 22.
    var end = bytes.count - 22
    let floor = max(0, bytes.count - 22 - 65_535)
    while end >= floor, u32(end) != 0x0605_4B50 { end -= 1 }
    guard end >= floor else { throw ExtensionError.badPackage }
    let count = u16(end + 10)
    var at = u32(end + 16)
    let root = directory.standardizedFileURL.path
    for _ in 0..<count {
      guard at + 46 <= bytes.count, u32(at) == 0x0201_4B50 else { throw ExtensionError.badPackage }
      let method = u16(at + 10)
      let compressed = u32(at + 20)
      let size = u32(at + 24)
      let nameLength = u16(at + 28)
      let next = at + 46 + nameLength + u16(at + 30) + u16(at + 32)
      let local = u32(at + 42)
      guard at + 46 + nameLength <= bytes.count,
        let name = String(bytes: bytes[(at + 46)..<(at + 46 + nameLength)], encoding: .utf8)
      else { throw ExtensionError.badPackage }
      at = next
      let relative = name.drop { $0 == "/" }
      if relative.isEmpty || relative.hasSuffix("/") { continue }
      let target = directory.appendingPathComponent(String(relative)).standardizedFileURL
      // Nothing outside the extension's folder ("../").
      guard target.path.hasPrefix(root + "/") else { throw ExtensionError.badPackage }
      guard local + 30 <= bytes.count, u32(local) == 0x0403_4B50 else { throw ExtensionError.badPackage }
      let start = local + 30 + u16(local + 26) + u16(local + 28)
      guard start + compressed <= bytes.count else { throw ExtensionError.badPackage }
      let content: Data
      switch method {
      case 0:
        content = Data(bytes[start..<(start + compressed)])
      case 8:
        content = try inflate(bytes, start..<(start + compressed), size: size)
      default:
        throw ExtensionError.badPackage
      }
      try FileManager.default.createDirectory(
        at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
      try content.write(to: target)
    }
  }

  /// Raw deflate (COMPRESSION_ZLIB is raw, without the zlib header).
  private static func inflate(_ bytes: [UInt8], _ range: Range<Int>, size: Int) throws -> Data {
    if size == 0 { return Data() }
    var output = [UInt8](repeating: 0, count: size)
    let written = bytes[range].withUnsafeBufferPointer { source in
      compression_decode_buffer(&output, size, source.baseAddress!, range.count, nil, COMPRESSION_ZLIB)
    }
    guard written == size else { throw ExtensionError.badPackage }
    return Data(output)
  }

  // MARK: Manifest

  /// Namespaces WebKit on iOS may lack. Each missing one becomes a stub:
  /// on… members are events that never fire, anything else a function that
  /// does nothing (its promise resolves, its callback gets no value).
  static let stubbedNamespaces = [
    "notifications", "idle", "management", "privacy", "downloads", "contextMenus", "menus", "history",
    "bookmarks", "sessions", "topSites", "offscreen", "identity", "tts", "ttsEngine", "power", "sidePanel",
    "fontSettings", "proxy", "browsingData", "contentSettings", "debugger", "search", "tabGroups", "webRequest",
    "enterprise", "gcm", "instanceID", "pageCapture", "printerProvider", "system", "wallpaper",
  ]

  /// Members of namespaces WebKit has that it may lack, stubbed the same
  /// way when missing.
  static let stubbedMembers: [String: [String]] = [
    "webNavigation": [
      "onCreatedNavigationTarget", "onHistoryStateUpdated", "onReferenceFragmentUpdated", "onTabReplaced",
      "onBeforeNavigate", "onCommitted", "onDOMContentLoaded", "onCompleted", "onErrorOccurred",
    ],
    "tabs": [
      "onReplaced", "onHighlighted", "onZoomChange", "onAttached", "onDetached", "highlight", "discard", "group",
      "ungroup", "captureVisibleTab", "setZoomSettings", "getZoomSettings",
    ],
    "runtime": [
      "onSuspend", "onSuspendCanceled", "onUpdateAvailable", "onRestartRequired", "onConnectExternal",
      "onMessageExternal", "onUserScriptMessage", "setUninstallURL", "requestUpdateCheck", "restart", "getContexts",
    ],
    "windows": ["onBoundsChanged"],
    "action": ["openPopup", "setBadgeTextColor", "getBadgeTextColor", "getUserSettings", "onUserSettingsChanged", "isEnabled"],
    "storage": ["managed", "session"],
    "scripting": ["registerContentScripts", "unregisterContentScripts", "getRegisteredContentScripts"],
    "permissions": ["onAdded", "onRemoved", "addHostAccessRequest", "removeHostAccessRequest"],
    "commands": ["onCommand"],
    "alarms": ["onAlarm"],
  ]

  static let polyfill = """
    (() => {
      const stub = (path) => new Proxy(function () {}, {
        get(_, key) {
          if (typeof key !== "string" || key === "then") return undefined;
          if (key.startsWith("on")) {
            return { addListener() {}, removeListener() {}, hasListener() { return false; }, hasListeners() { return false; } };
          }
          return stub(path + "." + key);
        },
        apply(_, __, args) {
          // What a caller can use: lists for queries, objects for getters.
          const name = path.split(".").pop();
          const result = name === "queryState" ? "active"
            : /^(getAll|query|search|matchAll)/.test(name) ? []
            : /^get/.test(name) ? {} : undefined;
          const callback = args.length && typeof args[args.length - 1] === "function" ? args[args.length - 1] : null;
          if (callback) { setTimeout(() => callback(result), 0); return undefined; }
          return Promise.resolve(result);
        },
      });
      const members = \(membersJSON);
      const event = () => ({ addListener() {}, removeListener() {}, hasListener() { return false; }, hasListeners() { return false; } });
      const filler = (name, member) => (member.startsWith("on") ? event() : stub(name + "." + member));
      // Some of WebKit's getters throw (a member it lists but doesn't run).
      const absent = (object, key) => { try { return object[key] === undefined; } catch (e) { return false; } };
      // Added for good: WebKit redefines its namespaces' properties after
      // startup and drops configurable additions. The globals themselves
      // stay WebKit's: a proxy in their place stops events reaching the
      // page's listeners (action.onClicked never fires).
      const add = (object, key, value) => {
        try { Object.defineProperty(object, key, { value, configurable: false, writable: false, enumerable: false }); } catch (e) {}
      };
      for (const api of new Set([globalThis.chrome, globalThis.browser])) {
        if (!api) continue;
        for (const name of \(stubbedNamespaces.description)) {
          if (absent(api, name)) add(api, name, stub(name));
        }
        for (const name of Object.keys(members)) {
          let namespace;
          try { namespace = api[name]; } catch (e) { continue; }
          if (!namespace) continue;
          for (const member of members[name]) {
            if (absent(namespace, member)) add(namespace, member, filler(name, member));
          }
        }
      }
      // What a service worker has and a background page doesn't.
      if (typeof globalThis.importScripts !== "function") globalThis.importScripts = function () {};
      if (!globalThis.clients) {
        globalThis.clients = {
          claim: async () => {},
          get: async () => undefined,
          matchAll: async () => [],
          openWindow: async (url) => { try { await globalThis.chrome.tabs.create({ url }); } catch (e) {} return null; },
        };
      }
      if (typeof globalThis.skipWaiting !== "function") globalThis.skipWaiting = async () => {};
      if (!globalThis.registration) {
        globalThis.registration = { scope: location.origin + "/", active: null, showNotification: async () => {}, getNotifications: async () => [] };
      }
    })();
    """

  private static var membersJSON: String {
    (try? JSONSerialization.data(withJSONObject: stubbedMembers, options: .sortedKeys))
      .flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
  }

  static func adaptManifest(in directory: URL) throws {
    let url = directory.appendingPathComponent("manifest.json")
    guard var manifest = try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any] else {
      throw ExtensionError.badPackage
    }
    try polyfill.write(to: directory.appendingPathComponent(polyfillName), atomically: true, encoding: .utf8)
    var first = [polyfillName]
    #if DEBUG
      // -CompassNoPolyfill YES: install without it (to tell its effects apart).
      if UserDefaults.standard.bool(forKey: "CompassNoPolyfill") { first = [] }
    #endif
    #if DEBUG
      try debugScript.write(to: directory.appendingPathComponent(debugName), atomically: true, encoding: .utf8)
      first.append(debugName)
      // The debug script reports through native messaging.
      var permissions = manifest["permissions"] as? [String] ?? []
      if !permissions.contains("nativeMessaging") { permissions.append("nativeMessaging") }
      manifest["permissions"] = permissions
    #endif

    if var background = manifest["background"] as? [String: Any] {
      let module = background["type"] as? String == "module"
      var scripts = (background["scripts"] as? [String] ?? []).map { String($0.drop { $0 == "/" }) }
      if let worker = background["service_worker"] as? String {
        let path = String(worker.drop { $0 == "/" })
        background["service_worker"] = nil
        scripts = (module ? [] : importedScripts(of: path, in: directory)) + [path]
      }
      if let page = background["page"] as? String {
        inject(first, intoPage: page, in: directory)
      } else if module {
        // A classic script can't run before a module background: an entry
        // module imports ours first.
        let imports = (first + scripts).map { "import \"/\($0)\";" }.joined(separator: "\n")
        try (imports + "\n").write(to: directory.appendingPathComponent(entryName), atomically: true, encoding: .utf8)
        background["scripts"] = [entryName]
      } else if !scripts.isEmpty {
        background["scripts"] = first + scripts
      }
      manifest["background"] = background
    }
    let action = (manifest["action"] ?? manifest["browser_action"]) as? [String: Any]
    let pages = [
      action?["default_popup"] as? String,
      (manifest["options_ui"] as? [String: Any])?["page"] as? String,
      manifest["options_page"] as? String,
    ]
    for page in pages.compactMap({ $0 }) { inject(first, intoPage: page, in: directory) }
    let data = try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys])
    try data.write(to: url, options: .atomic)
  }

  /// Puts `scripts` first in an extension page's <head>.
  private static func inject(_ scripts: [String], intoPage page: String, in directory: URL) {
    let path = (page.split(separator: "?").first ?? "").drop { $0 == "/" }
    let url = directory.appendingPathComponent(String(path))
    guard var html = try? String(contentsOf: url, encoding: .utf8) else { return }
    let tags = scripts.map { "<script src=\"/\($0)\"></script>" }.joined()
    if let head = html.range(of: "<head[^>]*>", options: [.regularExpression, .caseInsensitive]) {
      html.insert(contentsOf: tags, at: head.upperBound)
    } else {
      html = tags + html
    }
    try? html.write(to: url, atomically: true, encoding: .utf8)
  }

  /// The scripts `path` loads with importScripts("…", '…'), and theirs,
  /// first-loaded first, relative to the worker's folder (or the root, for
  /// "/…").
  static func importedScripts(of path: String, in directory: URL) -> [String] {
    var result: [String] = []
    var seen: Set<String> = [path]
    func visit(_ script: String) {
      guard let source = try? String(contentsOf: directory.appendingPathComponent(script), encoding: .utf8) else { return }
      let base = (script as NSString).deletingLastPathComponent
      for call in source.matches(of: /importScripts\s*\(([^)]*)\)/) {
        for literal in call.1.matches(of: /["']([^"']+)["']/) {
          let name = String(literal.1)
          let resolved = name.hasPrefix("/")
            ? String(name.dropFirst())
            : ((base as NSString).appendingPathComponent(name) as NSString).standardizingPath
          guard !seen.contains(resolved) else { continue }
          seen.insert(resolved)
          visit(resolved)
          result.append(resolved)
        }
      }
    }
    visit(path)
    return result
  }

  #if DEBUG
    /// Debug builds: the background and popup pages forward console errors
    /// and warnings, uncaught errors and rejections to the app
    /// (sendNativeMessage), so a simulator run shows why an extension fails.
    static let debugApplication = "compass-debug"
    static let debugScript = """
      (() => {
        const api = globalThis.browser ?? globalThis.chrome;
        const where = location.pathname;
        const text = (args) => args.map((a) => {
          try { return a instanceof Error ? (a.name + ": " + a.message + " | " + (a.stack || "").split("\\n").slice(0, 4).join(" < ")) : a && typeof a === "object" && a.message ? (a.name + ": " + a.message + " " + JSON.stringify(a)) : typeof a === "string" ? a : JSON.stringify(a); }
          catch (e) { return String(a); }
        }).join(" ").slice(0, 3000);
        const send = (level, args) => {
          try { api.runtime.sendNativeMessage("\(debugApplication)", { level, where, text: text(args) }).catch(() => {}); } catch (e) {}
        };
        for (const level of ["error", "warn"]) {
          const original = console[level];
          console[level] = (...args) => { send(level, args); original.apply(console, args); };
        }
        addEventListener("error", (e) => send("uncaught", [e.message + " @ " + e.filename + ":" + e.lineno]));
        addEventListener("unhandledrejection", (e) => send("rejection", [e.reason]));
        send("start", [where]);
      })();
      """
  #endif
}
