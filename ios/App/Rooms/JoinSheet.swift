// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import CompassRooms
import SwiftUI

/// The join preview (docs/design/ios.md, "Rooms"): what an invite link (or
/// an email invitation) shares, from which server, and where to put it;
/// then Join, after which it says who shared it and the role.
struct JoinSheet: View {
  let link: RoomInviteLink?
  let invitation: RoomsManager.Invitation?
  let close: () -> Void

  @State private var spaceID: String?
  @State private var working = false
  @State private var error: String?
  @State private var joined: RoomSync?

  private var model: RoomsModel { RoomsModel.shared }
  private var rooms: RoomsManager? { model.manager }

  private var kind: RoomKind { link?.kind ?? invitation?.kind ?? .folder }
  private var name: String {
    if let link, !link.name.isEmpty { return link.name }
    if let joined { return joined.name }
    return "Shared \(kindName)"
  }
  private var kindName: String {
    switch kind {
    case .space: "space"
    case .folder: "folder"
    case .easel: "easel"
    }
  }
  private var server: String {
    if let link { return URL(string: link.serverURL)?.host() ?? link.serverURL }
    return rooms?.accounts[invitation?.account ?? ""]?.serverURL.host() ?? ""
  }

  /// Spaces it can go in (not incognito), for a folder or an easel.
  private var spaces: [Space] { rooms?.store.spaces ?? [] }

  var body: some View {
    NavigationStack {
      Form {
        Section {
          HStack(spacing: 12) {
            Image(systemName: RoomBadge.symbol(kind)).font(.title2).frame(width: 36)
            VStack(alignment: .leading, spacing: 2) {
              Text(name).font(.headline)
              Text("A shared \(kindName)").font(.footnote).foregroundStyle(.secondary)
            }
          }
          LabeledContent("Server", value: server)
          if let invitation { LabeledContent("From", value: invitation.from) }
          if let joined, let info = joined.info {
            if let owner = info.member(info.owner), let rooms {
              LabeledContent("Shared by", value: rooms.displayName(of: owner, in: joined))
            }
            LabeledContent("Your role", value: info.role == .viewer ? "Viewer" : info.role == .owner ? "Owner" : "Editor")
          } else {
            LabeledContent("Your role", value: "Set by the invite")
          }
        }
        if joined == nil {
          if rooms == nil {
            Section {
              Text("Turn on sync in Settings to join shared items.").foregroundStyle(.secondary)
            }
          } else if kind != .space {
            Section {
              Picker("Space", selection: $spaceID) {
                ForEach(spaces) { Text($0.name).tag(Optional($0.id)) }
              }
            } header: {
              Text("Put It In")
            } footer: {
              Text("Where it shows is yours: others put it where they like.")
            }
          }
          Section {
            Button {
              Task { await join() }
            } label: {
              Label(invitation == nil ? "Join" : "Accept", systemImage: "person.crop.circle.badge.checkmark")
            }
            .disabled(working || rooms == nil)
            if let invitation {
              Button("Decline", role: .destructive) {
                Task {
                  try? await rooms?.decline(invitation)
                  close()
                }
              }
            }
          } footer: {
            Text("Its content is encrypted end to end: the server can't read it.")
          }
        } else {
          Section {
            Label("Joined. It's in your sidebar.", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
          }
        }
        if let error { Section { Text(error).foregroundStyle(.red) } }
      }
      .navigationTitle(invitation == nil ? "Join" : "Invitation")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .cancellationAction) { Button(joined == nil ? "Cancel" : "Done", action: close) }
        if working { ToolbarItem(placement: .confirmationAction) { ProgressView() } }
      }
      .onAppear {
        if spaceID == nil { spaceID = spaces.first?.id }
      }
    }
  }

  private func join() async {
    guard let rooms else { return }
    working = true
    error = nil
    defer { working = false }
    let space = spaceID.flatMap { rooms.store.space(id: $0) }
    do {
      if let link {
        guard let account = rooms.account(for: space) else { return }
        joined = try await rooms.join(link, account: account, space: space)
      } else if let invitation {
        joined = try await rooms.accept(invitation, space: space)
      }
    } catch let e as RoomError {
      error = e.description
    } catch {
      self.error = error.localizedDescription
    }
  }
}

/// Above the sidebar: you were removed from something, or someone invited
/// you by email.
struct RoomBanners: View {
  @Bindable var rooms: RoomsManager

  var body: some View {
    VStack(spacing: 6) {
      ForEach(rooms.notices) { notice in
        banner(icon: "person.crop.circle.badge.xmark", text: notice.message) {
          Button {
            rooms.notices.removeAll { $0.id == notice.id }
          } label: {
            Image(systemName: "xmark").font(.footnote.weight(.semibold))
          }
          .accessibilityLabel("Dismiss")
        }
      }
      ForEach(rooms.invitations) { invitation in
        banner(icon: "envelope.badge.person.crop", text: "\(invitation.from) invited you to a shared \(invitation.kind.rawValue).") {
          Button("View") { RoomsModel.shared.presentInvitation(invitation) }
            .font(.footnote.weight(.semibold))
        }
      }
    }
  }

  private func banner<Trailing: View>(icon: String, text: String, @ViewBuilder trailing: () -> Trailing) -> some View {
    HStack(spacing: 10) {
      Image(systemName: icon)
      Text(text).font(.footnote).fixedSize(horizontal: false, vertical: true)
      Spacer(minLength: 0)
      trailing()
    }
    .padding(10)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    .padding(.horizontal, 12)
  }
}

/// Settings → Sync: "Your Name for Sharing".
struct SharingNameRow: View {
  @State private var name = RoomsModel.shared.displayName

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      TextField("Your Name for Sharing", text: $name)
        .textContentType(.name)
        .submitLabel(.done)
        .onSubmit { RoomsModel.shared.displayName = name }
        .onDisappear { if name != RoomsModel.shared.displayName { RoomsModel.shared.displayName = name } }
      Text("What people you share with see, when the server doesn't have your name from company sign-in.")
        .font(.footnote)
        .foregroundStyle(.secondary)
    }
  }
}
