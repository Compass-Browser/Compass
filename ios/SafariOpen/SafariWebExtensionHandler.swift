// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import Foundation
import SafariServices

/// The Safari extension's native side (Resources/popup.js):
///   {action: "spaces"}                       → {spaces: [{id, name, accent}]}
///   {action: "open"|"pin", url, title, spaceID} → {ok}
/// "open" and "pin" leave a request in Compass's Inbox.
final class SafariWebExtensionHandler: NSObject, NSExtensionRequestHandling {
  func beginRequest(with context: NSExtensionContext) {
    let item = context.inputItems.first as? NSExtensionItem
    let message = item?.userInfo?[SFExtensionMessageKey] as? [String: Any] ?? [:]
    let response = NSExtensionItem()
    response.userInfo = [SFExtensionMessageKey: Self.reply(to: message)]
    context.completeRequest(returningItems: [response])
  }

  static func reply(to message: [String: Any]) -> [String: Any] {
    guard let container = SharedContainer.group else { return ["ok": false] }
    switch message["action"] as? String {
    case "spaces":
      let spaces = container.readSnapshot()?.spaces ?? []
      return [
        "spaces": spaces.map {
          ["id": $0.id, "name": $0.name, "accent": String(format: "#%06X", $0.style.accent)]
        }
      ]
    case let action? where action == "open" || action == "pin":
      guard let url = message["url"] as? String, url.hasPrefix("http") else { return ["ok": false] }
      let item = InboxItem(
        action: action == "open" ? .open : .pin, url: url, title: message["title"] as? String ?? "",
        spaceID: message["spaceID"] as? String)
      guard (try? container.inbox.add(item)) != nil else { return ["ok": false] }
      SharedContainer.postChanged()
      return ["ok": true]
    default:
      return ["ok": false]
    }
  }
}
