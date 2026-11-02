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
  static let keepAliveName = "compass-keep-alive.html"
  static let keepAliveScriptName = "compass-keep-alive.js"
  static let keepAlivePort = "compass-keep-alive"
  /// The native app ID action.openPopup() messages (WebKit on iOS has no
  /// openPopup; 1Password signs in through it).
  static let openPopupApplication = "compass-open-popup"

  /// The keep-alive page's script (WebExtensions.keepAlive): a port to the
  /// background, pinged every 4 s, so WebKit doesn't unload it as idle
  /// (it does after about 5 s on iOS, and 1Password starts over each time).
  static let keepAliveScript = """
    (() => {
      const api = globalThis.browser ?? globalThis.chrome;
      let port = null;
      const connect = () => {
        try {
          port = api.runtime.connect({ name: "\(keepAlivePort)" });
          port.onDisconnect.addListener(() => { port = null; setTimeout(connect, 1000); });
        } catch (e) { setTimeout(connect, 2000); }
      };
      connect();
      setInterval(() => { try { if (port) port.postMessage("ping"); } catch (e) {} }, 4000);
    })();
    """

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
        // action.openPopup(): Compass opens the popup (native messaging).
        try {
          if (api.action && absent(api.action, "openPopup") && api.runtime && api.runtime.sendNativeMessage) {
            add(api.action, "openPopup", () => {
              try { return Promise.resolve(api.runtime.sendNativeMessage("\(openPopupApplication)", {})).then(() => undefined, () => undefined); }
              catch (e) { return Promise.resolve(); }
            });
          }
        } catch (e) {}
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
          // On the phone WebKit swaps some namespace objects out after
          // startup (storage, losing what was added): pinned, it can't.
          add(api, name, namespace);
        }
      }
      // The keep-alive page's port (ExtensionPackage.keepAliveScript): held
      // open by the background, so the connection keeps it loaded.
      try {
        const runtime = (globalThis.chrome || globalThis.browser || {}).runtime;
        if (runtime && runtime.onConnect) {
          const held = [];
          runtime.onConnect.addListener((port) => {
            if (port.name !== "\(keepAlivePort)") return;
            held.push(port);
            port.onMessage.addListener(() => {});
            port.onDisconnect.addListener(() => held.splice(held.indexOf(port), 1));
          });
        }
      } catch (e) {}
      \(framePorts)
      // When WebKit unloads an idle background page, its IndexedDB
      // connections close first, with a "close" event Chrome never sends
      // when it stops a service worker: 1Password takes it for a crash and
      // starts over on its error page. Dropped.
      if (globalThis.IDBDatabase) {
        const proto = IDBDatabase.prototype;
        const addEventListener = proto.addEventListener;
        proto.addEventListener = function (type, ...rest) {
          if (type === "close") return;
          return addEventListener.call(this, type, ...rest);
        };
        try { Object.defineProperty(proto, "onclose", { get() { return null; }, set() {}, configurable: true }); } catch (e) {}
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
      // An extension page in a frame on a web page (1Password's "Save
      // passkey?" sheet) is laid out for a desktop window: a box with a
      // fixed width (400 px) runs past the phone's edge. Such a box gets a
      // max-width of the frame less 16 px a side, the margin 1Password keeps
      // (measured once it's off the edge: where it is mid-animation varies).
      try {
        if (typeof document !== "undefined" && window.top !== window) {
          const fitted = new WeakSet();
          const fit = () => {
            const width = innerWidth;
            if (!document.body || width >= 480) return;
            for (const element of document.body.querySelectorAll("*")) {
              const box = element.getBoundingClientRect();
              if (box.width < width / 2 || box.right <= 0 || box.left >= width) continue;
              if (fitted.has(element) || (box.left >= 0 && box.right <= width)) continue;
              fitted.add(element);
              element.style.setProperty("box-sizing", "border-box", "important");
              element.style.setProperty("max-width", "calc(100vw - 32px)", "important");
            }
          };
          let queued = false;
          const queue = () => {
            if (queued) return;
            queued = true;
            requestAnimationFrame(() => { queued = false; fit(); });
          };
          new MutationObserver(queue).observe(document, { childList: true, subtree: true, attributes: true, attributeFilter: ["class", "style"] });
          addEventListener("resize", queue);
          addEventListener("load", queue);
        }
      } catch (e) {}
    })();
    """

  /// The message key of an emulated port's traffic.
  static let portKey = "__compassPort"

  /// Ports from extension frames in web pages, over runtime.sendMessage.
  ///
  /// WebKit drops a `runtime.connect` port opened by an extension frame
  /// inside a tab a few ms after the background's onConnect, unless the
  /// frame connects as soon as it loads (1Password's "Save passkey?" sheet
  /// connects about 120 ms in, and loses it every time). `sendMessage` from
  /// the same frame always gets through. So in such a frame `connect()`
  /// returns a Port of ours: its messages go out as sendMessage calls, one
  /// after another, and the background's come back as replies to a long
  /// poll. In the background, onConnect listeners get a matching Port. A
  /// port whose frame goes away without a word (no poll for 25 s) is
  /// disconnected. Every context's onMessage listeners skip this traffic.
  static let framePorts = """
    try {
        const KEY = "\(portKey)";
        const runtime = (globalThis.chrome || globalThis.browser || {}).runtime;
        const isFrame = typeof window !== "undefined" && window.top !== window;
        let backgroundPage = "/_generated_background_page.html";
        try { const page = runtime.getManifest().background.page; if (page) backgroundPage = "/" + page.replace(/^\\//, ""); } catch (e) {}
        const isBackground = typeof location !== "undefined" && location.pathname === backgroundPage;
        const events = (set) => ({
          addListener(f) { set.add(f); }, removeListener(f) { set.delete(f); },
          hasListener(f) { return set.has(f); }, hasListeners() { return set.size > 0; },
        });
        const fire = (set, ...args) => {
          for (const f of [...set]) { try { f(...args); } catch (e) { setTimeout(() => { throw e; }); } }
        };
        const disconnected = () => new Error("Attempting to use a disconnected port object");
        if (runtime && runtime.onMessage && runtime.sendMessage) {
          // The extension's onMessage listeners never see this traffic.
          const onMessage = runtime.onMessage;
          const add = onMessage.addListener, remove = onMessage.removeListener, has = onMessage.hasListener;
          const wrapped = new Map();
          const wrap = (f) => {
            if (typeof f !== "function") return f;
            if (!wrapped.has(f)) wrapped.set(f, function (message, ...rest) { return message && message[KEY] ? undefined : f.call(this, message, ...rest); });
            return wrapped.get(f);
          };
          const define = (object, key, value) => { try { Object.defineProperty(object, key, { value, configurable: false, writable: false }); } catch (e) {} };
          define(onMessage, "addListener", function (f) { return add.call(this, wrap(f)); });
          define(onMessage, "removeListener", function (f) { const result = remove.call(this, wrap(f)); wrapped.delete(f); return result; });
          if (has) define(onMessage, "hasListener", function (f) { return wrapped.has(f) && has.call(this, wrapped.get(f)); });

          if (isBackground && runtime.onConnect) {
            const onConnect = runtime.onConnect;
            const connectAdd = onConnect.addListener, connectRemove = onConnect.removeListener, connectHas = onConnect.hasListener;
            const listeners = new Set();
            define(onConnect, "addListener", function (f) { listeners.add(f); return connectAdd.call(this, f); });
            define(onConnect, "removeListener", function (f) { listeners.delete(f); return connectRemove.call(this, f); });
            if (connectHas) define(onConnect, "hasListener", function (f) { return connectHas.call(this, f); });
            const ports = new Map();
            const flush = (record) => {
              if (!record.waiter) return;
              clearTimeout(record.timer);
              const reply = record.waiter;
              record.waiter = null;
              const messages = record.queue.splice(0);
              try { reply({ messages, closed: !record.open }); } catch (e) {}
              if (!record.open) ports.delete(record.id);
            };
            const drop = (record) => {
              if (!record.open) return;
              record.open = false;
              ports.delete(record.id);
              fire(record.onDisconnect, record.port);
            };
            setInterval(() => {
              const now = Date.now();
              for (const record of [...ports.values()]) {
                if (!record.waiter && now - record.seen > 25000) drop(record);
              }
            }, 5000);
            add.call(onMessage, (message, sender, reply) => {
              if (!message || !message[KEY]) return undefined;
              const record = ports.get(message.id);
              if (record) record.seen = Date.now();
              switch (message.op) {
                case "open": {
                  const onPortMessage = new Set(), onDisconnect = new Set();
                  const created = { id: message.id, open: true, queue: [], waiter: null, timer: null, seen: Date.now(), onDisconnect };
                  created.port = {
                    name: message.name || "", sender,
                    onMessage: events(onPortMessage), onDisconnect: events(onDisconnect),
                    postMessage(value) {
                      if (!created.open) throw disconnected();
                      created.queue.push(value === undefined ? null : JSON.parse(JSON.stringify(value)));
                      flush(created);
                    },
                    disconnect() {
                      if (!created.open) return;
                      created.open = false;
                      if (created.waiter) flush(created); else setTimeout(() => ports.delete(created.id), 30000);
                    },
                  };
                  created.onPortMessage = onPortMessage;
                  ports.set(message.id, created);
                  reply({ ok: true });
                  fire(listeners, created.port);
                  return undefined;
                }
                case "msg":
                  if (!record || !record.open) { reply({ gone: true }); return undefined; }
                  reply({ ok: true });
                  fire(record.onPortMessage, message.msg, record.port);
                  return undefined;
                case "poll":
                  if (!record) { reply({ messages: [], closed: true }); return undefined; }
                  if (record.waiter) flush(record);
                  record.waiter = reply;
                  if (record.queue.length || !record.open) { flush(record); return undefined; }
                  record.timer = setTimeout(() => flush(record), 10000);
                  return true;
                case "close":
                  reply({ ok: true });
                  if (record) drop(record);
                  return undefined;
              }
              return undefined;
            });
          }

          if (isFrame && runtime.connect) {
            const connect = runtime.connect;
            const open = new Set();
            const emulated = function (...args) {
              const extension = typeof args[0] === "string" ? args[0] : null;
              if (extension && extension !== runtime.id) return connect.apply(runtime, args);
              const info = (extension ? args[1] : args[0]) || {};
              const id = Date.now().toString(36) + Math.random().toString(36).slice(2);
              const onPortMessage = new Set(), onDisconnect = new Set();
              let live = true;
              let chain = Promise.resolve();
              const send = (message) => (chain = chain.then(() => runtime.sendMessage(Object.assign({ [KEY]: 1, id }, message))));
              const end = () => {
                if (!live) return;
                live = false;
                open.delete(port);
                fire(onDisconnect, port);
              };
              const poll = () => {
                if (!live) return;
                runtime.sendMessage({ [KEY]: 1, id, op: "poll" }).then((answer) => {
                  if (!live) return;
                  if (!answer) { end(); return; }
                  for (const value of answer.messages || []) {
                    if (!live) return;
                    fire(onPortMessage, value, port);
                  }
                  if (answer.closed) end(); else poll();
                }, end);
              };
              const port = {
                name: String(info.name || ""), sender: undefined,
                onMessage: events(onPortMessage), onDisconnect: events(onDisconnect),
                postMessage(value) {
                  if (!live) throw disconnected();
                  send({ op: "msg", msg: value === undefined ? null : value }).then((answer) => { if (!answer || answer.gone) end(); }, end);
                },
                disconnect() {
                  if (!live) return;
                  live = false;
                  open.delete(port);
                  send({ op: "close" }).catch(() => {});
                },
              };
              open.add(port);
              send({ op: "open", name: port.name }).then((answer) => { if (answer && answer.ok) poll(); else end(); }, end);
              return port;
            };
            try { Object.defineProperty(runtime, "connect", { value: emulated, configurable: false, writable: false }); } catch (e) {}
            addEventListener("pagehide", () => { for (const port of [...open]) port.disconnect(); });
          }
        }
      } catch (e) {}
    """

  private static var membersJSON: String {
    (try? JSONSerialization.data(withJSONObject: stubbedMembers, options: .sortedKeys))
      .flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
  }

  /// Before each load: the polyfill (and debug script) as this build has
  /// them, since they're written into the folder at install and an
  /// extension installed by an older build would keep the old ones. A
  /// folder from before the polyfill gets adapted now.
  static func refresh(in directory: URL) {
    let files = FileManager.default
    try? "<!doctype html><title></title><script src=\"/\(keepAliveScriptName)\"></script>".write(
      to: directory.appendingPathComponent(keepAliveName), atomically: true, encoding: .utf8)
    try? keepAliveScript.write(
      to: directory.appendingPathComponent(keepAliveScriptName), atomically: true, encoding: .utf8)
    guard files.fileExists(atPath: directory.appendingPathComponent(polyfillName).path) else {
      try? adaptManifest(in: directory)
      return
    }
    try? polyfill.write(to: directory.appendingPathComponent(polyfillName), atomically: true, encoding: .utf8)
    var scripts = [polyfillName]
    #if DEBUG
      if files.fileExists(atPath: directory.appendingPathComponent(debugName).path) {
        try? debugScript.write(to: directory.appendingPathComponent(debugName), atomically: true, encoding: .utf8)
        scripts.append(debugName)
      }
    #endif
    // Pages an older build left without them.
    injectIntoAllPages(scripts, in: directory)
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
    injectIntoAllPages(first, in: directory)
    let data = try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys])
    try data.write(to: url, options: .atomic)
  }

  /// Puts `scripts` first in an extension page's <head>.
  /// Every HTML page of the extension: its own pages opened in tabs
  /// (1Password's setup and sign-in, app/app.html) touch the same missing
  /// APIs as its background.
  static func injectIntoAllPages(_ scripts: [String], in directory: URL) {
    guard !scripts.isEmpty,
      let walker = FileManager.default.enumerator(at: directory, includingPropertiesForKeys: nil)
    else { return }
    let root = directory.standardizedFileURL.path
    for case let url as URL in walker where ["html", "htm"].contains(url.pathExtension.lowercased()) {
      let path = String(url.standardizedFileURL.path.dropFirst(root.count + 1))
      guard path != keepAliveName else { continue }
      inject(scripts, intoPage: path, in: directory)
    }
  }

  private static func inject(_ scripts: [String], intoPage page: String, in directory: URL) {
    let path = (page.split(separator: "?").first ?? "").drop { $0 == "/" }
    let url = directory.appendingPathComponent(String(path))
    guard var html = try? String(contentsOf: url, encoding: .utf8) else { return }
    // Already there (a page listed twice, or a refresh).
    guard let first = scripts.first, !html.contains("/\(first)\"") else { return }
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
