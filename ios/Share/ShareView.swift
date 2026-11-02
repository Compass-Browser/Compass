// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import SwiftUI

struct ShareView: View {
  @Bindable var model: ShareModel

  var body: some View {
    NavigationStack {
      Group {
        if let done = model.done {
          VStack(spacing: 12) {
            Image(systemName: "checkmark.circle.fill").font(.system(size: 44)).foregroundStyle(.green)
            Text(done).font(.headline)
          }
          .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.loaded && model.url == nil {
          ContentUnavailableView("No Link", systemImage: "link", description: Text("There's no web link to share."))
        } else {
          form
        }
      }
      .navigationTitle("Compass")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .cancellationAction) {
          Button("Cancel") { model.finish?(false) }
        }
        ToolbarItem(placement: .confirmationAction) {
          Button(model.action == .open ? "Open" : "Add") { model.confirm() }
            .bold()
            .disabled(model.url == nil || model.done != nil || (model.action == .pin && model.space == nil))
        }
      }
    }
    .tint(model.space.map { Color(hex: $0.style.accent) } ?? .accentColor)
  }

  private var form: some View {
    Form {
      Section {
        HStack(spacing: 12) {
          Image(systemName: "globe").font(.system(size: 22)).foregroundStyle(.secondary).frame(width: 32)
          VStack(alignment: .leading, spacing: 2) {
            Text(model.title.isEmpty ? (model.url?.host() ?? "…") : model.title).font(.headline).lineLimit(2)
            Text(model.url?.absoluteString ?? "").font(.caption).foregroundStyle(.secondary).lineLimit(1)
          }
        }
      }
      if model.snapshot != nil {
        Section {
          Picker("Action", selection: $model.action) {
            ForEach(ShareModel.Action.allCases) { Text($0.rawValue).tag($0) }
          }
          .pickerStyle(.segmented)
          .listRowBackground(Color.clear)
          .listRowInsets(EdgeInsets())
        }
      }
      if let snapshot = model.snapshot, !snapshot.spaces.isEmpty {
        Section("Space") {
          ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
              ForEach(snapshot.spaces) { space in
                SpaceChip(space: space, selected: space.id == model.spaceID) { model.select(space) }
              }
            }
            .padding(.vertical, 4)
          }
        }
        if model.action == .pin, let space = model.space, !space.folders.isEmpty {
          Section("Folder") {
            FolderRow(name: "Pinned", depth: 0, symbol: "pin", selected: model.folderID == nil) { model.folderID = nil }
            ForEach(space.allFolders, id: \.folder.id) { item in
              FolderRow(name: item.folder.name, depth: item.depth + 1, symbol: "folder", selected: model.folderID == item.folder.id) {
                model.folderID = item.folder.id
              }
            }
          }
        }
      } else {
        Section {
          Text("Opens in a new tab. Open Compass once to choose spaces and pin from here.")
            .font(.footnote).foregroundStyle(.secondary)
        }
      }
    }
  }
}

struct SpaceChip: View {
  let space: SpaceInfo
  let selected: Bool
  let action: () -> Void

  var body: some View {
    let accent = Color(hex: space.style.accent)
    Button(action: action) {
      HStack(spacing: 6) {
        switch space.icon {
        case .symbol(let name): Image(systemName: name).font(.system(size: 13, weight: .semibold))
        case .emoji(let emoji): Text(emoji).font(.system(size: 13))
        case .initial(let letter): Text(letter).font(.system(size: 13, weight: .bold, design: .rounded))
        }
        Text(space.name).font(.system(size: 14, weight: .medium)).lineLimit(1)
      }
      .padding(.horizontal, 12)
      .frame(height: 34)
      .foregroundStyle(selected ? .white : .primary)
      .background(selected ? accent : accent.opacity(0.15), in: Capsule())
    }
    .buttonStyle(.plain)
    .accessibilityAddTraits(selected ? .isSelected : [])
  }
}

struct FolderRow: View {
  let name: String
  let depth: Int
  let symbol: String
  let selected: Bool
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      HStack(spacing: 8) {
        Image(systemName: symbol).foregroundStyle(.secondary).frame(width: 20)
        Text(name).foregroundStyle(.primary)
        Spacer()
        if selected { Image(systemName: "checkmark").fontWeight(.semibold).foregroundStyle(.tint) }
      }
      .padding(.leading, CGFloat(depth) * 16)
    }
  }
}

extension Color {
  init(hex: UInt32) {
    self.init(
      red: Double((hex >> 16) & 0xFF) / 255, green: Double((hex >> 8) & 0xFF) / 255, blue: Double(hex & 0xFF) / 255)
  }
}
