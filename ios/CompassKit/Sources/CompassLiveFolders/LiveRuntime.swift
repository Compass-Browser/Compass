// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import JavaScriptCore

/// Runs live folder plugins in JavaScriptCore, a fresh context per refresh
/// (docs/design/live-folders.md, "Runtime"; the reference is
/// tools/live_folders/runtime.mjs). The context loads the web globals JSC
/// lacks, the SDK, the prelude and the plugin's entry; the host defines
/// `__compassHost` first and enforces every limit itself.
public struct LiveRuntime: Sendable {
  public let scripts: LiveScripts

  public init(scripts: LiveScripts) {
    self.scripts = scripts
  }

  /// Headers a plugin can't set: the host owns them (and `sec-*`).
  static let forbiddenHeaders: Set<String> = [
    "cookie", "cookie2", "host", "origin", "referer", "content-length", "connection", "keep-alive",
    "transfer-encoding", "upgrade", "te", "proxy-authorization", "set-cookie",
  ]
  static let methods: Set<String> = ["GET", "HEAD", "POST"]

  /// One refresh. `configJSON` is the folder's config (a JSON object);
  /// `secrets` this device's values (undeclared ones are dropped);
  /// `grantCookies` whether the user let this folder send cookies.
  public func run(
    _ plugin: LivePlugin, configJSON: String, secrets: [String: String] = [:], grantCookies: Bool = false,
    environment: LiveEnvironment = LiveEnvironment(), limits: LiveLimits = LiveLimits(), fetcher: any LiveFetcher
  ) async -> LiveRun {
    let run = Run(
      plugin: plugin, scripts: scripts, configJSON: configJSON, secrets: secrets, grantCookies: grantCookies,
      environment: environment, limits: limits, fetcher: fetcher)
    return await withCheckedContinuation { continuation in
      run.start { continuation.resume(returning: $0) }
    }
  }
}

/// One refresh's state. Everything JavaScript happens on `queue`; the
/// timeout fires from elsewhere, so a plugin stuck in a loop still ends
/// the refresh (its thread is then left to it).
private final class Run: @unchecked Sendable {
  let plugin: LivePlugin
  let scripts: LiveScripts
  let configJSON: String
  let secrets: [String: String]
  let grantCookies: Bool
  let environment: LiveEnvironment
  let limits: LiveLimits
  let fetcher: any LiveFetcher
  let queue = DispatchQueue(label: "compass.live-folder.run")

  private let lock = NSLock()
  private var done: ((LiveRun) -> Void)?
  private var fetches: [LiveRun.Fetch] = []
  private var log: [String] = []

  // On `queue` only.
  private var rules: LiveRules?
  private var context: JSContext?
  private var allowed = LiveAllowedHosts(hosts: [], cookies: [])
  private var fetchCount = 0
  private var timers: [Int: DispatchWorkItem] = [:]
  private var nextTimer = 1
  private var stringify: JSValue?
  private var parse: JSValue?
  private var promiseThen: JSValue?
  private var exception: JSValue?

  init(
    plugin: LivePlugin, scripts: LiveScripts, configJSON: String, secrets: [String: String], grantCookies: Bool,
    environment: LiveEnvironment, limits: LiveLimits, fetcher: any LiveFetcher
  ) {
    self.plugin = plugin
    self.scripts = scripts
    self.configJSON = configJSON
    self.secrets = secrets
    self.grantCookies = grantCookies
    self.environment = environment
    self.limits = limits
    self.fetcher = fetcher
  }

  func start(_ completion: @escaping (LiveRun) -> Void) {
    done = completion
    // Holds the run until then: nothing else does (the context's callbacks
    // hold it weakly).
    DispatchQueue.global().asyncAfter(deadline: .now() + limits.timeout) { [self] in
      finish(.failure(LiveError("timeout", "No result after \(Int(limits.timeout * 1000)) ms")))
    }
    queue.async { [self] in
      do {
        try begin()
      } catch let error as LiveError {
        finish(.failure(error))
      } catch {
        finish(.failure(LiveError("plugin_error", "\(error)")))
      }
    }
  }

  private var isFinished: Bool {
    lock.lock()
    defer { lock.unlock() }
    return done == nil
  }

  /// Ends the refresh, once; later calls (a late fetch, the timeout after a
  /// result) do nothing.
  private func finish(_ result: Result<LiveResult, LiveError>) {
    lock.lock()
    let completion = done
    done = nil
    let outcome = LiveRun(result: result, fetches: fetches, log: log)
    lock.unlock()
    guard let completion else { return }
    completion(outcome)
    queue.async { [self] in
      // Breaks the context's references back to this run.
      for timer in timers.values { timer.cancel() }
      timers.removeAll()
      context = nil
      stringify = nil
      parse = nil
      promiseThen = nil
      rules = nil
    }
  }

  // MARK: Setting up

  private func begin() throws {
    let rules = LiveRules(scripts: scripts)
    self.rules = rules
    let manifest = plugin.manifest
    let config = try rules.config(manifest, configJSON)
    let declared = Set(manifest.secrets.map(\.key))
    let own = secrets.filter { declared.contains($0.key) }
    for secret in manifest.secrets where !secret.optional && (own[secret.key] ?? "").isEmpty {
      throw LiveError("missing_secret", "\(secret.label) is not set")
    }
    allowed = try rules.hosts(manifest, config: config, secrets: own)

    guard let context = JSContext(virtualMachine: JSVirtualMachine()) else {
      throw LiveError("plugin_error", "No JavaScript context")
    }
    self.context = context
    context.name = "Live folder: \(manifest.id)"
    context.exceptionHandler = { [weak self] _, e in self?.exception = e }
    // Captured before any plugin code runs, which may replace them.
    let json = context.objectForKeyedSubscript("JSON")!
    stringify = json.forProperty("stringify")
    parse = json.forProperty("parse")
    promiseThen = context.objectForKeyedSubscript("Promise").forProperty("prototype").forProperty("then")

    try evaluate(scripts.globals, "jsc_globals.js")
    installTimers(in: context)
    try evaluate(Self.lockdown, "lockdown.js")
    context.setObject(host(in: context), forKeyedSubscript: "__compassHost" as NSString)
    for (name, source) in scripts.context { try evaluate(source, name) }
    try evaluate(plugin.source, manifest.entry)

    guard let configValue = call(parse, this: json, [config]),
      let promise = context.objectForKeyedSubscript("__compassRun").call(withArguments: [configValue])
    else {
      throw thrown() ?? LiveError("bad_plugin", "The plugin can't start")
    }
    if let error = thrown() { throw error }
    let resolved: @convention(block) (JSValue) -> Void = { [weak self] value in self?.resolved(value) }
    let rejected: @convention(block) (JSValue) -> Void = { [weak self] value in
      self?.finish(.failure(Self.error(from: value)))
    }
    _ = call(
      promiseThen, this: promise,
      [JSValue(object: resolved, in: context)!, JSValue(object: rejected, in: context)!])
  }

  /// No `eval` and no `new Function` (docs/design/live-folders.md, "The
  /// `compass` object"); JSC has no switch for it, so the constructors that
  /// compile strings are replaced. Not a boundary: the host enforces the
  /// rules that matter.
  static let lockdown = """
    (function (g) {
      'use strict';
      const blocked = function () { throw new EvalError('Code generation from strings is not allowed'); };
      blocked.prototype = Function.prototype;
      const fix = (o, k) => Object.defineProperty(o, k, {value: blocked, writable: false, configurable: false});
      for (const f of [function () {}, async function () {}, function* () {}, async function* () {}]) {
        fix(Object.getPrototypeOf(f), 'constructor');
      }
      fix(g, 'eval');
      fix(g, 'Function');
    })(globalThis);
    """

  private func evaluate(_ source: String, _ name: String) throws {
    context?.evaluateScript(source, withSourceURL: URL(string: "compass-live:///\(name)"))
    if let error = thrown() { throw error }
  }

  /// The exception the last call left, as a LiveError.
  private func thrown() -> LiveError? {
    guard let e = exception else { return nil }
    exception = nil
    return Self.error(from: e)
  }

  /// `errorOf` in the reference: the error's `code` if it has one, else
  /// plugin_error.
  static func error(from e: JSValue) -> LiveError {
    let codeValue = e.isObject ? e.forProperty("code") : nil
    let code = codeValue.flatMap { $0.isString ? $0.toString() : nil } ?? "plugin_error"
    let messageValue = e.isObject ? e.forProperty("message") : nil
    let message = messageValue.flatMap { $0.isString ? $0.toString() : nil } ?? e.toString() ?? code
    return LiveError(code, message)
  }

  /// Calls `function` with `this`, through the C API (a plugin can replace
  /// Function.prototype.call).
  private func call(_ function: JSValue?, this: JSValue, _ args: [Any]) -> JSValue? {
    guard let context, let function, function.isObject else { return nil }
    let ctx = context.jsGlobalContextRef
    let values = args.map { (JSValue(object: $0, in: context) ?? JSValue(undefinedIn: context)).jsValueRef }
    var exceptionRef: JSValueRef?
    let result = values.withUnsafeBufferPointer { buffer in
      JSObjectCallAsFunction(
        ctx, JSValueToObject(ctx, function.jsValueRef, nil), JSValueToObject(ctx, this.jsValueRef, nil),
        values.count, buffer.baseAddress, &exceptionRef)
    }
    if let exceptionRef {
      exception = JSValue(jsValueRef: exceptionRef, in: context)
      return nil
    }
    return result.map { JSValue(jsValueRef: $0, in: context) }
  }

  // MARK: The result

  private func resolved(_ value: JSValue) {
    guard !isFinished, let context, let rules else { return }
    // Out of the plugin's realm the way postMessage would take it.
    let raw = value.isUndefined ? JSValue(nullIn: context)! : value
    guard let json = call(stringify, this: context.objectForKeyedSubscript("JSON"), [raw]) else {
      finish(.failure(thrown() ?? LiveError("bad_result", "The result isn't JSON")))
      return
    }
    let text = json.isString ? json.toString()! : "null"
    do {
      finish(.success(try rules.result(text, maxItems: limits.maxItems)))
    } catch let error as LiveError {
      finish(.failure(error))
    } catch {
      finish(.failure(LiveError("bad_result", "\(error)")))
    }
  }

  // MARK: Host primitives

  /// `__compassHost`: fetch, log, env (plugins/runtime/prelude.js).
  private func host(in context: JSContext) -> JSValue {
    let host = JSValue(newObjectIn: context)!
    let fetch: @convention(block) (JSValue) -> JSValue = { [weak self] request in
      guard let self, let context = JSContext.current() else { return JSValue(undefinedIn: JSContext.current()) }
      return JSValue(newPromiseIn: context) { resolve, reject in
        self.fetch(request, resolve: resolve!, reject: reject!)
      }
    }
    let log: @convention(block) (JSValue) -> Void = { [weak self] message in
      guard let self else { return }
      lock.lock()
      if self.log.count < 200 { self.log.append(message.toString() ?? "") }
      lock.unlock()
    }
    host.setObject(fetch, forKeyedSubscript: "fetch" as NSString)
    host.setObject(log, forKeyedSubscript: "log" as NSString)
    let own = secrets.filter { key, _ in plugin.manifest.secrets.contains { $0.key == key } }
    host.setObject(
      [
        "now": (environment.now.timeIntervalSince1970 * 1000).rounded(), "locale": environment.locale,
        "timeZone": environment.timeZone, "secrets": own,
      ], forKeyedSubscript: "env" as NSString)
    return host
  }

  private func reject(_ reject: JSValue, _ code: String, _ message: String) {
    guard let context else { return }
    let error = JSValue(newErrorFromMessage: message, in: context)!
    error.setObject(code, forKeyedSubscript: "code" as NSString)
    reject.call(withArguments: [error])
  }

  /// `compass.fetch` after the prelude: checks the request, records it,
  /// and resolves with {status, url, headers, body}.
  private func fetch(_ request: JSValue, resolve: JSValue, reject: JSValue) {
    guard let rules else { return }
    if fetchCount >= limits.maxFetches {
      self.reject(reject, "too_many_fetches", "More than \(limits.maxFetches) fetches in one refresh")
      return
    }
    let rawURL = request.forProperty("url")?.toString() ?? ""
    guard let href = rules.href(rawURL), let url = URL(string: href) else {
      self.reject(reject, "bad_request", "Not a URL: \(rawURL)")
      return
    }
    guard rules.allowed(href, allowed.hosts) else {
      self.reject(reject, "host_not_allowed", "\(url.host() ?? href) is not in the plugin's hosts")
      return
    }
    let method = request.forProperty("method")?.toString() ?? "GET"
    guard LiveRuntime.methods.contains(method) else {
      self.reject(reject, "bad_request", "Method \(method) is not allowed")
      return
    }
    var headers: [String: String] = [:]
    if let given = request.forProperty("headers"), given.isObject,
      let dict = given.toDictionary() as? [String: Any]
    {
      for (name, value) in dict {
        let lower = name.lowercased()
        if LiveRuntime.forbiddenHeaders.contains(lower) || lower.hasPrefix("sec-") { continue }
        headers[lower] = "\(value)"
      }
    }
    let bodyValue = request.forProperty("body")
    let body = bodyValue.flatMap { $0.isNull || $0.isUndefined ? nil : $0.toString() }
    let withCookies =
      request.forProperty("credentials")?.toString() == "include" && grantCookies && allowed.allowsCookies(url)
    fetchCount += 1
    lock.lock()
    fetches.append(LiveRun.Fetch(method: method, url: href, cookies: withCookies))
    lock.unlock()

    let outgoing = LiveFetchRequest(method: method, url: url, headers: headers, body: body, withCookies: withCookies)
    let (fetcher, allowed, limits) = (self.fetcher, self.allowed, self.limits)
    // Held until the response: the run's queue is the only one using them.
    let callbacks = Unchecked((resolve, reject))
    Task {
      let outcome: Result<LiveFetchResponse, LiveError>
      do {
        let response = try await fetcher.fetch(outgoing, allowed: allowed, limits: limits)
        outcome =
          response.body.count > limits.maxResponseBytes
          ? .failure(
            LiveError(
              "response_too_large", "Response from \(url.host() ?? href) is over \(limits.maxResponseBytes) bytes"))
          : .success(response)
      } catch let error as LiveError {
        outcome = .failure(error)
      } catch {
        outcome = .failure(LiveError("network", error.localizedDescription))
      }
      self.queue.async {
        let (resolve, reject) = callbacks.value
        guard !self.isFinished, let context = self.context else { return }
        switch outcome {
        case .success(let response):
          let value: [String: Any] = [
            "status": response.status, "url": response.url.absoluteString, "headers": response.headers,
            "body": Self.decode(response.body, contentType: Self.header("content-type", in: response.headers)),
          ]
          resolve.call(withArguments: [JSValue(object: value, in: context)!])
        case .failure(let error):
          self.reject(reject, error.code, error.message)
        }
      }
    }
  }

  static func header(_ name: String, in headers: [String: String]) -> String? {
    headers.first { $0.key.lowercased() == name }?.value
  }

  /// The body as text, in the `charset` of Content-Type, UTF-8 otherwise.
  static func decode(_ data: Data, contentType: String?) -> String {
    if let contentType,
      let match = contentType.range(of: #"charset\s*=\s*"?([^";\s]+)"#, options: [.regularExpression, .caseInsensitive])
    {
      let label = contentType[match].split(separator: "=", maxSplits: 1).last.map {
        $0.trimmingCharacters(in: CharacterSet(charactersIn: "\" "))
      }
      if let label, label.lowercased() != "utf-8", label.lowercased() != "utf8" {
        let cf = CFStringConvertIANACharSetNameToEncoding(label as CFString)
        if cf != kCFStringEncodingInvalidId,
          let s = String(data: data, encoding: String.Encoding(rawValue: CFStringConvertEncodingToNSStringEncoding(cf)))
        {
          return s
        }
      }
    }
    // Invalid sequences become U+FFFD, as TextDecoder does; a BOM goes.
    var s = String(decoding: data, as: UTF8.self)
    if s.hasPrefix("\u{FEFF}") { s.removeFirst() }
    return s
  }

  // MARK: Timers

  /// setTimeout and clearTimeout, on the run's queue.
  private func installTimers(in context: JSContext) {
    let set: @convention(block) () -> Int = { [weak self] in
      guard let self, let args = JSContext.currentArguments() as? [JSValue], let fn = args.first,
        fn.isObject, !fn.isNull
      else { return 0 }
      let ms = args.count > 1 ? max(0, args[1].toDouble().isFinite ? args[1].toDouble() : 0) : 0
      let extra = Array(args.dropFirst(2))
      let id = nextTimer
      nextTimer += 1
      let callback = Unchecked((fn, extra))
      let item = DispatchWorkItem { [weak self] in
        guard let self else { return }
        self.timers[id] = nil
        guard !self.isFinished else { return }
        callback.value.0.call(withArguments: callback.value.1)
        _ = self.thrown()  // an uncaught error in a callback, as in a browser
      }
      timers[id] = item
      queue.asyncAfter(deadline: .now() + ms / 1000, execute: item)
      return id
    }
    let clear: @convention(block) (JSValue) -> Void = { [weak self] id in
      guard let self, id.isNumber else { return }
      self.timers.removeValue(forKey: Int(id.toInt32()))?.cancel()
    }
    context.setObject(set, forKeyedSubscript: "setTimeout" as NSString)
    context.setObject(clear, forKeyedSubscript: "clearTimeout" as NSString)
  }
}

/// A value only one queue uses, carried across a concurrency boundary.
private struct Unchecked<T>: @unchecked Sendable {
  let value: T
  init(_ value: T) { self.value = value }
}
