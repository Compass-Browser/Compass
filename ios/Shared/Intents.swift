// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Built into the app and the Widgets extension (project.yml): the spaces as
// App Intents entities, and the intents widgets, controls, the Focus filter
// and Shortcuts run. Intents that change the sidebar leave a request in the
// Inbox, so they work the same whichever process runs them.

import AppIntents
import CompassShared
import Foundation

extension SharedContainer {
  /// The App Group's, else (an unsigned build) the app's own folder, as
  /// SystemBridge uses.
  static var current: SharedContainer {
    group
      ?? SharedContainer(
        root: FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
          .appendingPathComponent("Compass/Shared", isDirectory: true))
  }
}

/// Set by the app: runs a link in it. Nil in an extension's process.
enum IntentRouter {
  @MainActor static var handle: ((DeepLink) -> Void)?
  @MainActor static var cancelDownload: ((String) -> Void)?
}

// MARK: Spaces

struct SpaceEntity: AppEntity, Identifiable {
  static let typeDisplayRepresentation = TypeDisplayRepresentation(name: "Space")
  static let defaultQuery = SpaceQuery()

  let id: String
  @Property(title: "Name") var name: String

  init(id: String, name: String) {
    self.id = id
    self.name = name
  }

  init(_ space: SpaceInfo) { self.init(id: space.id, name: space.name) }

  var displayRepresentation: DisplayRepresentation { DisplayRepresentation(title: "\(name)") }
}

struct SpaceQuery: EntityQuery {
  private var spaces: [SpaceInfo] { SharedContainer.current.readSnapshot()?.spaces ?? [] }

  func entities(for identifiers: [SpaceEntity.ID]) async throws -> [SpaceEntity] {
    spaces.filter { identifiers.contains($0.id) }.map(SpaceEntity.init)
  }

  func suggestedEntities() async throws -> [SpaceEntity] { spaces.map(SpaceEntity.init) }

  func defaultResult() async -> SpaceEntity? {
    SharedContainer.current.readSnapshot()?.currentSpace.map(SpaceEntity.init)
  }
}

// MARK: Opening the app

/// Opens the app on a link (the widgets' buttons, the Control Center control).
private func open(_ link: DeepLink) async {
  await MainActor.run { IntentRouter.handle?(link) }
}

struct NewTabIntent: AppIntent {
  static let title: LocalizedStringResource = "New Compass Tab"
  static let description = IntentDescription("Opens Compass on a new tab.")
  static let openAppWhenRun = true

  func perform() async throws -> some IntentResult {
    await open(.newTab(space: nil))
    return .result()
  }
}

struct OpenSpaceIntent: AppIntent {
  static let title: LocalizedStringResource = "Open Space"
  static let description = IntentDescription("Opens Compass on a space's sidebar.")
  static let openAppWhenRun = true

  @Parameter(title: "Space") var space: SpaceEntity

  init() {}
  init(space: SpaceEntity) { self.space = space }

  func perform() async throws -> some IntentResult {
    await open(.space(space.id))
    return .result()
  }
}

/// A `compass:` link, as a button's intent: small widgets have no links, only
/// one tap area or intent buttons.
struct OpenLinkIntent: AppIntent {
  static let title: LocalizedStringResource = "Open Compass Link"
  static let openAppWhenRun = true
  static let isDiscoverable = false

  @Parameter(title: "Link") var link: String

  init() {}
  init(_ link: DeepLink) { self.link = link.url.absoluteString }

  func perform() async throws -> some IntentResult {
    if let url = URL(string: link), let target = DeepLink(url: url) { await open(target) }
    return .result()
  }
}
