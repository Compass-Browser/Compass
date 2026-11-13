// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import CompassModel
import Network
import UIKit
import WebKit

/// Claude's developer tools on iOS (docs/design/claude.md, "Developer
/// tools", iOS). WKWebView has no DevTools protocol, so they're made of:
/// - a capture script in the page world (`DevToolsScripts.capture`): the
///   page's fetch, XHR, beacons, sockets and event listeners;
/// - read-only scripts in Claude's content world: styles, timings, storage;
/// - WebKit's cookie store and web view settings;
/// - Compass's local proxy (`LocalProxy`): every connection of the
///   profiles' website data, and network emulation.
/// All of it only while "Let Claude Use Developer Tools" is on; never for
/// Incognito, which Claude can't use.
@MainActor
final class DevToolsNetwork {
  static let shared = DevToolsNetwork()

  let proxy = LocalProxy()
  /// The capture script's reader on `window`: random per run, so a page
  /// can't count on it.
  let key = "__compassDevTools" + UUID().uuidString.replacingOccurrences(of: "-", with: "").prefix(12)
  private(set) var port: UInt16?
  /// The data stores seen, to take the proxy off them again.
  private var stores: [WKWebsiteDataStore] = []
  private let controllers = NSHashTable<WKUserContentController>.weakObjects()
  private lazy var script = WKUserScript(
    source: DevToolsScripts.capture(key: key), injectionTime: .atDocumentStart, forMainFrameOnly: true, in: .page)

  private var isOn: Bool { ClaudeSettings.shared.devtools }

  /// The proxy's user name for a store: which data the connection is for.
  static func label(_ store: WKWebsiteDataStore) -> String { store.identifier?.uuidString.lowercased() ?? "home" }

  /// The setting's state on `stores` and `views` (TabManager.applyDevTools).
  func apply(stores: [WKWebsiteDataStore], views: [WKWebView]) {
    for store in stores where !self.stores.contains(where: { $0 === store }) { self.stores.append(store) }
    if isOn {
      for view in views { addCapture(to: view.configuration.userContentController) }
      Task {
        guard let port = try? await proxy.start(), self.isOn else { return }
        self.port = port
        for store in self.stores { self.configure(store) }
      }
    } else {
      port = nil
      for store in self.stores { store.proxyConfigurations = [] }
      proxy.stop()
    }
  }

  /// Sends `store`'s connections through the proxy while it runs. Failover
  /// is allowed: if the proxy stops answering, pages still load.
  func configure(_ store: WKWebsiteDataStore) {
    if !stores.contains(where: { $0 === store }) { stores.append(store) }
    guard isOn, let port, let endpointPort = NWEndpoint.Port(rawValue: port) else { return }
    var configuration = ProxyConfiguration(
      httpCONNECTProxy: .hostPort(host: "127.0.0.1", port: endpointPort), tlsOptions: nil)
    configuration.applyCredential(username: Self.label(store), password: proxy.password)
    configuration.allowFailover = true
    store.proxyConfigurations = [configuration]
  }

  /// The capture script, from the controller's next load. A tab that was
  /// open when the setting went off keeps it until it's made again (the
  /// tools refuse meanwhile).
  func addCapture(to controller: WKUserContentController) {
    guard isOn, !controllers.contains(controller) else { return }
    controllers.add(controller)
    controller.addUserScript(script)
  }

  /// `store`'s connections opened since `since`.
  func connections(for store: WKWebsiteDataStore, since: Date?) -> [NetworkListing.Connection] {
    proxy.connections(store: Self.label(store)).filter { since == nil || $0.opened >= since! }
  }

  var isRunning: Bool { port != nil }
}

/// What emulate changed on a tab, to undo it.
struct Emulation {
  weak var view: WKWebView?
  var store: String
  var userAgent: String?
  var style: UIUserInterfaceStyle
  var zoom: CGFloat
  var network = false
  /// What's emulated, as the answer lists it, in the order set.
  var parts: [(String, String)] = []

  mutating func set(_ what: String, _ how: String) {
    parts.removeAll { $0.0 == what }
    parts.append((what, how))
  }
}

extension ClaudeTools {
  /// The desktop's device presets: a viewport width and a user agent.
  private static let devices: [String: (label: String, width: Double, userAgent: String)] = [
    "iphone": (
      "iPhone", 393,
      "Mozilla/5.0 (iPhone; CPU iPhone OS 19_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/19.0 Mobile/15E148 Safari/604.1"
    ),
    "android": (
      "Android phone", 412,
      "Mozilla/5.0 (Linux; Android 15; Pixel 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.0.0 Mobile Safari/537.36"
    ),
    "ipad": (
      "iPad", 820,
      "Mozilla/5.0 (iPad; CPU OS 19_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/19.0 Mobile/15E148 Safari/604.1"
    ),
    "laptop": (
      "laptop", 1366,
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/19.0 Safari/605.1.15"
    ),
  ]
  static let notAllowed = "The user didn't allow it."

  /// network_requests, network_response, inspect_element, page_storage,
  /// edit_page_storage, performance, emulate (the setting is checked).
  func runDevTool(_ name: String, node: SidebarNode, view: WKWebView, input: JSONValue) async -> Result {
    guard let app else { return .error("No window.") }
    let store = app.tabs.dataStore(of: node)
    switch name {
    case "network_requests": return await networkRequests(view, store: store, input: input)
    case "network_response": return await networkResponse(view, store: store, input: input)
    case "inspect_element": return await inspectElement(view, input: input)
    case "page_storage": return await pageStorage(view, store: store, input: input)
    case "edit_page_storage": return await editPageStorage(view, store: store, input: input)
    case "performance": return .text(DevToolsReports.performance(await claudeJSON(view, DevToolsScripts.performance)))
    case "emulate": return emulate(view, store: store, input: input)
    default: return .error("\(name) isn't available on iOS.")
    }
  }

  // MARK: Scripts

  /// Runs `body` in Claude's content world; its JSON answer.
  private func claudeJSON(_ view: WKWebView, _ body: String, _ arguments: [String: Any] = [:]) async -> JSONValue? {
    let value = try? await view.callAsyncJavaScript(body, arguments: arguments, in: nil, contentWorld: ClaudeChat.world)
    return (value as? String).flatMap(JSONValue.parse)
  }

  /// The page's own requests, and whether the capture script is there.
  private func pageRequests(_ view: WKWebView) async -> (requests: [NetworkListing.PageRequest], captured: Bool) {
    let key = DevToolsNetwork.shared.key
    let value = try? await view.evaluateJavaScript(DevToolsScripts.readRequests(key: key), in: nil, contentWorld: .page)
    guard let text = value as? String, let log = JSONValue.parse(text) else { return ([], false) }
    return ((log["entries"]?.array ?? []).compactMap(NetworkListing.PageRequest.init), true)
  }

  /// Everything network_requests and network_response merge.
  private func networkSources(_ view: WKWebView, store: WKWebsiteDataStore) async -> (
    requests: [NetworkListing.PageRequest], captured: Bool, resources: [NetworkListing.Resource],
    connections: [NetworkListing.Connection]
  ) {
    let page = await pageRequests(view)
    let timings = await claudeJSON(view, DevToolsScripts.resources)
    let resources = NetworkListing.resources(from: timings, besides: page.requests)
    // The connections since the page started loading (a second's margin).
    let since = timings?["origin"]?.number.map { Date(timeIntervalSince1970: $0 / 1000 - 1) }
    let connections = DevToolsNetwork.shared.connections(for: store, since: since)
    return (page.requests, page.captured, resources, connections)
  }

  // MARK: Network

  private func networkRequests(_ view: WKWebView, store: WKWebsiteDataStore, input: JSONValue) async -> Result {
    var filter = NetworkListing.Filter()
    filter.onlyNew = input["only_new"] == .bool(true)
    filter.failed = input["failed"] == .bool(true)
    filter.types = Set(input["types"]?.array?.compactMap(\.string) ?? [])
    filter.limit = Int(min(max(input["limit"]?.number ?? 50, 1), 200))
    if let pattern = input["url"]?.string, !pattern.isEmpty {
      do {
        filter.pattern = try NSRegularExpression(pattern: pattern)
      } catch {
        return .error("Bad pattern: \(error.localizedDescription)")
      }
    }
    let sources = await networkSources(view, store: store)
    var seen = networkSeen
    let text = NetworkListing.list(
      requests: sources.requests, resources: sources.resources, connections: sources.connections,
      captured: sources.captured, proxyOn: DevToolsNetwork.shared.isRunning, filter: filter, seen: &seen)
    networkSeen = seen
    return .text(text)
  }

  private func networkResponse(_ view: WKWebView, store: WKWebsiteDataStore, input: JSONValue) async -> Result {
    guard let id = input["id"]?.string, !id.isEmpty else { return .error("No id: network_requests gives them.") }
    let sources = await networkSources(view, store: store)
    let answer = NetworkListing.response(
      id: id, requests: sources.requests, resources: sources.resources, connections: sources.connections,
      includeBody: input["body"] != .bool(false), offset: Int(max(0, input["offset"]?.number ?? 0)),
      showSecrets: input["show_secrets"] == .bool(true))
    return answer.isError ? .error(answer.text) : .text(answer.text)
  }

  // MARK: Elements

  private func inspectElement(_ view: WKWebView, input: JSONValue) async -> Result {
    var selector = input["selector"]?.string
    if let ref = input["ref"]?.string, !ref.isEmpty {
      let found = await agent(view, "selector", ["ref": .string(ref)])
      if found.isError { return found }
      selector = found.content.first?["text"]?.string.flatMap(JSONValue.parse)?["selector"]?.string
    }
    guard let selector, !selector.isEmpty else { return .error("Give the element's ref or a CSS selector.") }
    let properties = input["properties"]?.array?.compactMap(\.string) ?? []
    guard
      let inspected = await claudeJSON(
        view, DevToolsScripts.inspect, ["selector": selector, "properties": properties])
    else { return .error("The page didn't answer (it may still be loading).") }
    if let error = inspected["error"]?.string { return .error(error) }
    let key = DevToolsNetwork.shared.key
    let listeners = (try? await view.callAsyncJavaScript(
      DevToolsScripts.readListeners(key: key), arguments: ["selector": selector], in: nil, contentWorld: .page))
      .flatMap { $0 as? String }.flatMap(JSONValue.parse)
    return .text(
      DevToolsReports.inspect(
        inspected, selector: selector, listeners: listeners, userAgent: input["user_agent"] == .bool(true)))
  }

  // MARK: Storage

  private static func origin(_ url: URL) -> String {
    var origin = "\(url.scheme ?? "https")://\(url.host ?? "")"
    if let port = url.port { origin += ":\(port)" }
    return origin
  }

  private static func cookie(_ cookie: HTTPCookie) -> DevToolsFormat.Cookie {
    DevToolsFormat.Cookie(
      name: cookie.name, value: cookie.value, domain: cookie.domain, path: cookie.path,
      expires: cookie.isSessionOnly ? nil : cookie.expiresDate, httpOnly: cookie.isHTTPOnly, secure: cookie.isSecure,
      sameSite: cookie.sameSitePolicy.map { $0.rawValue.prefix(1).uppercased() + $0.rawValue.dropFirst() })
  }

  private func pageStorage(_ view: WKWebView, store: WKWebsiteDataStore, input: JSONValue) async -> Result {
    guard let url = view.url, let host = url.host else { return .error("The tab has no page.") }
    let request = DevToolsReports.StorageRequest(
      store: input["store"]?.string, filter: input["filter"]?.string, showValues: input["show_values"] == .bool(true))
    var cookies: [DevToolsFormat.Cookie]?
    if request.cookies {
      cookies = await store.httpCookieStore.allCookies().map(Self.cookie).filter { $0.applies(to: host) }
        .sorted { $0.name < $1.name }
    }
    var storage: JSONValue?
    if request.local || request.session {
      let read = await agent(view, "storage", .object([:]))
      if !read.isError { storage = read.content.first?["text"]?.string.flatMap(JSONValue.parse) }
    }
    let indexedDB = request.indexedDB ? await claudeJSON(view, DevToolsScripts.indexedDBNames) : nil
    return .text(
      DevToolsReports.storage(
        origin: Self.origin(url), request: request, cookies: cookies, storage: storage, indexedDB: indexedDB))
  }

  private func editPageStorage(_ view: WKWebView, store: WKWebsiteDataStore, input: JSONValue) async -> Result {
    let kind = input["store"]?.string ?? ""
    let action = input["action"]?.string ?? ""
    let name = input["name"]?.string ?? ""
    guard ["cookies", "localStorage", "sessionStorage"].contains(kind) else {
      return .error("store must be cookies, localStorage or sessionStorage.")
    }
    guard ["set", "remove", "clear"].contains(action) else { return .error("action must be set, remove or clear.") }
    if action != "clear" && name.isEmpty { return .error("\(action) needs a name.") }
    if action == "set" && input["value"]?.string == nil { return .error("set needs a value.") }
    guard let url = view.url, let host = url.host, ["http", "https"].contains(url.scheme ?? "") else {
      return .error("Only web pages (http, https) have cookies.")
    }
    let value = input["value"]?.string ?? ""
    // What the user approves, as the desktop words it.
    let title: String
    if kind == "cookies" {
      title = action == "set" ? "Set the cookie “\(name)” on \(host)"
        : action == "remove" ? "Delete the cookie “\(name)” on \(host)" : "Delete all cookies of \(host)"
    } else {
      let where_ = " in \(host)\(kind == "localStorage" ? "'s local storage" : "'s session storage")"
      title = action == "set" ? "Set “\(name)”\(where_)" : action == "remove" ? "Remove “\(name)”\(where_)" : "Clear everything\(where_)"
    }
    var detail = action == "set" ? DevToolsFormat.cut(value, 2000) : ""
    if action == "set", kind == "cookies", let attributes = input["cookie"], attributes.object?.isEmpty == false {
      detail += "\n" + String(decoding: attributes.encoded(), as: UTF8.self)
    }
    guard await approve?(Approval(tool: "edit_page_storage", title: title, detail: detail)) == true else {
      return .error(Self.notAllowed)
    }
    guard view.url?.host == host else { return .error("The page changed while the user was asked; nothing was changed.") }

    if kind != "cookies" {
      let edited = await agent(
        view, "editStorage",
        ["store": .string(kind), "action": .string(action), "name": .string(name), "value": .string(value)])
      if edited.isError { return edited }
      return .text(
        action == "set" ? "Set “\(name)” in \(kind)." : action == "remove" ? "Removed “\(name)” from \(kind)." : "Cleared \(kind).")
    }
    let cookieStore = store.httpCookieStore
    let matching = await cookieStore.allCookies().filter { Self.cookie($0).applies(to: host) }
    switch action {
    case "set":
      let attributes = input["cookie"]
      var properties: [HTTPCookiePropertyKey: Any] = [
        .name: name, .value: value, .domain: attributes?["domain"]?.string ?? host,
        .path: attributes?["path"]?.string ?? "/",
      ]
      if let expires = attributes?["expires"]?.number { properties[.expires] = Date(timeIntervalSince1970: expires) }
      if attributes?["secure"] == .bool(true) { properties[.secure] = "TRUE" }
      if attributes?["http_only"] == .bool(true) { properties[HTTPCookiePropertyKey("HttpOnly")] = "TRUE" }
      switch attributes?["same_site"]?.string {
      case "Strict": properties[.sameSitePolicy] = HTTPCookieStringPolicy.sameSiteStrict
      case "Lax": properties[.sameSitePolicy] = HTTPCookieStringPolicy.sameSiteLax
      case "None": properties[.sameSitePolicy] = "none"
      default: break
      }
      guard let cookie = HTTPCookie(properties: properties) else { return .error("Those cookie attributes aren't valid.") }
      await cookieStore.setCookie(cookie)
      return .text("Set the cookie “\(name)”.")
    case "remove":
      let gone = matching.filter { $0.name == name }
      if gone.isEmpty { return .error("\(host) has no cookie named “\(name)”.") }
      for cookie in gone { await cookieStore.deleteCookie(cookie) }
      return .text("Deleted the cookie “\(name)”.")
    default:
      for cookie in matching { await cookieStore.deleteCookie(cookie) }
      return .text("Deleted \(matching.count) \(matching.count == 1 ? "cookie" : "cookies") of \(host).")
    }
  }

  // MARK: Emulation

  private func emulate(_ view: WKWebView, store: WKWebsiteDataStore, input: JSONValue) -> Result {
    if input["reset"] == .bool(true) {
      let had = emulation != nil
      endEmulation()
      return .text(had ? "Emulation undone: the tab is back to normal." : "Nothing was emulated.")
    }
    if emulation?.view !== view { endEmulation() }
    var state = emulation ?? Emulation(
      view: view, store: DevToolsNetwork.label(store), userAgent: view.customUserAgent,
      style: view.overrideUserInterfaceStyle, zoom: view.pageZoom)
    var unavailable: [String] = []
    var newAgent = false
    var width = input["width"]?.number
    if let device = input["device"]?.string {
      guard let preset = Self.devices[device] else { return .error("Unknown device \(device).") }
      view.customUserAgent = preset.userAgent
      newAgent = true
      width = width ?? preset.width
      state.set("device", "\(preset.label) (user agent and width)")
    }
    if let width {
      // WebKit lays the page out in bounds / pageZoom CSS pixels.
      let zoom = min(max(view.bounds.width / CGFloat(width), 0.25), 5)
      view.pageZoom = zoom
      let actual = Int((view.bounds.width / zoom).rounded())
      state.set("viewport", "\(actual) CSS pixels wide, by zoom (the height follows the screen)")
    }
    if input["height"] != nil { unavailable.append("a height of its own (it follows the screen)") }
    if input["scale"] != nil { unavailable.append("the pixel ratio") }
    if input["mobile"] != nil { unavailable.append("mobile and touch (iOS is always a touch screen)") }
    if let agent = input["user_agent"]?.string {
      view.customUserAgent = agent.isEmpty ? state.userAgent : agent
      newAgent = true
      state.set("user agent", DevToolsFormat.cut(agent, 120))
    }
    if let scheme = input["color_scheme"]?.string {
      view.overrideUserInterfaceStyle = scheme == "dark" ? .dark : .light
      state.set("color scheme", scheme)
    }
    if input["reduced_motion"] != nil { unavailable.append("reduced motion (a system setting on iOS)") }
    if input["media"]?.string == "print" { unavailable.append("print media") }
    if let network = input["network"]?.string {
      if DevToolsNetwork.shared.isRunning {
        DevToolsNetwork.shared.proxy.setConditions(LocalProxy.Conditions.preset(network), store: state.store)
        state.network = network != "none"
        if network == "none" {
          state.parts.removeAll { $0.0 == "network" }
        } else {
          state.set("network", "\(network), for every tab of this profile (on iOS Compass's proxy slows the profile's new connections, not one tab's)")
        }
      } else {
        unavailable.append("network conditions (Compass's proxy isn't running)")
      }
    }
    if input["cpu_slowdown"] != nil { unavailable.append("a slower CPU") }
    emulation = state
    var text = state.parts.isEmpty
      ? "Nothing emulated."
      : "Emulating: " + state.parts.map { "\($0.0): \($0.1)" }.joined(separator: "; ") + "."
    if !unavailable.isEmpty { text += "\nUnavailable on iOS: " + unavailable.joined(separator: "; ") + "." }
    if newAgent { text += "\nThe user agent applies to new requests: navigate with \"reload\" to load the page with it." }
    text += "\nIt's undone when the chat stops, a minute after your last tool, or with reset."
    return .text(text)
  }

  /// Puts the tab back as it was: user agent, color scheme, zoom, network.
  func endEmulation() {
    emulationEnd?.cancel()
    emulationEnd = nil
    guard let state = emulation else { return }
    emulation = nil
    if let view = state.view {
      view.customUserAgent = state.userAgent
      view.overrideUserInterfaceStyle = state.style
      view.pageZoom = state.zoom
    }
    if state.network { DevToolsNetwork.shared.proxy.setConditions(nil, store: state.store) }
  }

  /// A minute after Claude's last tool, emulation ends.
  func scheduleEmulationEnd() {
    guard emulation != nil else { return }
    emulationEnd?.cancel()
    emulationEnd = Task { [weak self] in
      try? await Task.sleep(for: .seconds(60))
      guard !Task.isCancelled else { return }
      self?.endEmulation()
    }
  }
}
