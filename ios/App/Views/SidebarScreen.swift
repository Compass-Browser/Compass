// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import CompassSync
import SwiftUI

/// The sidebar, a bottom sheet over the browser, styled as the desktop's
/// (resources/sidebar/app.css): the space's tint behind everything, the
/// spaces side by side (swipe between them) each with its favorites, name,
/// Pinned and Today, and the space switcher at the bottom.
struct SidebarScreen: View {
  @Environment(AppModel.self) private var app

  var body: some View {
    @Bindable var app = app
    let spaces = app.visibleSpaces
    VStack(spacing: 0) {
      TabView(selection: $app.selectedSpaceID) {
        ForEach(spaces) { space in
          SpacePage(space: space)
            .tag(Optional(space.id))
        }
      }
      .tabViewStyle(.page(indexDisplayMode: .never))
      bottomBar
    }
    .animation(.easeInOut(duration: 0.25), value: app.selectedSpaceID)
    // Incognito: dark, whatever the panel's tint underneath.
    .modifier(IncognitoLook(on: app.selectedSpace?.isIncognito == true))
  }

  /// Settings, the space switcher (`#spaces`: each space's icon, the shown
  /// one at full opacity, the others dimmed, then "+"), and a new tab.
  private var bottomBar: some View {
    HStack(spacing: 0) {
      Button { app.showingSettings = true } label: {
        Image(systemName: syncIcon).font(.system(size: 18, weight: .medium)).frame(width: 44, height: 44)
      }
      .accessibilityLabel("Settings")
      Spacer(minLength: 4)
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 2) {
          let spaces = app.visibleSpaces
          ForEach(Array(spaces.enumerated()), id: \.element.id) { i, space in
            let shown = space.id == app.selectedSpace?.id
            Button { app.selectedSpaceID = space.id } label: {
              SpaceIcon(space: space)
                .foregroundStyle(shown ? space.theme.accent : .primary)
                .opacity(shown ? 1 : 0.45)
                .scaleEffect(shown ? 1.15 : 1)
                .frame(width: 34, height: 34)
                .animation(.spring(response: 0.3, dampingFraction: 0.6), value: shown)
            }
            .accessibilityLabel(space.name)
            .accessibilityAddTraits(shown ? .isSelected : [])
            // A wider gap where the profile changes.
            .padding(
              .leading,
              i > 0 && !app.store.sameProfile(spaces[i - 1].profileUUID, space.profileUUID) ? 10 : 0)
          }
          Button {
            // In the profile of the space it was made from.
            let space = app.store.addSpace(
              name: "Space \(app.store.spaces.count + 1)", theme: SpaceTheme(SpaceColor.allCases.filter { $0 != .grey }.randomElement()!),
              profile: app.selectedSpace?.profileUUID ?? "")
            app.selectedSpaceID = space.id
          } label: {
            Image(systemName: "plus").font(.system(size: 15, weight: .semibold))
              .opacity(0.45)
              .frame(width: 34, height: 34)
          }
          .accessibilityLabel("New space")
        }
        .padding(.horizontal, 4)
      }
      .scrollBounceBehavior(.basedOnSize)
      .fixedSize(horizontal: app.store.spaces.count < 7, vertical: false)
      Spacer(minLength: 4)
      Button { app.openCommandBar(newTab: true) } label: {
        Image(systemName: "plus").font(.system(size: 18, weight: .semibold)).frame(width: 44, height: 44)
      }
      .accessibilityLabel("New tab")
    }
    .buttonStyle(.plain)
    .foregroundStyle(.primary)
    .padding(.horizontal, 10)
    .padding(.bottom, 2)
  }

  private var syncIcon: String {
    switch app.sync.state {
    case .off: "gearshape"
    case .syncing: "arrow.triangle.2.circlepath"
    case .on: "checkmark.icloud"
    case .failed: "exclamationmark.icloud"
    }
  }
}

/// A section's title: small, uppercase, dimmed (`.section-header`).
private struct SectionHeader<Trailing: View>: View {
  let title: String
  @ViewBuilder var trailing: Trailing

  var body: some View {
    HStack {
      Text(title.uppercased())
        .font(.system(size: 12, weight: .semibold))
        .tracking(0.4)
        .opacity(0.55)
      Spacer()
      trailing
    }
    .padding(.leading, 12)
    .padding(.trailing, 6)
    .padding(.top, 14)
    .padding(.bottom, 2)
  }
}

struct SpacePage: View {
  @Environment(AppModel.self) private var app
  let space: Space
  @State private var renaming = false
  @State private var newName = ""
  @State private var showingArchive = false
  @State private var editingTheme = false
  @State private var moving: ChainProfile?

  var body: some View {
    // A plain stack, not a List: List rows only fade and jump in height,
    // while here a folder's children slide down and everything below moves
    // with them in one animation.
    ScrollView {
      LazyVStack(spacing: 2) {
        let favorites = app.store.favorites(for: space)
        if !space.isIncognito, !favorites.children.phoneVisible.isEmpty {
          FavoritesGrid(root: favorites)
        }
        header
        if space.isIncognito && space.today.children.isEmpty {
          Text("Incognito tabs aren't saved or synced, leave no history, and keep their cookies only until you close them or quit Compass. Sites can still see your visits.")
            .font(.footnote)
            .opacity(0.6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(12)
        }

        if !space.isIncognito {
        SectionHeader(title: "Pinned") { EmptyView() }
          .dropDestination(for: String.self) { ids, _ in drop(ids, into: space.pinned) }
        if space.pinned.children.phoneVisible.isEmpty {
          Text("Pin tabs to keep them in this space.")
            .font(.footnote)
            .opacity(0.55)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(12)
            .overlay(
              RoundedRectangle(cornerRadius: SidebarStyle.rowRadius)
                .strokeBorder(style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
                .opacity(0.3)
            )
            .dropDestination(for: String.self) { ids, _ in drop(ids, into: space.pinned) }
        }
        ForEach(space.pinned.children.phoneVisible) { node in
          NodeTree(node: node, depth: 0)
        }
        }

        SectionHeader(title: "Today") {
          if !space.today.children.phoneVisible.isEmpty {
            Button("Clear") {
              withAnimation(SidebarStyle.motion) {
                // What the phone shows; the desktop's own pages stay.
                for entry in space.today.children.phoneVisible { app.store.archive(entry) }
              }
            }
            .font(.system(size: 13, weight: .medium))
            .buttonStyle(.plain)
            .opacity(0.6)
          }
        }
        Button {
          app.openCommandBar(newTab: true)
        } label: {
          HStack(spacing: 10) {
            Image(systemName: "plus")
              .font(.system(size: 15, weight: .semibold))
              .frame(width: SidebarStyle.favicon)
            Text("New Tab")
            Spacer()
          }
          .padding(.horizontal, 12)
          .frame(height: SidebarStyle.rowHeight)
          .contentShape(Rectangle())
          .opacity(0.7)
        }
        .buttonStyle(SidebarPressStyle())
        // Dropped here: Today's newest entry.
        .dropDestination(for: String.self) { ids, _ in drop(ids, into: space.today) }
        ForEach(space.today.children.phoneVisible.reversed()) { node in
          NodeTree(node: node, depth: 0)
        }
      }
      // Pulled down past its top, the panel moves instead (SidebarPanel):
      // the list stays put in it.
      .offset(y: -app.sidebarPull)
      .padding(.horizontal, 12)
      .padding(.bottom, 12)
      // Entries appearing, closing and moving animate, also when the change
      // comes from another device. Folders have their own (NodeTree).
      .animation(SidebarStyle.motion, value: layout)
    }
    // Clear of the panel's grab handle.
    .contentMargins(.top, 22, for: .scrollContent)
    .sidebarPullToClose(app)
    // At half height a drag moves the panel instead (SidebarPanel).
    .scrollDisabled(app.sidebarHalf)
    .scrollIndicators(.hidden)
    .foregroundStyle(.primary)
    // Incognito, locked or covered.
    .overlay { if space.isIncognito && app.incognitoHidden { IncognitoLockView() } }
    .alert("Rename Space", isPresented: $renaming) {
      TextField("Name", text: $newName)
      Button("Rename") { app.store.renameSpace(space, to: newName) }
      Button("Cancel", role: .cancel) {}
    }
    .sheet(isPresented: $showingArchive) { ArchiveView(space: space) }
    .sheet(isPresented: $editingTheme) { ThemeEditor(space: space) }
    .alert(
      "Move “\(space.name)” to \(moving?.name ?? "")?",
      isPresented: Binding(get: { moving != nil }, set: { if !$0 { moving = nil } })
    ) {
      Button("Move") {
        if let moving {
          // Live tabs reload in the new profile's website data store.
          for tab in space.allNodes where tab.isTab { app.tabs.unload(tab.id) }
          if let shown = app.selectedNode, app.store.space(of: shown) === space { app.selectedNodeID = nil }
          app.store.setProfile(space, to: app.sync.spaceProfile(for: moving))
        }
        moving = nil
      }
      Button("Cancel", role: .cancel) { moving = nil }
    } message: {
      Text("Its tabs will be signed out of sites, because each profile keeps its own cookies.")
    }
  }

  /// The space's name (`#spaceName`), its profile and its menu.
  private var header: some View {
    HStack(spacing: 8) {
      Text(space.name)
        .font(.system(size: 17, weight: .semibold))
        .lineLimit(1)
      if !space.isIncognito, let profile = app.sync.profileName(of: space) {
        Text(profile)
          .font(.system(size: 11, weight: .medium))
          .opacity(0.6)
          .padding(.horizontal, 7)
          .padding(.vertical, 2)
          .background(SidebarStyle.fill, in: Capsule())
      }
      Spacer()
      Menu {
        spaceMenu
      } label: {
        Image(systemName: "ellipsis")
          .font(.system(size: 15, weight: .semibold))
          .frame(width: 32, height: 32)
          .background(SidebarStyle.fill, in: Circle())
      }
      .accessibilityLabel("Space menu")
    }
    .padding(.leading, 12)
    .padding(.top, 10)
    .contentShape(Rectangle())
    .contextMenu { spaceMenu }  // a long press on the space's header
  }

  @ViewBuilder private var spaceMenu: some View {
    if space.isIncognito {
      Button("Close All Incognito Tabs", systemImage: "xmark.circle", role: .destructive) {
        withAnimation(SidebarStyle.motion) { app.services.closeAllIncognito() }
      }
    } else {
      normalSpaceMenu
    }
  }

  @ViewBuilder private var normalSpaceMenu: some View {
    Button("Rename", systemImage: "pencil") {
      newName = space.name
      renaming = true
    }
    Button("Edit Theme", systemImage: "paintpalette") { editingTheme = true }
    Button("Archive", systemImage: "archivebox") { showingArchive = true }
    Picker(selection: Binding(
      get: { ArchiveAfter(space.archiveAfter) },
      set: { app.store.setArchiveAfter(space, $0.interval) })
    ) {
      ForEach(ArchiveAfter.allCases) { Text($0.name).tag($0) }
    } label: {
      Label("Archive Tabs", systemImage: "clock.arrow.circlepath")
    }
    .pickerStyle(.menu)
    // Asks first: the space's tabs lose their cookies (another profile's
    // website data store).
    let others = app.sync.profiles.filter { $0.uuid != app.sync.profile(of: space)?.uuid }
    if app.sync.profiles.count > 1 && !others.isEmpty {
      Menu("Move to Profile", systemImage: "person.crop.circle") {
        ForEach(others) { profile in
          Button(profile.name) { moving = profile }
        }
      }
    }
    if app.store.spaces.count > 1 {
      Button("Delete Space", systemImage: "trash", role: .destructive) {
        app.store.deleteSpace(space)
        app.selectedSpaceID = app.store.spaces.first?.id
      }
    }
  }

  /// What the List shows, for its animation: entries, the shown entry and
  /// the favorites. Not open folders: they have their own (SidebarStyle.folder).
  private var layout: [String] {
    let entries = (space.pinned.children + space.today.children).flatMap(\.subtree)
    return entries.map(\.id)
      + app.store.favorites(for: space).children.map(\.id)
      + [app.selectedNodeID ?? ""]
  }

  /// Dropped on a section's header: the end of Pinned, Today's top.
  private func drop(_ ids: [String], into root: SidebarNode) -> Bool {
    guard let id = ids.first, let node = app.store.node(id: id), node.parent !== root || root.section == .today
    else { return false }
    withAnimation(SidebarStyle.motion) {
      _ = app.store.move(node, to: root, at: root.children.filter { $0 !== node }.count)
    }
    return true
  }
}

/// A light press highlight, the desktop's hover fill.
struct SidebarPressStyle: ButtonStyle {
  var scale: CGFloat = 0.98

  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .background(
        SidebarStyle.fill.opacity(configuration.isPressed ? 1 : 0),
        in: RoundedRectangle(cornerRadius: SidebarStyle.rowRadius)
      )
      .scaleEffect(configuration.isPressed ? scale : 1)
      .animation(.easeOut(duration: 0.1), value: configuration.isPressed)
  }
}

/// Favorites as the desktop's tiles: they share each row's width, four to a
/// row; the shown one is the light pill.
struct FavoritesGrid: View {
  @Environment(AppModel.self) private var app
  /// The favorites of the space's profile.
  let root: SidebarNode

  var body: some View {
    // Rows of four; a shorter last row's tiles grow to fill it, as on the
    // desktop (`flex: 1 1 48px`).
    let favorites = root.children.phoneVisible
    let rows = stride(from: 0, to: favorites.count, by: 4).map { Array(favorites[$0..<min($0 + 4, favorites.count)]) }
    VStack(spacing: 8) {
      ForEach(rows, id: \.first!.id) { row in
        HStack(spacing: 8) {
          ForEach(row) { fav in tile(fav) }
        }
      }
    }
  }

  private func tile(_ fav: SidebarNode) -> some View {
    let shown = app.selectedNodeID == fav.id
    return Button { app.show(fav) } label: {
      SiteIcon(url: fav.url, size: 26)
        .frame(maxWidth: .infinity, minHeight: SidebarStyle.tileHeight)
        .background(
          shown ? SidebarStyle.active : SidebarStyle.fill,
          in: RoundedRectangle(cornerRadius: SidebarStyle.tileRadius)
        )
        .shadow(color: .black.opacity(shown ? 0.12 : 0), radius: 1.5, y: 1)
        .contentShape(.contextMenuPreview, RoundedRectangle(cornerRadius: SidebarStyle.tileRadius))
    }
    .buttonStyle(SidebarPressStyle(scale: 0.95))
    .accessibilityLabel(fav.displayTitle)
    .contextMenu {
      Button("Remove from Favorites", systemImage: "star.slash", role: .destructive) { app.close(fav) }
    }
  }
}

/// An entry and, for an open folder, its children, which slide down from
/// under the folder's row as they fade in.
struct NodeTree: View {
  let node: SidebarNode
  let depth: Int

  var body: some View {
    if node.isFolder {
      VStack(spacing: 2) {
        NodeRow(node: node, depth: depth)
        // Clipped below the folder's row, so the children come out from
        // under it rather than over it.
        VStack(spacing: 0) {
          if !node.collapsed {
            VStack(spacing: 2) {
              ForEach(node.children.phoneVisible) { child in
                NodeTree(node: child, depth: depth + 1)
              }
            }
            .transition(.move(edge: .top).combined(with: .opacity))
          }
        }
        .frame(maxWidth: .infinity)
        .clipped()
      }
    } else {
      NodeRow(node: node, depth: depth)
    }
  }
}

struct NodeRow: View {
  @Environment(AppModel.self) private var app
  @Environment(\.openWindow) private var openWindow
  let node: SidebarNode
  let depth: Int
  @State private var renaming = false
  @State private var newTitle = ""

  var body: some View {
    Group {
      if node.isSplit {
        // Panes of a split share one row (`.split`), each its own entry.
        HStack(spacing: 4) {
          ForEach(node.children.phoneVisible) { pane in
            entry(pane, shown: app.selectedNodeID == node.id && app.shownTab === pane)
          }
        }
      } else {
        entry(node, shown: app.selectedNodeID == node.id)
      }
    }
    .padding(.leading, CGFloat(depth) * SidebarStyle.indent)
    .overlay(alignment: .trailing) {
      // Today's entries close from the row, as on the desktop (`.close`).
      if node.section == .today && !node.isFolder {
        Button {
          withAnimation(SidebarStyle.motion) { app.close(node) }
        } label: {
          Image(systemName: "xmark")
            .font(.system(size: 12, weight: .bold))
            .opacity(0.45)
            .frame(width: 40, height: SidebarStyle.rowHeight)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Close \(node.displayTitle)")
      }
    }
    .contextMenu { menu }
    // Long press and drag to reorder: dropped on an entry, it goes before
    // it (into it, for a folder).
    .draggable(node.id) {
      Label(node.displayTitle, systemImage: node.isFolder ? "folder" : "globe")
        .padding(8)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
    }
    .dropDestination(for: String.self) { ids, _ in drop(ids) }
    .alert(node.isFolder ? "Rename Folder" : "Rename Tab", isPresented: $renaming) {
      TextField("Name", text: $newTitle)
      Button("Rename") { app.store.rename(node, customTitle: newTitle) }
      Button("Cancel", role: .cancel) {}
    }
  }

  /// One pill: the desktop's `.tab` / `.folder-row`.
  private func entry(_ item: SidebarNode, shown: Bool) -> some View {
    // Pinned entries without a live tab are dimmed, as on the desktop.
    let dimmed = item.section == .pinned && !item.isFolder && !item.tabs.contains(where: app.tabs.isLive)
    return Button {
      if item.isFolder {
        withAnimation(SidebarStyle.folder) { app.show(item) }
      } else {
        app.show(item)
      }
    } label: {
      HStack(spacing: 10) {
        if item.isFolder {
          Image(systemName: "chevron.right")
            .font(.system(size: 11, weight: .bold))
            .rotationEffect(.degrees(item.collapsed ? 0 : 90))
            .opacity(0.6)
            .frame(width: 10)
          Image(systemName: item.collapsed ? "folder" : "folder.fill")
            .font(.system(size: 15))
            .opacity(0.7)
            .contentTransition(.symbolEffect(.replace))
        } else {
          SiteIcon(url: item.url, size: SidebarStyle.favicon).opacity(dimmed ? 0.55 : 1)
        }
        Text(title(item))
          .font(.system(size: 16, weight: item.isFolder ? .medium : .regular))
          .lineLimit(1)
          .opacity(dimmed ? 0.55 : 1)
        Spacer(minLength: 0)
      }
      .padding(.leading, 12)
      .padding(.trailing, item.section == .today && !item.isFolder ? 36 : 12)
      .frame(height: SidebarStyle.rowHeight)
      .background(
        shown ? SidebarStyle.active : .clear,
        in: RoundedRectangle(cornerRadius: SidebarStyle.rowRadius)
      )
      .shadow(color: .black.opacity(shown ? 0.12 : 0), radius: 1.5, y: 1)
      // Claude is acting on this tab (ClaudeTools).
      .overlay {
        if app.claude.actingTab == item.id {
          RoundedRectangle(cornerRadius: SidebarStyle.rowRadius)
            .strokeBorder(Color.orange.opacity(0.8), lineWidth: 1.5)
            .overlay(alignment: .trailing) {
              Image(systemName: "sparkles").font(.caption).foregroundStyle(.orange).padding(.trailing, 10)
                .symbolEffect(.pulse)
            }
            .allowsHitTesting(false)
        }
      }
      .contentShape(Rectangle())
      .contentShape(.contextMenuPreview, RoundedRectangle(cornerRadius: SidebarStyle.rowRadius))
    }
    .buttonStyle(SidebarPressStyle())
  }

  private func drop(_ ids: [String]) -> Bool {
    guard let id = ids.first, id != node.id, let moved = app.store.node(id: id),
      !isInside(node, moved)
    else { return false }
    withAnimation(SidebarStyle.motion) {
      if node.isFolder {
        _ = app.store.move(moved, to: node, at: node.children.filter { $0 !== moved }.count)
      } else if let parent = node.parent {
        let siblings = parent.children.filter { $0 !== moved }
        let index = siblings.firstIndex { $0 === node } ?? siblings.count
        // Today's top level is shown newest first: before on screen is after.
        let reversed = parent === app.store.space(of: node)?.today
        _ = app.store.move(moved, to: parent, at: reversed ? index + 1 : index)
      }
    }
    return true
  }

  /// Whether `node` is `ancestor` or inside it (no folder into itself).
  private func isInside(_ node: SidebarNode, _ ancestor: SidebarNode) -> Bool {
    var current: SidebarNode? = node
    while let c = current {
      if c === ancestor { return true }
      current = c.parent
    }
    return false
  }

  private func title(_ item: SidebarNode) -> String {
    item.displayTitle.isEmpty ? "Untitled" : item.displayTitle
  }

  @ViewBuilder private var menu: some View {
    let space = app.store.space(of: node)
    // iPad: the entry in a window of its own (its page moves there).
    if !node.isFolder, UIApplication.shared.supportsMultipleScenes {
      Button("Open in New Window", systemImage: "macwindow.badge.plus") {
        openWindow(value: WindowTarget(entry: node.id))
      }
    }
    Button("Rename", systemImage: "pencil") {
      newTitle = node.isFolder ? node.title : node.displayTitle
      renaming = true
    }
    // Incognito: nothing that would copy it into synced lists (pinned,
    // favorites, other spaces); Close removes it.
    if let space, space.isIncognito {
      if node.isTab, let parent = node.parent, !parent.isSplit {
        let candidates = parent.children.filter { $0.isTab && $0 !== node }
        if !candidates.isEmpty {
          Menu("Split With", systemImage: "rectangle.split.2x1") {
            ForEach(candidates) { other in
              Button(other.displayTitle) { app.store.split(node, with: other) }
            }
          }
        }
      }
    } else if let space {
      if node.section == .today {
        Button("Pin", systemImage: "pin") { app.store.move(node, to: space.pinned, at: space.pinned.children.count) }
        Button("Archive", systemImage: "archivebox") { app.store.archive(node) }
      } else if node.section == .pinned && !node.isFolder {
        Button("Unpin", systemImage: "pin.slash") { app.store.move(node, to: space.today, at: space.today.children.count) }
      }
      if node.isTab && node.parent?.isSplit != true {
        Button("Add to Favorites", systemImage: "star") {
          app.store.addTab(url: node.url, title: node.title, to: app.store.favorites(for: space))
        }
      }
      if node.section == .pinned {
        Button("New Folder", systemImage: "folder.badge.plus") {
          let parent = node.isFolder ? node : (node.parent ?? space.pinned)
          app.store.addFolder(name: "New Folder", to: parent)
        }
      }
      let others = app.store.spaces.filter { $0 !== space }
      if !others.isEmpty, let section = node.section, section != .archive {
        Menu("Move to Space", systemImage: "rectangle.stack") {
          ForEach(others) { target in
            Button(target.name) { _ = app.store.move(node, toSpace: target, section: section) }
          }
        }
      }
      if node.isTab, let parent = node.parent, !parent.isSplit {
        let candidates = parent.children.filter { $0.isTab && $0 !== node }
        if !candidates.isEmpty {
          Menu("Split With", systemImage: "rectangle.split.2x1") {
            ForEach(candidates) { other in
              Button(other.displayTitle) { app.store.split(node, with: other) }
            }
          }
        }
      }
    }
    Divider()
    Button(node.section == .today ? "Close" : "Delete", systemImage: "trash", role: .destructive) {
      app.close(node)
    }
  }
}

/// Archived entries, newest first: one space's (from its menu), or every
/// space's (from Settings, `space` nil).
struct ArchiveView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  let space: Space?

  var body: some View {
    NavigationStack {
      List {
        let spaces = space.map { [$0] } ?? app.store.spaces
        if spaces.allSatisfy({ $0.archive.children.isEmpty }) {
          Text("Nothing archived").foregroundStyle(.secondary)
        }
        ForEach(spaces) { space in
          if !space.archive.children.isEmpty {
            Section(self.space == nil ? space.name : "") {
              ForEach(space.archive.children.sorted { ($0.archivedAt ?? .distantPast) > ($1.archivedAt ?? .distantPast) }) { entry in
                row(entry)
              }
            }
          }
        }
      }
      .navigationTitle("Archive")
      .toolbar { Button("Done") { dismiss() } }
    }
  }

  private func row(_ entry: SidebarNode) -> some View {
    Button {
      if let restored = app.store.restore(entry) {
        app.show(restored)
        dismiss()
      }
    } label: {
      HStack {
        SiteIcon(url: entry.tabs.first?.url ?? "")
        VStack(alignment: .leading) {
          Text(entry.displayTitle).lineLimit(1).foregroundStyle(.primary)
          if let at = entry.archivedAt {
            Text(at, style: .relative).font(.caption).foregroundStyle(.secondary)
          }
        }
      }
    }
  }
}

/// Incognito's sidebar look: near black, in the dark color scheme.
struct IncognitoLook: ViewModifier {
  let on: Bool

  func body(content: Content) -> some View {
    if on {
      content
        .background(Color(white: 0.09).ignoresSafeArea())
        .environment(\.colorScheme, .dark)
    } else {
      content
    }
  }
}
