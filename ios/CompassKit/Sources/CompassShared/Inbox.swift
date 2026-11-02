// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// A request from an extension for the app: open a link, or add it to a
/// space's Pinned. Only the app changes the sidebar (so the change syncs
/// like any other); extensions leave these in the Inbox.
public struct InboxItem: Codable, Equatable, Sendable, Identifiable {
  public enum Action: String, Codable, Sendable {
    /// A new Today tab in the space, shown.
    case open
    /// A new Pinned tab in the space, in `folderID` if given.
    case pin
  }

  public var id: String
  public var action: Action
  public var url: String
  public var title: String
  /// nil = the space shown last.
  public var spaceID: String?
  public var folderID: String?
  public var date: Date

  public init(
    id: String = UUID().uuidString.lowercased(), action: Action, url: String, title: String = "",
    spaceID: String? = nil, folderID: String? = nil, date: Date = Date()
  ) {
    self.id = id
    self.action = action
    self.url = url
    self.title = title
    self.spaceID = spaceID
    self.folderID = folderID
    self.date = date
  }
}

/// Requests waiting for the app: one JSON file each, so an extension and the
/// app never write the same file.
public struct Inbox: Sendable {
  public let directory: URL

  public init(directory: URL) {
    self.directory = directory
  }

  public func add(_ item: InboxItem) throws {
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let data = try JSONEncoder.shared.encode(item)
    try data.write(to: directory.appendingPathComponent(item.id + ".json"), options: .atomic)
  }

  /// Whether anything waits.
  public var isEmpty: Bool { files().isEmpty }

  /// Takes every waiting request, the oldest first. Unreadable files are
  /// dropped.
  public func drain() -> [InboxItem] {
    var items: [InboxItem] = []
    for file in files() {
      if let data = try? Data(contentsOf: file), let item = try? JSONDecoder.shared.decode(InboxItem.self, from: data) {
        items.append(item)
      }
      try? FileManager.default.removeItem(at: file)
    }
    return items.sorted { $0.date < $1.date }
  }

  private func files() -> [URL] {
    let names = (try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? []
    return names.filter { $0.pathExtension == "json" }
  }
}

extension JSONEncoder {
  static let shared: JSONEncoder = {
    let e = JSONEncoder()
    e.dateEncodingStrategy = .secondsSince1970
    e.outputFormatting = [.sortedKeys]
    return e
  }()
}

extension JSONDecoder {
  static let shared: JSONDecoder = {
    let d = JSONDecoder()
    d.dateDecodingStrategy = .secondsSince1970
    return d
  }()
}
