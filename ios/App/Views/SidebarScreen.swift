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
    let spaces = app.store.spaces
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
    .presentationBackground { SpaceTint(color: app.selectedSpace?.color.color ?? .blue) }
    .animation(.easeInOut(duration: 0.25), value: app.selectedSpaceID)
    .sheet(isPresented: app.presented(\.showingCommandBar, fromSidebar: true)) {
      CommandBar(replacing: nil)
    }
    .sheet(isPresented: app.presented(\.showingSettings, fromSidebar: true)) { SettingsView() }
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
          let spaces = app.store.spaces
          ForEach(Array(spaces.enumerated()), id: \.element.id) { i, space in
            let shown = space.id == app.selectedSpace?.id
            Button { app.selectedSpaceID = space.id } label: {
              SpaceIcon(space: space)
                .foregroundStyle(shown ? space.color.color : .primary)
                .opacity(shown ? 1 : 0.45)
                .frame(width: 34, height: 34)
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
              name: "Space \(app.store.spaces.count + 1)", color: .allCases.randomElement()!,
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
      Button { app.showingCommandBar = true } label: {
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

/// A plain row of the sidebar's List: no separator, no system background
/// (each row draws its own pill), the desktop's 2 px between rows.
private struct SidebarRowStyle: ViewModifier {
  func body(content: Content) -> some View {
    content
      .listRowBackground(Color.clear)
      .listRowSeparator(.hidden)
      .listRowInsets(EdgeInsets(top: 1, leading: 12, bottom: 1, trailing: 12))
  }
}

extension View {
  fileprivate func sidebarRow() -> some View { modifier(SidebarRowStyle()) }
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
  @State private var moving: ChainProfile?

  var body: some View {
    List {
      let favorites = app.store.favorites(for: space)
      if !favorites.children.isEmpty {
        FavoritesGrid(root: favorites)
          .sidebarRow()
      }
      header.sidebarRow()

      SectionHeader(title: "Pinned") { EmptyView() }.sidebarRow()
      if space.pinned.children.isEmpty {
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
          .sidebarRow()
      }
      ForEach(space.pinned.children) { node in
        NodeTree(node: node)
      }
      .onMove { from, to in move(in: space.pinned, from: from, to: to) }

      SectionHeader(title: "Today") {
        if !space.today.children.isEmpty {
          Button("Clear") {
            for entry in space.today.children { app.store.archive(entry) }
          }
          .font(.system(size: 13, weight: .medium))
          .buttonStyle(.plain)
          .opacity(0.6)
        }
      }
      .sidebarRow()
      Button {
        app.showingCommandBar = true
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
      .sidebarRow()
      ForEach(space.today.children.reversed()) { node in
        NodeTree(node: node)
      }
    }
    .listStyle(.plain)
    .scrollContentBackground(.hidden)
    // Clear of the sheet's grab handle.
    .contentMargins(.top, 22, for: .scrollContent)
    .environment(\.defaultMinListRowHeight, 0)
    .foregroundStyle(.primary)
    .alert("Rename Space", isPresented: $renaming) {
      TextField("Name", text: $newName)
      Button("Rename") { app.store.renameSpace(space, to: newName) }
      Button("Cancel", role: .cancel) {}
    }
    .sheet(isPresented: $showingArchive) { ArchiveView(space: space) }
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
      if let profile = app.sync.profileName(of: space) {
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
    Button("Rename", systemImage: "pencil") {
      newName = space.name
      renaming = true
    }
    Menu("Color", systemImage: "paintpalette") {
      ForEach(SpaceColor.allCases, id: \.self) { color in
        Button(color.name) { app.store.setColor(space, color) }
      }
    }
    Button("Archive", systemImage: "archivebox") { showingArchive = true }
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

  private func move(in root: SidebarNode, from: IndexSet, to: Int) {
    guard let index = from.first else { return }
    let node = root.children[index]
    app.store.move(node, to: root, at: to > index ? to - 1 : to)
  }
}

/// A light press highlight, the desktop's hover fill.
struct SidebarPressStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .background(
        SidebarStyle.fill.opacity(configuration.isPressed ? 1 : 0),
        in: RoundedRectangle(cornerRadius: SidebarStyle.rowRadius)
      )
      .scaleEffect(configuration.isPressed ? 0.98 : 1)
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
    let favorites = root.children
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
    .buttonStyle(.plain)
    .accessibilityLabel(fav.displayTitle)
    .contextMenu {
      Button("Remove from Favorites", systemImage: "star.slash", role: .destructive) { app.close(fav) }
    }
  }
}

/// A row and, for open folders, its children.
struct NodeTree: View {
  @Environment(AppModel.self) private var app
  let node: SidebarNode
  var depth = 0

  var body: some View {
    NodeRow(node: node, depth: depth)
      .sidebarRow()
    if node.isFolder && !node.collapsed {
      ForEach(node.children) { child in
        NodeTree(node: child, depth: depth + 1)
      }
    }
  }
}

struct NodeRow: View {
  @Environment(AppModel.self) private var app
  let node: SidebarNode
  let depth: Int
  @State private var renaming = false
  @State private var newTitle = ""

  var body: some View {
    Group {
      if node.isSplit {
        // Panes of a split share one row (`.split`), each its own entry.
        HStack(spacing: 4) {
          ForEach(node.children) { pane in
            entry(pane, shown: app.selectedNodeID == node.id && app.shownTab === pane)
          }
        }
      } else {
        entry(node, shown: app.selectedNodeID == node.id)
      }
    }
    .padding(.leading, CGFloat(depth) * SidebarStyle.indent)
    .swipeActions(edge: .trailing) {
      if node.section == .today {
        Button("Close", systemImage: "xmark") { app.close(node) }.tint(.red)
      } else {
        Button("Delete", systemImage: "trash", role: .destructive) { app.close(node) }
      }
    }
    .contextMenu { menu }
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
    return Button { app.show(item) } label: {
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
        } else {
          SiteIcon(url: item.url, size: SidebarStyle.favicon).opacity(dimmed ? 0.55 : 1)
        }
        Text(title(item))
          .font(.system(size: 16, weight: item.isFolder ? .medium : .regular))
          .lineLimit(1)
          .opacity(dimmed ? 0.55 : 1)
        Spacer(minLength: 0)
      }
      .padding(.horizontal, 12)
      .frame(height: SidebarStyle.rowHeight)
      .background(
        shown ? SidebarStyle.active : .clear,
        in: RoundedRectangle(cornerRadius: SidebarStyle.rowRadius)
      )
      .shadow(color: .black.opacity(shown ? 0.12 : 0), radius: 1.5, y: 1)
      .contentShape(Rectangle())
      .contentShape(.contextMenuPreview, RoundedRectangle(cornerRadius: SidebarStyle.rowRadius))
    }
    .buttonStyle(SidebarPressStyle())
    .animation(.easeOut(duration: 0.12), value: item.collapsed)
  }

  private func title(_ item: SidebarNode) -> String {
    item.displayTitle.isEmpty ? "Untitled" : item.displayTitle
  }

  @ViewBuilder private var menu: some View {
    let space = app.store.space(of: node)
    Button("Rename", systemImage: "pencil") {
      newTitle = node.isFolder ? node.title : node.displayTitle
      renaming = true
    }
    if let space {
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

struct ArchiveView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  let space: Space

  var body: some View {
    NavigationStack {
      List {
        if space.archive.children.isEmpty {
          Text("Nothing archived").foregroundStyle(.secondary)
        }
        ForEach(space.archive.children.sorted { ($0.archivedAt ?? .distantPast) > ($1.archivedAt ?? .distantPast) }) { entry in
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
      .navigationTitle("Archive")
      .toolbar { Button("Done") { dismiss() } }
    }
  }
}
