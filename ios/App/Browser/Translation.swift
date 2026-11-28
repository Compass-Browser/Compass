// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassTranslate
import SwiftUI
@preconcurrency import Translation
import WebKit
import os

private let log = Logger(subsystem: "it.cipi1965.compass", category: "translate")

/// Page translation on iOS (docs/design/translation.md, "iOS"): the shared
/// `page_translator.js` in Compass's own content world finds the page's
/// text and writes translations back into its nodes; its engine posts each
/// batch here, and Apple's Translation framework translates it on the
/// device. The page's language comes from its `lang` and its text
/// (`PageLanguage`); a page in a language the user doesn't read gets the
/// translate button in the address pill.
@MainActor @Observable
final class PageTranslation {
  static let world = WKContentWorld.world(name: "CompassTranslate")
  static let handler = "compassTranslate"

  enum Step: Equatable {
    /// In another language: the button offers to translate.
    case offered
    case translating
    case translated
    case failed
  }

  struct State: Equatable {
    var step: Step
    /// Normalized codes (`PageLanguage`).
    var source: String
    var target: String
    /// The page it's about.
    var url: URL
  }

  /// By tab; a tab with nothing to offer has none.
  private(set) var states: [String: State] = [:]
  let prefs = TranslationPrefs()

  /// Languages Apple translates on this device, for Translate To (filled
  /// once, on first use).
  private(set) var languages: [String] = []

  func state(_ id: String, _ view: WKWebView?) -> State? {
    guard let state = states[id], view?.url == state.url else { return nil }
    return state
  }

  /// A page loaded: its language, and whether to offer or (Always
  /// Translate) translate it.
  func check(_ id: String, _ view: WKWebView) {
    guard let url = view.url, TranslateScripts.isWebPage(url), Self.isAvailable else {
      states[id] = nil
      return
    }
    // Already about this page (a reload of a translated page starts over).
    if let state = states[id], state.url == url, state.step != .translated { return }
    Task {
      guard let sample = await TranslateScripts.sample(view), view.url == url else { return }
      let preferred = Locale.preferredLanguages
      guard let source = PageLanguage.detect(langAttribute: sample.lang, text: sample.text),
        prefs.shouldOffer(source: source, url: url, preferred: preferred)
      else {
        states[id] = nil
        return
      }
      let target = prefs.always[source] ?? PageLanguage.target(source: source, preferred: preferred)
      states[id] = State(step: .offered, source: source, target: target, url: url)
      // Debug: `-CompassTranslate YES` translates every offered page.
      #if DEBUG
        let debug = UserDefaults.standard.bool(forKey: "CompassTranslate")
      #else
        let debug = false
      #endif
      if prefs.always[source] != nil || debug { translate(id, view) }
    }
  }

  /// Another URL in the tab: nothing until it's checked.
  func pageChanged(_ id: String, _ view: WKWebView) {
    if let state = states[id], state.url != view.url { states[id] = nil }
  }

  /// Translates the tab's page into `target` (the state's when nil),
  /// starting from the original if it's translated already.
  func translate(_ id: String, _ view: WKWebView, to target: String? = nil) {
    guard var state = state(id, view) else { return }
    if let target { state.target = target }
    state.step = .translating
    states[id] = state
    let (source, url) = (state.source, state.url)
    Task {
      let ok = await TranslateScripts.start(view, source: source, target: state.target)
      guard view.url == url, states[id]?.target == state.target else { return }
      log.info("translate \(source, privacy: .public)→\(state.target, privacy: .public): \(ok ? "ok" : "failed", privacy: .public)")
      states[id]?.step = ok ? .translated : .failed
    }
  }

  /// Back to the page as it was.
  func showOriginal(_ id: String, _ view: WKWebView) {
    guard state(id, view) != nil else { return }
    states[id]?.step = .offered
    Task { await TranslateScripts.restore(view) }
  }

  func alwaysTranslate(_ id: String, _ view: WKWebView, _ on: Bool) {
    guard let state = state(id, view) else { return }
    prefs.setAlways(state.source, on ? state.target : nil)
    if on, state.step == .offered { translate(id, view) }
  }

  func neverTranslate(_ id: String, _ view: WKWebView, site: Bool) {
    guard let state = state(id, view) else { return }
    if site { prefs.setNeverSite(state.url) } else { prefs.setNever(state.source) }
    if state.step == .translated || state.step == .translating { Task { await TranslateScripts.restore(view) } }
    states[id] = nil
  }

  func isAlways(_ state: State) -> Bool { prefs.always[state.source] != nil }

  /// The languages Translate To offers: Apple's, by name.
  func loadLanguages() async {
    guard languages.isEmpty, #available(iOS 18.0, *) else { return }
    let codes = Set(await AppleTranslator.supportedCodes().compactMap(PageLanguage.normalize))
    languages = codes.sorted { Self.name($0).localizedCompare(Self.name($1)) == .orderedAscending }
  }

  static func name(_ code: String) -> String {
    Locale.current.localizedString(forIdentifier: code) ?? code
  }

  /// Apple's Translation framework: iOS 18 and later. Bergamot, for
  /// iOS 17 and the languages Apple lacks, comes later (T6b).
  static var isAvailable: Bool {
    if #available(iOS 18.0, *) { return true }
    return false
  }

  /// A batch from the page's engine: `{source, target, segments: [{text,
  /// html}]}`, answered with the translations (null for each that
  /// failed), or an error the page records.
  func received(_ body: Any) async -> (Any?, String?) {
    guard let message = body as? [String: Any],
      let source = message["source"] as? String, let target = message["target"] as? String,
      let raw = message["segments"] as? [[String: Any]]
    else { return (nil, "bad message") }
    let segments = raw.map {
      PlainTextBatch.Segment(text: $0["text"] as? String ?? "", html: $0["html"] as? Bool ?? false)
    }
    let batch = PlainTextBatch(segments)
    // Debug: `-CompassTranslateFake YES` upper-cases instead (the
    // simulator has no Apple Translation), to check the page and the UI.
    #if DEBUG
      if UserDefaults.standard.bool(forKey: "CompassTranslateFake") {
        return (batch.translations(from: batch.strings.map { $0.uppercased() }).map { $0 as Any? ?? NSNull() }, nil)
      }
    #endif
    guard #available(iOS 18.0, *) else { return (nil, "unsupported") }
    do {
      let results = try await AppleTranslator.shared.translate(batch.strings, from: source, to: target)
      return (batch.translations(from: results).map { $0 as Any? ?? NSNull() }, nil)
    } catch AppleTranslator.Failure.unsupported {
      log.error("\(source, privacy: .public)→\(target, privacy: .public): unsupported")
      return (nil, "unsupported")
    } catch {
      log.error("\(source, privacy: .public)→\(target, privacy: .public): \(String(describing: error), privacy: .public)")
      return (nil, "failed")
    }
  }
}

/// `page_translator.js` in the tabs' pages, in the translation world.
@MainActor
enum TranslateScripts {
  private static let source: String = {
    guard let url = Bundle.main.url(forResource: "page_translator", withExtension: "js"),
      let text = try? String(contentsOf: url, encoding: .utf8)
    else { return "" }
    return text
  }()

  static func isWebPage(_ url: URL) -> Bool { ["http", "https"].contains(url.scheme?.lowercased() ?? "") }

  /// Defines `__compassTranslator` in the world (once per document: the
  /// script returns early when it's there).
  private static func define(in view: WKWebView) async -> Bool {
    guard !source.isEmpty else { return false }
    return (try? await view.evaluateJavaScript(source + "\n;true", in: nil, contentWorld: PageTranslation.world))
      as? Bool == true
  }

  static func sample(_ view: WKWebView) async -> (lang: String, text: String)? {
    guard await define(in: view) else { return nil }
    let value = try? await view.callAsyncJavaScript(
      "return __compassTranslator.sample(2000);", arguments: [:], in: nil, contentWorld: PageTranslation.world)
    guard let sample = value as? [String: Any] else { return nil }
    return (sample["lang"] as? String ?? "", sample["text"] as? String ?? "")
  }

  /// Starts translating, and waits for the first batch (the first screen):
  /// true when something was translated, false when nothing could be.
  static func start(_ view: WKWebView, source: String, target: String) async -> Bool {
    guard await define(in: view) else { return false }
    let value = try? await view.callAsyncJavaScript(
      """
      const engine = segments => window.webkit.messageHandlers.\(PageTranslation.handler)
          .postMessage({source, target, segments});
      __compassTranslator.start({source, target, engine});
      await __compassTranslator.started();
      const {done, failed} = __compassTranslator.progress();
      return done > 0 || failed === 0;
      """,
      arguments: ["source": source, "target": target], in: nil, contentWorld: PageTranslation.world)
    return value as? Bool == true
  }

  static func restore(_ view: WKWebView) async {
    _ = try? await view.callAsyncJavaScript(
      "if (globalThis.__compassTranslator) __compassTranslator.restore();",
      arguments: [:], in: nil, contentWorld: PageTranslation.world)
  }
}

/// Apple's Translation framework. A session only lives inside SwiftUI's
/// `.translationTask` (iOS 18–25), so a hidden `TranslationHost` in each
/// window runs one for the pair asked for, and serves the batches queued
/// here until none comes for 30 seconds. A language iOS hasn't downloaded
/// asks the user in the system's own sheet. The session stays in the
/// host's task; only the strings cross to the main actor.
@available(iOS 18.0, *)
@MainActor @Observable
final class AppleTranslator {
  static let shared = AppleTranslator()

  enum Failure: Error { case unsupported, failed }

  /// What the hosts run: set for a new pair, invalidated to run again.
  var configuration: TranslationSession.Configuration?

  struct Pair: Hashable, Sendable {
    let source: String
    let target: String
  }

  /// A batch for a host: its id and strings.
  struct Work: Sendable {
    let id: Int
    let strings: [String]
  }

  private struct Job {
    let work: Work
    let pair: Pair
    let done: CheckedContinuation<[String?], Error>
  }

  @ObservationIgnored private var queue: [Job] = []
  @ObservationIgnored private var running: [Int: Job] = [:]
  @ObservationIgnored private var nextID = 0
  /// Hosts waiting for work, by the pair they serve.
  @ObservationIgnored private var idle: [(token: Int, pair: Pair, wake: CheckedContinuation<Work?, Never>)] = []
  @ObservationIgnored private var servedPairs: [Pair: Int] = [:]

  /// LanguageAvailability isn't Sendable: asked off the main actor.
  nonisolated static func supportedCodes() async -> [String] {
    await LanguageAvailability().supportedLanguages.map(\.minimalIdentifier)
  }

  nonisolated static func supports(_ source: String, _ target: String) async -> Bool {
    await LanguageAvailability().status(
      from: Locale.Language(identifier: source), to: Locale.Language(identifier: target)) != .unsupported
  }

  static func pair(_ configuration: TranslationSession.Configuration?) -> Pair? {
    guard let source = configuration?.source, let target = configuration?.target else { return nil }
    return Pair(
      source: PageLanguage.normalize(source.minimalIdentifier) ?? source.minimalIdentifier,
      target: PageLanguage.normalize(target.minimalIdentifier) ?? target.minimalIdentifier)
  }

  func translate(_ strings: [String], from source: String, to target: String) async throws -> [String?] {
    if strings.isEmpty { return [] }
    if await !Self.supports(source, target) { throw Failure.unsupported }
    let pair = Pair(source: source, target: target)
    return try await withCheckedThrowingContinuation { done in
      nextID += 1
      let job = Job(work: Work(id: nextID, strings: strings), pair: pair, done: done)
      if let index = idle.firstIndex(where: { $0.pair == pair }) {
        let host = idle.remove(at: index)
        running[job.work.id] = job
        host.wake.resume(returning: job.work)
        return
      }
      queue.append(job)
      if (servedPairs[pair] ?? 0) > 0 { return }  // a host will take it
      if Self.pair(configuration) == pair {
        configuration?.invalidate()
      } else {
        configuration = TranslationSession.Configuration(
          source: Locale.Language(identifier: source), target: Locale.Language(identifier: target))
      }
    }
  }

  /// The next batch for a host serving `pair`: one queued, or one that
  /// comes within 30 seconds; nil to end its session.
  func next(_ pair: Pair) async -> Work? {
    if let index = queue.firstIndex(where: { $0.pair == pair }) {
      let job = queue.remove(at: index)
      running[job.work.id] = job
      return job.work
    }
    // Another pair is wanted now: let this session go.
    if Self.pair(configuration) != pair { return nil }
    nextID += 1
    let token = nextID
    return await withCheckedContinuation { (wake: CheckedContinuation<Work?, Never>) in
      idle.append((token, pair, wake))
      Task { @MainActor in
        try? await Task.sleep(for: .seconds(30))
        // Still idle (the same waiter): end.
        if let index = self.idle.firstIndex(where: { $0.token == token }) {
          self.idle.remove(at: index).wake.resume(returning: nil)
        }
      }
    }
  }

  func finish(_ id: Int, _ results: Result<[String?], Error>, pair: Pair) {
    guard let job = running.removeValue(forKey: id) else { return }
    job.done.resume(with: results)
    if case .failure(let error) = results {
      // The user turned the download down, or the session broke: the
      // rest of this pair's batches fail too.
      for other in queue where other.pair == pair { other.done.resume(throwing: error) }
      queue.removeAll { $0.pair == pair }
    }
  }

  func hostStarted(_ pair: Pair) {
    log.info("session for \(pair.source, privacy: .public)→\(pair.target, privacy: .public)")
    servedPairs[pair, default: 0] += 1
  }
  func hostEnded(_ pair: Pair) {
    servedPairs[pair, default: 1] -= 1
    // Work that came while it was ending: run again.
    if servedPairs[pair] == 0, queue.contains(where: { $0.pair == pair }) {
      if Self.pair(configuration) == pair { configuration?.invalidate() }
    }
  }
}

/// A host's session, serving its pair until there's nothing more.
@available(iOS 18.0, *)
nonisolated func serveTranslations(_ session: TranslationSession, pair: AppleTranslator.Pair) async {
  await AppleTranslator.shared.hostStarted(pair)
  while let work = await AppleTranslator.shared.next(pair) {
    let requests = work.strings.enumerated().map {
      TranslationSession.Request(sourceText: $0.element, clientIdentifier: String($0.offset))
    }
    let result: Result<[String?], Error>
    do {
      let responses = try await session.translations(from: requests)
      var results = [String?](repeating: nil, count: work.strings.count)
      for response in responses {
        if let index = response.clientIdentifier.flatMap(Int.init), results.indices.contains(index) {
          results[index] = response.targetText
        }
      }
      result = .success(results)
    } catch {
      result = .failure(error)
    }
    await AppleTranslator.shared.finish(work.id, result, pair: pair)
    if case .failure = result { break }
  }
  await AppleTranslator.shared.hostEnded(pair)
}

/// Runs Apple's sessions, invisibly, in a window (BrowserScreen).
struct TranslationHost: View {
  var body: some View {
    if #available(iOS 18.0, *) {
      AppleTranslationHost()
    }
  }
}

@available(iOS 18.0, *)
private struct AppleTranslationHost: View {
  @State private var translator = AppleTranslator.shared

  var body: some View {
    let configuration = translator.configuration
    let pair = AppleTranslator.pair(configuration)
    Color.clear
      .frame(width: 0, height: 0)
      .allowsHitTesting(false)
      .accessibilityHidden(true)
      .translationTask(configuration) { session in
        guard let pair else { return }
        await serveTranslations(session, pair: pair)
      }
  }
}

/// Forwards the page engine's batches, with replies, without the content
/// controller retaining the tab manager.
final class TranslateMessageProxy: NSObject, WKScriptMessageHandlerWithReply {
  weak var target: TabManager?
  init(_ target: TabManager) { self.target = target }

  @MainActor
  func userContentController(
    _ controller: WKUserContentController, didReceive message: WKScriptMessage
  ) async -> (Any?, String?) {
    guard let handle = target?.onTranslateMessage else { return (nil, "no translator") }
    return await handle(message.body)
  }
}

extension AppModel {
  /// The shown tab's translation, if its page is offered or translated.
  var shownTranslation: PageTranslation.State? {
    let _ = services.translation.states
    guard let tab = shownTab else { return nil }
    return services.translation.state(tab.id, tabs.live[tab.id])
  }

  /// The button's tap: translate, or (translated, failed) nothing: its menu
  /// does the rest.
  func translateShown(to target: String? = nil) {
    guard let tab = shownTab, let view = tabs.live[tab.id] else { return }
    services.translation.translate(tab.id, view, to: target)
  }

  func showOriginalShown() {
    guard let tab = shownTab, let view = tabs.live[tab.id] else { return }
    services.translation.showOriginal(tab.id, view)
  }

  /// ⌘⇧Y and the page menu's tile: translate, or back to the original.
  func toggleTranslation() {
    guard let state = shownTranslation else { return }
    if state.step == .translated || state.step == .translating { showOriginalShown() } else { translateShown() }
  }

  func alwaysTranslateShown(_ on: Bool) {
    guard let tab = shownTab, let view = tabs.live[tab.id] else { return }
    services.translation.alwaysTranslate(tab.id, view, on)
  }

  func neverTranslateShown(site: Bool) {
    guard let tab = shownTab, let view = tabs.live[tab.id] else { return }
    services.translation.neverTranslate(tab.id, view, site: site)
  }
}

/// The translate button at the address's start (BrowserScreen): offered,
/// a tap translates; busy, a spinner; translated, filled, with its menu.
struct TranslateButton: View {
  @Environment(AppModel.self) private var app
  let state: PageTranslation.State
  let tint: Color

  var body: some View {
    let translation = app.services.translation
    Menu {
      Section(header(state)) {
        Menu {
          ForEach(translation.languages.filter { PageLanguage.base($0) != PageLanguage.base(state.source) }, id: \.self) { code in
            Button {
              app.translateShown(to: code)
            } label: {
              if code == state.target && state.step == .translated {
                Label(PageTranslation.name(code), systemImage: "checkmark")
              } else {
                Text(PageTranslation.name(code))
              }
            }
          }
        } label: {
          Label(String(localized: "Translate To", comment: "Translate menu: a submenu of languages to translate the page into."), systemImage: "globe")
        }
      }
      if state.step == .translated || state.step == .translating {
        Button(String(localized: "Show Original", comment: "Translate menu: put the page back in its own language."), systemImage: "arrow.uturn.backward") {
          app.showOriginalShown()
        }
      } else if state.step == .failed {
        Button(String(localized: "Try Again", comment: "Translate menu: translate the page again after it failed."), systemImage: "arrow.clockwise") {
          app.translateShown()
        }
      }
      Section {
        Toggle(isOn: Binding(get: { translation.isAlways(state) }, set: { app.alwaysTranslateShown($0) })) {
          Text(String(localized: "Always Translate \(PageTranslation.name(state.source))", comment: "Translate menu: translate pages in this language without asking. The argument is a language name."))
        }
        Button(String(localized: "Never Translate \(PageTranslation.name(state.source))", comment: "Translate menu: don't offer to translate pages in this language any more. The argument is a language name.")) {
          app.neverTranslateShown(site: false)
        }
        Button(String(localized: "Never Translate This Site", comment: "Translate menu: don't offer to translate this site's pages any more.")) {
          app.neverTranslateShown(site: true)
        }
      }
    } label: {
      Group {
        if state.step == .translating {
          ProgressView().controlSize(.small)
        } else {
          Image(systemName: "translate")
            .font(.system(size: 16, weight: .semibold))
            .symbolVariant(state.step == .translated ? .fill : .none)
            .foregroundStyle(color)
        }
      }
      .frame(width: 40, height: 44)
      .contentShape(Rectangle())
    } primaryAction: {
      switch state.step {
      case .offered: app.translateShown()
      case .failed: app.translateShown()
      case .translating, .translated: app.showOriginalShown()
      }
    }
    .padding(.leading, 2)
    .accessibilityLabel(label)
    .accessibilityIdentifier("BarTranslate")
    .task { await translation.loadLanguages() }
  }

  private var color: Color {
    switch state.step {
    case .translated: tint
    case .failed: .red
    default: .primary
    }
  }

  private var label: String {
    let language = PageTranslation.name(state.source)
    switch state.step {
    case .offered: return String(localized: "Translate from \(language)", comment: "Address bar's translate button: translate the page. The argument is a language name.")
    case .translating: return String(localized: "Translating…", comment: "Address bar's translate button while the page translates.")
    case .translated: return String(localized: "Translated from \(language)", comment: "Address bar's translate button on a translated page. The argument is a language name.")
    case .failed: return String(localized: "This page couldn't be translated", comment: "Address bar's translate button after translating failed.")
    }
  }

  private func header(_ state: PageTranslation.State) -> String {
    let language = PageTranslation.name(state.source)
    if state.step == .translated {
      return String(localized: "Translated from \(language)", comment: "Address bar's translate button on a translated page. The argument is a language name.")
    }
    return String(localized: "Page in \(language)", comment: "Translate menu header: the language the page is in. The argument is a language name.")
  }
}
