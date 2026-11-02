// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import CompassModel
import UIKit
import WebKit

/// Runs Claude's tools on iOS (I3, docs/design/claude.md "iOS"): the shared
/// tools.json, with results shaped as the desktop's ClaudeTools gives them.
/// Page tools act on the chat's target tab:
/// - page_agent.js in the "claude" content world reads the page and acts
///   with synthetic DOM events (isTrusted == false: a few sites ignore
///   them; the real touch input is untouched);
/// - `takeSnapshot` takes screenshots (JPEG, at most 1280 px on a side);
/// - a document-start script in the page world keeps the console
///   (`ConsoleCapture`), for read_console;
/// - run_js runs in the page world.
/// Browser tools (other tabs) are refused until I4 and without the
/// whole-browser permission.
@MainActor
final class ClaudeTools {
  struct Result {
    var content: [JSONValue]
    var isError = false

    static func text(_ text: String) -> Result { Result(content: [["type": "text", "text": .string(text)]]) }
    static func error(_ text: String) -> Result { Result(content: [["type": "text", "text": .string(text)]], isError: true) }
  }

  static let notPermitted =
    "Not permitted: the user hasn't turned on \"Let Claude use the whole browser\" (Compass settings, Claude). Without it, Claude can only use the tab the chat is about. Tell the user if the task needs more."
  static let screenshotMaxEdge: CGFloat = 1280
  static let maxJSChars = 20000

  /// The tab the chat is about (set when a message is sent).
  var targetTab: String?
  /// Screenshot pixels per CSS pixel, from the latest screenshot.
  private var screenshotScale: Double = 1
  /// read_console's "only new": the console entries already read.
  private var consoleRead = 0

  private weak var app: AppModel?

  init(app: AppModel) { self.app = app }

  func reset() {
    screenshotScale = 1
    consoleRead = 0
  }

  /// Runs one tool call.
  func run(_ name: String, input: JSONValue) async -> Result {
    let scopes = Self.scopes
    guard let scope = scopes[name] else { return .error("Unknown tool: \(name).") }
    if scope == "browser" {
      guard ClaudeSettings.shared.browserScope else { return .error(Self.notPermitted) }
      return .error("Other tabs aren't available on iOS yet.")
    }
    guard let app, let id = targetTab, let node = app.store.node(id: id) else {
      return .error("The tab the chat is about was closed.")
    }
    if name == "wait" {
      let seconds = min(max(input["seconds"]?.number ?? 1, 0.1), 10)
      try? await Task.sleep(for: .seconds(seconds))
      return .text("Waited \(seconds) s.")
    }
    if name == "navigate" { return await navigate(node, input: input) }
    guard let view = app.tabs.live[id] else { return .error("The tab isn't loaded. Open it first.") }
    guard let url = view.url, ["http", "https"].contains(url.scheme?.lowercased() ?? "") else {
      return .error("Claude can't use the browser's own pages, only web pages.")
    }
    switch name {
    case "read_page": return await agent(view, "readPage", input)
    case "get_page_text": return await agent(view, "pageText", input)
    case "find": return await agent(view, "find", input)
    case "click": return await agent(view, "click", cssInput(input))
    case "hover": return await agent(view, "hover", cssInput(input))
    case "type": return await agent(view, "type", input)
    case "key": return await agent(view, "key", input)
    case "scroll": return await agent(view, "scroll", cssInput(input))
    case "form_input": return await agent(view, "formInput", input)
    case "screenshot": return await screenshot(view, input: input)
    case "read_console": return await readConsole(view, input: input)
    case "run_js": return await runJS(view, input: input)
    default: return .error("\(name) isn't available on iOS.")
    }
  }

  /// tools.json's scopes, by tool name.
  static let scopes: [String: String] = {
    guard let url = Bundle.main.url(forResource: "tools", withExtension: "json"),
      let data = try? Data(contentsOf: url), let list = JSONValue.parse(data)?["tools"]?.array
    else { return [:] }
    var scopes: [String: String] = [:]
    for entry in list {
      if let name = entry["definition"]?["name"]?.string { scopes[name] = entry["scope"]?.string ?? "page" }
    }
    return scopes
  }()

  // MARK: Page agent

  /// `coordinate` in screenshot pixels becomes CSS pixels, as the page
  /// agent wants.
  private func cssInput(_ input: JSONValue) -> JSONValue {
    guard var object = input.object, let point = object["coordinate"]?.array, point.count == 2,
      let x = point[0].number, let y = point[1].number
    else { return input }
    object["coordinate"] = [.number(x / screenshotScale), .number(y / screenshotScale)]
    return .object(object)
  }

  private func agent(_ view: WKWebView, _ method: String, _ input: JSONValue) async -> Result {
    let body = ClaudeSettings.pageAgent + "\n;return JSON.stringify(globalThis.__compassAgent.\(method)(JSON.parse(opts)));"
    let options = String(decoding: input.encoded(), as: UTF8.self)
    do {
      let value = try await view.callAsyncJavaScript(body, arguments: ["opts": options], in: nil, contentWorld: ClaudeChat.world)
      guard let text = value as? String, var result = JSONValue.parse(text)?.object else {
        return .error("The page didn't answer (it may still be loading).")
      }
      guard result["ok"] == true else { return .error(result["error"]?.string ?? "The page couldn't be read.") }
      result["ok"] = nil
      // A tree or the page's text comes back as the desktop words it.
      if let tree = result["tree"]?.string {
        var text = Self.pageHeader(result)
        if let focused = result["focused"]?.string { text += "\nFocused: \(focused)" }
        return .text(text + "\n\n" + tree)
      }
      if method == "pageText", let body = result["text"]?.string {
        return .text(Self.pageHeader(result) + "\n\n" + body)
      }
      return .text(String(decoding: JSONValue.object(result).encoded(), as: UTF8.self))
    } catch {
      return .error("The page didn't answer: \(error.localizedDescription)")
    }
  }

  /// The page agent's title, URL and viewport, as the desktop's PageHeader.
  static func pageHeader(_ page: [String: JSONValue]) -> String {
    var header = "Title: \(page["title"]?.string ?? "")\nURL: \(page["url"]?.string ?? "")"
    if let v = page["viewport"] {
      func n(_ key: String) -> Int { Int(v[key]?.number ?? 0) }
      header += "\nViewport: \(n("width"))x\(n("height")), scrolled to \(n("scrollX")),\(n("scrollY")) of \(n("scrollWidth"))x\(n("scrollHeight"))"
    }
    return header
  }

  // MARK: Screenshot

  private func screenshot(_ view: WKWebView, input: JSONValue) async -> Result {
    if let ref = input["ref"]?.string {
      let scrolled = await agent(view, "scrollIntoView", ["ref": .string(ref)])
      if scrolled.isError { return scrolled }
      try? await Task.sleep(for: .milliseconds(300))
    }
    // What the user sees of the page: not what's under the status bar or
    // the bottom bar.
    var visible = view.bounds
    if #available(iOS 26.0, *) {
      visible = visible.inset(by: view.obscuredContentInsets)
    }
    let configuration = WKSnapshotConfiguration()
    configuration.rect = visible
    let longest = max(visible.width, visible.height) * view.traitCollection.displayScale
    let outScale = min(1, Self.screenshotMaxEdge / longest)
    // In points; the image has the screen's scale.
    configuration.snapshotWidth = NSNumber(value: Double(visible.width * outScale))
    guard let image = try? await view.takeSnapshot(configuration: configuration),
      let jpeg = image.jpegData(compressionQuality: 0.8)
    else { return .error("No screenshot.") }
    let width = Int(image.size.width * image.scale)
    let height = Int(image.size.height * image.scale)
    // CSS pixels across the visible page (zoom included).
    let cssWidth = (try? await view.evaluateJavaScript("window.visualViewport ? visualViewport.width : innerWidth", in: nil, contentWorld: ClaudeChat.world)) as? Double
    screenshotScale = Double(width) / (cssWidth ?? Double(visible.width))
    return Result(content: [
      ["type": "image", "source": ["type": "base64", "media_type": "image/jpeg", "data": .string(jpeg.base64EncodedString())]],
      ["type": "text", "text": .string("Screenshot of the visible page, \(width)x\(height). Use this image's pixels for coordinates.")],
    ])
  }

  // MARK: Console and JavaScript

  private func readConsole(_ view: WKWebView, input: JSONValue) async -> Result {
    let onlyNew = input["only_new"].map { $0 == true } ?? true
    let limit = Int(min(max(input["limit"]?.number ?? 100, 1), 500))
    let levels = input["levels"]?.array?.compactMap(\.string) ?? []
    let pattern = input["pattern"]?.string.flatMap { try? NSRegularExpression(pattern: $0) }
    if input["pattern"]?.string != nil, pattern == nil { return .error("Bad pattern.") }
    let value = try? await view.evaluateJavaScript(ConsoleCapture.readScript, in: nil, contentWorld: .page)
    guard let text = value as? String, let object = JSONValue.parse(text) else {
      return .text("No console messages (the page may have loaded before Claude could listen).")
    }
    let total = Int(object["total"]?.number ?? 0)
    var entries = object["entries"]?.array ?? []
    let first = total - entries.count
    if onlyNew { entries = Array(entries.dropFirst(max(0, consoleRead - first))) }
    consoleRead = total
    entries = entries.filter { entry in
      let level = entry["level"]?.string ?? "log"
      let text = entry["text"]?.string ?? ""
      return (levels.isEmpty || levels.contains(level))
        && (pattern?.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil || pattern == nil)
    }
    if entries.isEmpty { return .text(onlyNew ? "No new console messages." : "No console messages.") }
    var out = ""
    let skipped = max(0, entries.count - limit)
    if skipped > 0 { out += "\(skipped) earlier messages left out.\n" }
    for entry in entries.suffix(limit) {
      out += "[\(entry["level"]?.string ?? "log")] \(entry["text"]?.string ?? "")"
      if let source = entry["source"]?.string, !source.isEmpty { out += "  (\(source))" }
      out += "\n"
    }
    return .text(out)
  }

  private func runJS(_ view: WKWebView, input: JSONValue) async -> Result {
    guard let code = input["code"]?.string, !code.isEmpty else { return .error("No code.") }
    do {
      let value = try await view.evaluateJavaScript(code, in: nil, contentWorld: .page)
      var text: String
      if let value, JSONSerialization.isValidJSONObject(value),
        let data = try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
      {
        text = String(decoding: data, as: UTF8.self)
      } else if let value {
        text = value is String ? "\"\(value)\"" : "\(value)"
      } else {
        text = "undefined"
      }
      if text.count > Self.maxJSChars { text = String(text.prefix(Self.maxJSChars)) + "\n… (cut)" }
      return .text(text)
    } catch {
      let message = (error as NSError).userInfo["WKJavaScriptExceptionMessage"] as? String ?? error.localizedDescription
      return .error("Uncaught: \(message)")
    }
  }

  // MARK: Navigation

  private func navigate(_ node: SidebarNode, input: JSONValue) async -> Result {
    guard let app, let where_ = input["url"]?.string?.trimmingCharacters(in: .whitespaces), !where_.isEmpty else {
      return .error("No URL.")
    }
    let view = app.tabs.webView(for: node)
    switch where_ {
    case "back":
      guard view.canGoBack else { return .error("There's no page to go back to.") }
      view.goBack()
    case "forward":
      guard view.canGoForward else { return .error("There's no page to go forward to.") }
      view.goForward()
    case "reload":
      view.reload()
    default:
      guard let url = URL(string: where_), ["http", "https"].contains(url.scheme?.lowercased() ?? "") else {
        return .error("Only http(s) URLs.")
      }
      view.load(URLRequest(url: url))
    }
    // Until it has loaded (15 s at most), and a moment to settle.
    let deadline = Date().addingTimeInterval(15)
    try? await Task.sleep(for: .milliseconds(300))
    while view.isLoading, Date() < deadline { try? await Task.sleep(for: .milliseconds(200)) }
    try? await Task.sleep(for: .milliseconds(300))
    reset()
    return .text("Now at \(view.url?.absoluteString ?? where_)\(view.title.map { " (\($0))" } ?? "").")
  }
}

/// The page's console, kept for read_console: a document-start script in
/// the page world (main frame) wraps console.*, `error` and
/// `unhandledrejection`, and keeps the last 500 messages.
@MainActor
enum ConsoleCapture {
  static let script = WKUserScript(source: source, injectionTime: .atDocumentStart, forMainFrameOnly: true, in: .page)

  private static let source = """
    (() => {
      if (window.__compassConsole) return;
      const entries = [];
      let total = 0;
      const text = (args) => args.map(a => {
        if (typeof a === 'string') return a;
        if (a instanceof Error) return a.stack || String(a);
        try { return JSON.stringify(a); } catch { return String(a); }
      }).join(' ').slice(0, 2000);
      const add = (level, message, source) => {
        entries.push({level, text: message, source: source || ''});
        if (entries.length > 500) entries.shift();
        total++;
      };
      for (const [method, level] of [['debug', 'debug'], ['log', 'log'], ['info', 'info'], ['warn', 'warning'], ['error', 'error']]) {
        const original = console[method];
        console[method] = function (...args) {
          try { add(level, text(args)); } catch {}
          return original.apply(this, args);
        };
      }
      addEventListener('error', e => add('error', 'Uncaught ' + (e.error && e.error.stack || e.message), e.filename ? e.filename + ':' + e.lineno : ''));
      addEventListener('unhandledrejection', e => add('error', 'Unhandled rejection: ' + text([e.reason])));
      Object.defineProperty(window, '__compassConsole', {
        value: () => JSON.stringify({total, entries}), enumerable: false,
      });
    })();
    """

  static let readScript = "window.__compassConsole ? window.__compassConsole() : null"
}
