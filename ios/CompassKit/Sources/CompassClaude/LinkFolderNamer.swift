// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Observation

/// Names link folders with Claude (docs/design/ai-sidebar.md, "Names for
/// link folders"; the desktop's LinkFolderNamer): a link folder this device
/// made (`SidebarNode.linkFolderHere`, LinkFolders), in Today, whose name is
/// still its opener's or Claude's, is named from its entries' titles and
/// hosts when it reaches 3, 6 and 12 entries, 5 s after its last change.
/// Only with "Name Link Folders" on (and Claude set up: `ClaudeQuick`);
/// never in incognito. Also "Suggest a Name" for any folder, on request,
/// under the same switch.
@MainActor
@Observable
public final class LinkFolderNamer {
  /// The entry counts that ask for a name.
  public static let thresholds = [3, 6, 12]
  /// The most tabs a request lists.
  public static let maxTabs = 20
  /// A title this long (in UTF-8 bytes) says enough.
  static let maxTitleLength = 120
  /// The prompts file's section for these requests.
  public static let promptID = "folder_name"
  /// How long a folder Claude just named sparkles.
  public static let sparkleDuration: Duration = .milliseconds(1600)

  /// How long after a folder's last change it's named.
  @ObservationIgnored public var debounce: Duration = .seconds(5)
  /// Folders Claude just named: the sidebar shows a sparkle on them.
  public private(set) var sparkling: Set<String> = []

  let store: SpaceStore
  let quick: ClaudeQuick
  /// common/claude/sidebar_prompts.md's text.
  let prompts: String
  /// The model "Name Link Folders" asks.
  let model: @MainActor () -> String
  /// The browser's language, a BCP 47 tag ("it-IT").
  let locale: @MainActor () -> String
  /// The highest threshold each folder was asked at.
  @ObservationIgnored var askedAt: [String: Int] = [:]
  /// Folders waiting for their changes to settle.
  @ObservationIgnored var waiting: [String: Task<Void, Never>] = [:]
  /// Folders with a request on its way.
  @ObservationIgnored var asking: Set<String> = []

  public init(
    store: SpaceStore, quick: ClaudeQuick, prompts: String, model: @escaping @MainActor () -> String,
    locale: @escaping @MainActor () -> String = { Locale.preferredLanguages.first ?? "en-US" }
  ) {
    self.store = store
    self.quick = quick
    self.prompts = prompts
    self.model = model
    self.locale = locale
    store.localChangeObservers.append { [weak self] change in self?.changed(change) }
  }

  /// Whether Claude may be asked at all ("Name Link Folders" on, and Claude
  /// set up).
  public var isEnabled: Bool { quick.isEnabled(ClaudeQuick.nameLinkFoldersKey) }

  /// The folder menu's Undo Name: while its name is Claude's and the one
  /// before is known.
  public func canUndo(_ folder: SidebarNode) -> Bool {
    folder.isFolder && folder.nameSource == .claude && !folder.nameBeforeClaude.isEmpty
  }

  /// The folder menu's Suggest a Name (when Undo Name isn't there): any
  /// folder with entries but live folders, with the switch on, not in
  /// incognito.
  public func canSuggest(_ folder: SidebarNode) -> Bool {
    folder.isFolder && !folder.isLiveFolder && !folder.children.isEmpty && !store.isIncognito(folder)
      && store.contains(folder) && folder.section != .archive && isEnabled && !canUndo(folder)
  }

  /// Asks for a name for `folder` now, as the user did (Suggest a Name):
  /// the name, if any, is Claude's, with Undo Name.
  public func suggest(_ folder: SidebarNode) {
    guard canSuggest(folder) else { return }
    Task { await ask(folder.id, background: false) }
  }

  /// The user message for `folder`: its tabs' titles and hosts, a line each
  /// (the desktop's DescribeFolder()). Never paths, queries or what pages
  /// hold.
  public static func describe(_ folder: SidebarNode) -> String {
    var text = "The folder's tabs:\n"
    for tab in folder.tabs.prefix(maxTabs) {
      var title = (tab.customTitle.isEmpty ? tab.title : tab.customTitle)
        .replacingOccurrences(of: "\r", with: " ").replacingOccurrences(of: "\n", with: " ")
        .replacingOccurrences(of: "\t", with: " ")
        .trimmingCharacters(in: .whitespaces)
      if title.utf8.count > maxTitleLength {
        var cut = ""
        for character in title {
          if cut.utf8.count + String(character).utf8.count > maxTitleLength { break }
          cut.append(character)
        }
        title = cut + "\u{2026}"
      }
      let host = URL(string: tab.url)?.host() ?? ""
      if title.isEmpty && host.isEmpty { continue }
      text += "- " + (title.isEmpty ? host : title)
      if !host.isEmpty && !title.isEmpty { text += " (\(host))" }
      text += "\n"
    }
    return text
  }

  // MARK: Triggers

  /// Whether Claude may name `folder` on its own now.
  func isEligible(_ folder: SidebarNode) -> Bool {
    folder.isFolder && folder.linkFolderHere && !folder.isLiveFolder && folder.nameSource != .user
      && folder.section == .today && store.contains(folder) && !store.isIncognito(folder) && isEnabled
  }

  /// The highest threshold `folder` has reached; 0 below the first.
  static func threshold(_ folder: SidebarNode) -> Int {
    thresholds.last { folder.children.count >= $0 } ?? 0
  }

  /// The folder `node` is in (`node` itself if it's one), through splits.
  static func folder(of node: SidebarNode) -> SidebarNode? {
    var current: SidebarNode? = node
    while let c = current {
      if c.isFolder { return c }
      if c.isRoot { return nil }
      current = c.parent
    }
    return nil
  }

  func changed(_ change: SpaceStoreChange) {
    for id in change.deletedNodes {
      askedAt[id] = nil
      waiting.removeValue(forKey: id)?.cancel()
    }
    var folders: [String: SidebarNode] = [:]
    for id in change.putNodes.union(change.todayChurn) {
      if let node = store.node(id: id), let folder = Self.folder(of: node) { folders[folder.id] = folder }
    }
    for folder in folders.values { folderChanged(folder) }
  }

  /// A change in `folder`: (re)starts its wait if it reached a threshold it
  /// wasn't named at, or is waiting.
  func folderChanged(_ folder: SidebarNode) {
    let id = folder.id
    guard isEligible(folder) else {
      waiting.removeValue(forKey: id)?.cancel()
      return
    }
    guard Self.threshold(folder) > askedAt[id, default: 0] || waiting[id] != nil else { return }
    // 5 s after its last change.
    waiting[id]?.cancel()
    let debounce = debounce
    waiting[id] = Task { [weak self] in
      try? await Task.sleep(for: debounce)
      guard !Task.isCancelled else { return }
      await self?.waited(id)
    }
  }

  func waited(_ id: String) async {
    waiting[id] = nil
    guard let folder = store.node(id: id), isEligible(folder) else { return }
    let threshold = Self.threshold(folder)
    guard threshold > askedAt[id, default: 0] else { return }
    askedAt[id] = threshold
    await ask(id, background: true)
  }

  func ask(_ id: String, background: Bool) async {
    guard let folder = store.node(id: id), folder.isFolder, !store.isIncognito(folder), !asking.contains(id)
    else { return }
    asking.insert(id)
    let request = ClaudeQuick.Request(
      feature: ClaudeQuick.nameLinkFoldersKey, model: model(),
      system: SidebarPrompts.section(Self.promptID, in: prompts, locale: locale()), user: Self.describe(folder),
      maxTokens: 60, background: background
    ).withOnDevicePrompt(Self.promptID, prompts: prompts, locale: locale())
    let result = await quick.ask(request)
    asking.remove(id)
    guard case .success(let reply) = result, let text = reply["name"]?.string,
      let named = store.node(id: id), named.isFolder, store.contains(named), !store.isIncognito(named)
    else { return }
    // A name the user gave while Claude thought wins, and so does turning
    // the switch off; Suggest a Name asks for one whatever the name is.
    if background && !isEligible(named) { return }
    guard let name = sanitizeAiName(text), name != named.title else { return }
    store.setFolderName(named, name, source: .claude)
    sparkle(id)
  }

  private func sparkle(_ id: String) {
    sparkling.insert(id)
    Task { [weak self] in
      try? await Task.sleep(for: Self.sparkleDuration)
      self?.sparkling.remove(id)
    }
  }
}
