// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AppIntents
import CompassModel
import Foundation

// Compass's actions in the Shortcuts app. Only in the app target: they run
// in the app's process (in the background unless they open it) and work on
// the sidebar itself, so they see every entry and their changes sync.

/// A Pinned or Today entry: a tab or a folder.
struct TabEntity: AppEntity, Identifiable {
  static let typeDisplayRepresentation = TypeDisplayRepresentation(name: "Compass Tab")
  static let defaultQuery = TabQuery()

  let id: String
  @Property(title: "Title") var title: String
  /// Empty for a folder.
  @Property(title: "URL") var url: String
  @Property(title: "Is Folder") var isFolder: Bool
  /// The folders it's in, outermost first, joined by " / "; empty at the top.
  @Property(title: "Folder") var folder: String
  @Property(title: "Space") var space: String
  @Property(title: "Section") var section: String

  init(id: String, title: String, url: String, isFolder: Bool, folder: String, space: String, section: String) {
    self.id = id
    self.title = title
    self.url = url
    self.isFolder = isFolder
    self.folder = folder
    self.space = space
    self.section = section
  }

  var displayRepresentation: DisplayRepresentation {
    DisplayRepresentation(
      title: "\(title)", subtitle: "\(isFolder ? "Folder" : url)",
      image: .init(systemName: isFolder ? "folder" : "globe"))
  }

  @MainActor init(_ node: SidebarNode, in space: Space) {
    var folders: [String] = []
    var parent = node.parent
    while let p = parent, !p.isRoot {
      if p.isFolder { folders.insert(p.title, at: 0) }
      parent = p.parent
    }
    self.init(
      id: node.id, title: node.isFolder ? node.title : node.displayTitle, url: node.isFolder ? "" : node.url,
      isFolder: node.isFolder, folder: folders.joined(separator: " / "), space: space.name,
      section: node.section == .pinned ? "Pinned" : "Today")
  }

  /// A section's tabs and folders, depth first (a split as its panes).
  @MainActor static func all(in root: SidebarNode, of space: Space) -> [TabEntity] {
    root.children.flatMap(\.subtree).filter { $0.isTab || $0.isFolder }.map { TabEntity($0, in: space) }
  }
}

struct TabQuery: EntityQuery {
  @MainActor func entities(for identifiers: [TabEntity.ID]) async throws -> [TabEntity] {
    guard let store = AppServices.shared?.store else { return [] }
    return identifiers.compactMap { id in
      guard let node = store.node(id: id), let space = store.space(of: node) else { return nil }
      return TabEntity(node, in: space)
    }
  }
}

enum ShortcutError: Error, CustomLocalizedStringResourceConvertible {
  case notRunning
  case noSpace
  case notWeb

  var localizedStringResource: LocalizedStringResource {
    switch self {
    case .notRunning: "Compass isn't ready. Open it once and try again."
    case .noSpace: "That space isn't in Compass anymore."
    case .notWeb: "Only http and https links can go in Compass."
    }
  }
}

@MainActor private func resolve(_ entity: SpaceEntity) throws -> (AppModel, Space) {
  guard let app = AppDelegate.app else { throw ShortcutError.notRunning }
  guard let space = app.store.space(id: entity.id) else { throw ShortcutError.noSpace }
  return (app, space)
}

private func checkWeb(_ url: URL) throws {
  guard let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https" else {
    throw ShortcutError.notWeb
  }
}

struct AddToSpaceIntent: AppIntent {
  static let title: LocalizedStringResource = "Add to Space"
  static let description = IntentDescription(
    "Adds a web page to a space: to its Pinned, or as a Today tab. Compass doesn't open.")

  @Parameter(title: "URL") var url: URL
  @Parameter(title: "Space") var space: SpaceEntity
  @Parameter(title: "Pinned", description: "Off: add it to Today.", default: true) var pinned: Bool
  @Parameter(title: "Title", default: "") var title: String

  static var parameterSummary: some ParameterSummary {
    Summary("Add \(\.$url) to \(\.$space)") {
      \.$pinned
      \.$title
    }
  }

  @MainActor func perform() async throws -> some IntentResult & ReturnsValue<TabEntity> {
    try checkWeb(url)
    let (app, space) = try resolve(space)
    guard let node = app.store.addTab(url: url.absoluteString, title: title, to: pinned ? space.pinned : space.today)
    else { throw ShortcutError.noSpace }
    // Saved now: in the background the app may be suspended any moment.
    try? app.store.save(to: app.sidebarURL)
    app.system.writeSnapshot()
    return .result(value: TabEntity(node, in: space))
  }
}

struct OpenURLInSpaceIntent: AppIntent {
  static let title: LocalizedStringResource = "Open URL in Space"
  static let description = IntentDescription("Opens a web page in a new tab of a space.")
  static let openAppWhenRun = true

  @Parameter(title: "URL") var url: URL
  @Parameter(title: "Space") var space: SpaceEntity

  static var parameterSummary: some ParameterSummary { Summary("Open \(\.$url) in \(\.$space)") }

  @MainActor func perform() async throws -> some IntentResult {
    try checkWeb(url)
    let (app, space) = try resolve(space)
    app.openLink(url, in: space)
    return .result()
  }
}

struct GetPinnedTabsIntent: AppIntent {
  static let title: LocalizedStringResource = "Get Pinned Tabs"
  static let description = IntentDescription("Every tab and folder in a space's Pinned, folders' contents included.")

  @Parameter(title: "Space") var space: SpaceEntity

  static var parameterSummary: some ParameterSummary { Summary("Get pinned tabs of \(\.$space)") }

  @MainActor func perform() async throws -> some IntentResult & ReturnsValue<[TabEntity]> {
    let (_, space) = try resolve(space)
    return .result(value: TabEntity.all(in: space.pinned, of: space))
  }
}

struct GetTodayTabsIntent: AppIntent {
  static let title: LocalizedStringResource = "Get Today Tabs"
  static let description = IntentDescription("Every tab and folder in a space's Today, newest first.")

  @Parameter(title: "Space") var space: SpaceEntity

  static var parameterSummary: some ParameterSummary { Summary("Get today tabs of \(\.$space)") }

  @MainActor func perform() async throws -> some IntentResult & ReturnsValue<[TabEntity]> {
    let (_, space) = try resolve(space)
    // Today's top level is newest first, as the sidebar shows it.
    let newestFirst = space.today.children.reversed().flatMap { $0.subtree }.filter { $0.isTab || $0.isFolder }
    return .result(value: newestFirst.map { TabEntity($0, in: space) })
  }
}

struct GetSpacesIntent: AppIntent {
  static let title: LocalizedStringResource = "Get Spaces"
  static let description = IntentDescription("Every space in Compass, in the sidebar's order.")

  @MainActor func perform() async throws -> some IntentResult & ReturnsValue<[SpaceEntity]> {
    guard let app = AppDelegate.app else { throw ShortcutError.notRunning }
    return .result(value: app.store.spaces.map { SpaceEntity(id: $0.id, name: $0.name) })
  }
}
