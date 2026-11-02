// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// "Compass Browser" in the share sheet: a link to open in a space, or to
/// add to a space's Pinned (in a folder). It leaves a request in the App
/// Group's Inbox, which the app carries out (docs/design/ios.md, "System
/// extensions").
final class ShareViewController: UIViewController {
  private let model = ShareModel()

  override func viewDidLoad() {
    super.viewDidLoad()
    model.finish = { [weak self] open in self?.finish(open: open) }
    let host = UIHostingController(rootView: ShareView(model: model))
    addChild(host)
    host.view.frame = view.bounds
    host.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    view.addSubview(host.view)
    host.didMove(toParent: self)
    Task { await load() }
  }

  /// The shared page: a URL attachment, else the first URL in shared text.
  private func load() async {
    let items = extensionContext?.inputItems.compactMap { $0 as? NSExtensionItem } ?? []
    var title = items.lazy.compactMap { $0.attributedContentText?.string }.first ?? ""
    var url: URL?
    for provider in items.flatMap({ $0.attachments ?? [] }) {
      if provider.hasItemConformingToTypeIdentifier(UTType.url.identifier),
        let found = try? await provider.loadItem(forTypeIdentifier: UTType.url.identifier) as? URL,
        found.scheme?.hasPrefix("http") == true
      {
        url = found
        break
      }
      if provider.hasItemConformingToTypeIdentifier(UTType.plainText.identifier),
        let text = try? await provider.loadItem(forTypeIdentifier: UTType.plainText.identifier) as? String
      {
        if let found = Self.firstURL(in: text) {
          url = found
          if title.isEmpty {
            title = text.replacingOccurrences(of: found.absoluteString, with: "").trimmingCharacters(in: .whitespacesAndNewlines)
          }
          break
        }
      }
    }
    if title == url?.absoluteString { title = "" }
    await MainActor.run {
      model.url = url
      model.title = title
      model.loaded = true
    }
  }

  static func firstURL(in text: String) -> URL? {
    let detector = try? NSDataDetector(types: NSTextCheckingResult.CheckingType.link.rawValue)
    let range = NSRange(text.startIndex..., in: text)
    return detector?.matches(in: text, range: range).lazy.compactMap(\.url)
      .first { $0.scheme == "http" || $0.scheme == "https" }
  }

  private func finish(open: Bool) {
    if open { openApp(DeepLink.inbox.url) }
    extensionContext?.completeRequest(returningItems: nil)
  }

  /// Brings Compass up. Extensions may not open apps, but the share sheet's
  /// host is a UIApplication up the responder chain; its openURL is called
  /// by selector (the Swift API is unavailable to extensions). If iOS
  /// refuses, the request waits in the Inbox until Compass next comes up.
  private func openApp(_ url: URL) {
    var responder: UIResponder? = self
    let selector = NSSelectorFromString("openURL:options:completionHandler:")
    while let current = responder {
      if current.isKind(of: UIApplication.self), current.responds(to: selector),
        let method = current.method(for: selector)
      {
        typealias Open = @convention(c) (AnyObject, Selector, NSURL, NSDictionary, AnyObject?) -> Void
        unsafeBitCast(method, to: Open.self)(current, selector, url as NSURL, NSDictionary(), nil)
        return
      }
      responder = current.next
    }
  }
}

@MainActor
@Observable
final class ShareModel {
  enum Action: String, CaseIterable, Identifiable {
    case open = "Open"
    case pin = "Add to Pinned"
    var id: String { rawValue }
  }

  private static let lastSpaceKey = "ShareLastSpace"
  private static let lastActionKey = "ShareLastAction"

  var url: URL?
  var title = ""
  var loaded = false
  let snapshot: Snapshot?
  var action: Action
  var spaceID: String?
  /// nil = Pinned's top level.
  var folderID: String?
  var done: String?
  var finish: ((Bool) -> Void)?

  init() {
    let snapshot = SharedContainer.group?.readSnapshot()
    self.snapshot = snapshot
    let defaults = SharedContainer.defaults
    let last = defaults.string(forKey: Self.lastSpaceKey)
    let focus = SharedContainer.group?.readFocus()?.spaceID
    spaceID = (snapshot?.space(id: focus) ?? snapshot?.space(id: last) ?? snapshot?.currentSpace)?.id
    action = snapshot == nil ? .open : Action(rawValue: defaults.string(forKey: Self.lastActionKey) ?? "") ?? .open
  }

  var space: SpaceInfo? { snapshot?.space(id: spaceID) }

  func select(_ space: SpaceInfo) {
    guard space.id != spaceID else { return }
    spaceID = space.id
    folderID = nil
  }

  func confirm() {
    guard let url, let container = SharedContainer.group else { return }
    let item = InboxItem(
      action: action == .open ? .open : .pin, url: url.absoluteString, title: title, spaceID: spaceID,
      folderID: action == .pin ? folderID : nil)
    do {
      try container.inbox.add(item)
    } catch {
      done = "Couldn't save the link."
      return
    }
    SharedContainer.postChanged()
    let defaults = SharedContainer.defaults
    defaults.set(spaceID, forKey: Self.lastSpaceKey)
    defaults.set(action.rawValue, forKey: Self.lastActionKey)
    switch action {
    case .open:
      finish?(true)
    case .pin:
      let place = space?.folder(id: folderID)?.name ?? space?.name ?? "Pinned"
      done = "Added to \(place)"
      Task {
        try? await Task.sleep(for: .milliseconds(900))
        finish?(false)
      }
    }
  }
}
