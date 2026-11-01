// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import CompassSync
import SwiftUI

/// The sidebar, a bottom sheet over the browser: the spaces side by side
/// (swipe between them), each with the favorites, its pinned tree and Today,
/// tinted with the space's color.
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
    .background(
      LinearGradient(
        colors: [(app.selectedSpace?.color.color ?? .blue).opacity(0.28), Color(.systemBackground)],
        startPoint: .top, endPoint: .bottom
      )
      .ignoresSafeArea()
    )
    .animation(.easeInOut(duration: 0.25), value: app.selectedSpaceID)
    .sheet(isPresented: app.presented(\.showingCommandBar, fromSidebar: true)) {
      CommandBar(replacing: nil)
    }
    .sheet(isPresented: app.presented(\.showingSettings, fromSidebar: true)) { SettingsView() }
  }

  private var bottomBar: some View {
    HStack(spacing: 14) {
      Button { app.showingSettings = true } label: {
        Image(systemName: syncIcon).font(.title3)
      }
      .accessibilityLabel("Settings")
      Spacer()
      HStack(spacing: 8) {
        let spaces = app.store.spaces
        ForEach(Array(spaces.enumerated()), id: \.element.id) { i, space in
          Circle()
            .fill(space.id == app.selectedSpace?.id ? space.color.color : Color.secondary.opacity(0.35))
            .frame(width: 8, height: 8)
            // A wider gap where the profile changes.
            .padding(
              .leading,
              i > 0 && !app.store.sameProfile(spaces[i - 1].profileUUID, space.profileUUID) ? 6 : 0)
            .onTapGesture { app.selectedSpaceID = space.id }
        }
        Button {
          // In the profile of the space it was made from.
          let space = app.store.addSpace(
            name: "Space \(app.store.spaces.count + 1)", color: .allCases.randomElement()!,
            profile: app.selectedSpace?.profileUUID ?? "")
          app.selectedSpaceID = space.id
        } label: {
          Image(systemName: "plus").font(.caption.weight(.bold))
        }
        .accessibilityLabel("New space")
      }
      Spacer()
      Button { app.showingCommandBar = true } label: {
        Image(systemName: "plus.square.fill").font(.title2)
      }
      .accessibilityLabel("New tab")
    }
    .padding(.horizontal, 20)
    .padding(.vertical, 10)
    .background(.ultraThinMaterial)
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

struct SpacePage: View {
  @Environment(AppModel.self) private var app
  let space: Space
  @State private var renaming = false
  @State private var newName = ""
  @State private var showingArchive = false
  @State private var moving: ChainProfile?

  var body: some View {
    List {
      Section {
        header
          .listRowBackground(Color.clear)
          .listRowSeparator(.hidden)
        if !app.store.favorites(for: space).children.isEmpty {
          FavoritesGrid(root: app.store.favorites(for: space))
            .listRowBackground(Color.clear)
            .listRowSeparator(.hidden)
        }
      }
      Section {
        ForEach(space.pinned.children) { node in
          NodeTree(node: node)
        }
        .onMove { from, to in move(in: space.pinned, from: from, to: to) }
      } header: {
        Text("Pinned")
      }
      Section {
        Button {
          app.showingCommandBar = true
        } label: {
          Label("New Tab", systemImage: "plus")
        }
        ForEach(space.today.children.reversed()) { node in
          NodeTree(node: node)
        }
      } header: {
        HStack {
          Text("Today")
          Spacer()
          if !space.today.children.isEmpty {
            Button("Clear") {
              for entry in space.today.children { app.store.archive(entry) }
            }
            .font(.caption)
          }
        }
      }
    }
    .scrollContentBackground(.hidden)
    .listStyle(.insetGrouped)
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

  private var header: some View {
    HStack {
      Circle().fill(space.color.color).frame(width: 12, height: 12)
      Text(space.name).font(.title2.weight(.semibold))
      if let profile = app.sync.profileName(of: space) {
        Text(profile)
          .font(.caption.weight(.medium))
          .foregroundStyle(.secondary)
          .padding(.horizontal, 7)
          .padding(.vertical, 2)
          .background(.regularMaterial, in: Capsule())
      }
      Spacer()
      Menu {
        spaceMenu
      } label: {
        Image(systemName: "ellipsis.circle").font(.title3)
      }
    }
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

struct FavoritesGrid: View {
  @Environment(AppModel.self) private var app
  /// The favorites of the space's profile.
  let root: SidebarNode

  var body: some View {
    LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 10), count: 4), spacing: 10) {
      ForEach(root.children) { fav in
        Button { app.show(fav) } label: {
          SiteIcon(url: fav.url, size: 28)
            .frame(maxWidth: .infinity, minHeight: 54)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
        }
        .buttonStyle(.plain)
        .contextMenu {
          Button("Remove from Favorites", systemImage: "star.slash", role: .destructive) { app.close(fav) }
        }
      }
    }
  }
}

/// A row and, for open folders and splits, its children.
struct NodeTree: View {
  @Environment(AppModel.self) private var app
  let node: SidebarNode
  var depth = 0

  var body: some View {
    NodeRow(node: node, depth: depth)
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
    Button { app.show(node) } label: {
      HStack(spacing: 10) {
        icon
        Text(title)
          .lineLimit(1)
          .foregroundStyle(.primary)
        Spacer()
        if node.tabs.contains(where: app.tabs.isLive) {
          Circle().fill(app.store.space(of: node)?.color.color ?? .accentColor).frame(width: 6, height: 6)
        }
      }
      .padding(.leading, CGFloat(depth) * 18)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
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

  private var title: String {
    if node.isSplit { return node.children.map(\.displayTitle).joined(separator: "  |  ") }
    return node.displayTitle.isEmpty ? "Untitled" : node.displayTitle
  }

  @ViewBuilder private var icon: some View {
    switch node.kind {
    case .folder:
      Image(systemName: node.collapsed ? "folder" : "folder.fill").foregroundStyle(.secondary).frame(width: 18)
    case .split:
      Image(systemName: "rectangle.split.2x1").foregroundStyle(.secondary).frame(width: 18)
    default:
      SiteIcon(url: node.url)
    }
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
