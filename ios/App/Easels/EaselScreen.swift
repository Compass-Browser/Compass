// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassEasels
import CompassModel
import SwiftUI

/// What a compass://easel/ tab shows in place of a web view: the board's
/// editor, or for compass://easel/ the list of boards.
struct EaselScreen: View {
  @Environment(AppModel.self) private var app
  let tab: SidebarNode
  var topInset: CGFloat = 0
  var bottomInset: CGFloat = 0

  var body: some View {
    Group {
      if let uuid = EaselURL.uuid(from: tab.url) {
        if EaselStore.shared.easel(uuid) != nil {
          EaselEditor(board: uuid, tab: tab, topInset: topInset, bottomInset: bottomInset)
            .id(uuid)
        } else {
          missing
        }
      } else {
        EaselList(tab: tab, topInset: topInset, bottomInset: bottomInset)
      }
    }
  }

  /// A board another device made that hasn't synced yet, or one deleted.
  private var missing: some View {
    ContentUnavailableView {
      Label("Easel Not Here", systemImage: "rectangle.on.rectangle.angled")
    } description: {
      Text("This Easel isn't on this iPhone. It shows once it syncs, unless it was deleted.")
    } actions: {
      Button("Show All Easels") { app.store.update(tab, url: EaselURL.list, title: "Easels") }
    }
    .padding(.top, topInset)
    .padding(.bottom, bottomInset)
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(Color(.systemGroupedBackground))
  }
}

/// compass://easel/: every board, newest first. A tap opens one in this
/// tab; boards are deleted here (closing a board's tab doesn't delete it).
struct EaselList: View {
  @Environment(AppModel.self) private var app
  let tab: SidebarNode
  var topInset: CGFloat = 0
  var bottomInset: CGFloat = 0
  @State private var deleting: EaselBoard?

  var body: some View {
    let store = EaselStore.shared
    let boards = store.easels
    List {
      Section {
        Button {
          newEasel()
        } label: {
          Label("New Easel", systemImage: "plus")
        }
      }
      Section {
        ForEach(boards, id: \.uuid) { board in
          Button { app.openEasel(board, in: tab) } label: {
            HStack(spacing: 12) {
              Image(systemName: "rectangle.on.rectangle.angled")
                .font(.system(size: 18))
                .foregroundStyle(.tint)
                .frame(width: 28)
              VStack(alignment: .leading, spacing: 2) {
                Text(board.title.isEmpty ? "Untitled" : board.title).foregroundStyle(.primary).lineLimit(1)
                let count = store.objectCount(of: board.uuid)
                Text("\(count) item\(count == 1 ? "" : "s") · \(Easels.date(syncTime: board.updateTime).formatted(.relative(presentation: .named)))")
                  .font(.caption).foregroundStyle(.secondary)
              }
            }
          }
          .swipeActions {
            Button("Delete", role: .destructive) { deleting = board }
          }
          .contextMenu {
            Button("Delete", systemImage: "trash", role: .destructive) { deleting = board }
          }
        }
      } header: {
        Text("Easels")
      } footer: {
        if boards.isEmpty { Text("Boards of notes, drawings, images, links and captures of pages, on every device.") }
      }
    }
    .contentMargins(.top, topInset, for: .scrollContent)
    .contentMargins(.bottom, bottomInset, for: .scrollContent)
    .alert(
      "Delete “\(deleting?.title ?? "")”?",
      isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })
    ) {
      Button("Delete", role: .destructive) {
        if let board = deleting { EaselStore.shared.deleteEasel(board.uuid) }
        deleting = nil
      }
      Button("Cancel", role: .cancel) { deleting = nil }
    } message: {
      Text("It's deleted on every device, with everything on it.")
    }
  }

  private func newEasel() {
    switch EaselStore.shared.createEasel(title: "Untitled Easel") {
    case .failure(let error): app.showMessage(error.message)
    case .success(let board):
      // Pinned in the space, as New Easel does elsewhere; this tab stays
      // the list.
      if let pinned = app.pinEasel(board) { app.show(pinned) }
    }
  }
}
