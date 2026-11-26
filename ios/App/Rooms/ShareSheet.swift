// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassEasels
import CompassModel
import CompassRooms
import SwiftUI

/// Share a space, a pinned folder or an easel with other people
/// (docs/design/ios.md, "Rooms"): an invite link (role, expiry, uses),
/// invites by email on company sign-in servers, the members with their
/// roles, and Leave or Delete for Everyone.
struct ShareSheet: View {
  let target: RoomsModel.Target
  @Bindable var rooms: RoomsManager
  let close: () -> Void

  @State private var working = false
  @State private var error: String?
  @State private var role: RoomRole = .editor
  @State private var expiry: Expiry = .week
  @State private var uses: Uses = .unlimited
  @State private var link: String?
  @State private var copied = false
  @State private var email = ""
  @State private var emailRole: RoomRole = .editor
  @State private var emailResult: String?
  @State private var confirmingEnd = false
  @State private var removing: RoomMember?

  enum Expiry: String, CaseIterable, Identifiable {
    case day = "1 Day", week = "7 Days", never = "Never"
    var id: String { rawValue }
    var title: String {
      switch self {
      case .day: String(localized: "1 Day", comment: "Share sheet: an invite link expires after a day.")
      case .week: String(localized: "7 Days", comment: "Share sheet: an invite link expires after a week.")
      case .never: String(localized: "Never", comment: "Share sheet: an invite link never expires.")
      }
    }
    var date: Date? {
      switch self {
      case .day: Date().addingTimeInterval(24 * 60 * 60)
      case .week: Date().addingTimeInterval(7 * 24 * 60 * 60)
      case .never: nil
      }
    }
  }

  enum Uses: String, CaseIterable, Identifiable {
    case one = "1 Person", five = "5 People", twentyFive = "25 People", unlimited = "No Limit"
    var id: String { rawValue }
    var title: String {
      switch self {
      case .one: String(localized: "1 Person", comment: "Share sheet: how many people an invite link can be used by.")
      case .five: String(localized: "5 People", comment: "Share sheet: how many people an invite link can be used by.")
      case .twentyFive: String(localized: "25 People", comment: "Share sheet: how many people an invite link can be used by.")
      case .unlimited: String(localized: "No Limit", comment: "Share sheet: an invite link anyone can use.")
      }
    }
    var count: Int? {
      switch self {
      case .one: 1
      case .five: 5
      case .twentyFive: 25
      case .unlimited: nil
      }
    }
  }

  private var store: SpaceStore { rooms.store }

  /// The room of the item, if it's shared: its own, or (a board) the room
  /// of the folder or space it's in.
  private var room: RoomSync? {
    _ = rooms.revision
    switch target {
    case .space(let space): return rooms.room(ofSpace: space.id)
    case .folder(let node): return rooms.room(rootedAt: node) ?? rooms.room(of: node)
    case .easel(let id, _): return rooms.rooms.values.first { $0.kind == .easel && $0.rootUUID == id } ?? rooms.room(ofEasel: id)
    }
  }

  private var title: String {
    switch target {
    case .space(let space): space.name
    case .folder(let node):
      node.title.isEmpty ? String(localized: "Folder", comment: "Share sheet: a folder with no name.") : node.title
    case .easel(_, let title): title
    }
  }

  private var shareTitle: String {
    switch target.kind {
    case .space: String(localized: "Share This Space", comment: "Share sheet button: start sharing a space with other people.")
    case .folder: String(localized: "Share This Folder", comment: "Share sheet button: start sharing a folder with other people.")
    case .easel: String(localized: "Share This Easel", comment: "Share sheet button: start sharing an Easel with other people.")
    }
  }

  var body: some View {
    NavigationStack {
      Form {
        Section {
          HStack(spacing: 12) {
            Image(systemName: RoomBadge.symbol(target.kind))
              .font(.title2)
              .frame(width: 36)
            VStack(alignment: .leading, spacing: 2) {
              Text(title).font(.headline)
              Text(subtitle).font(.footnote).foregroundStyle(.secondary)
            }
          }
        }
        if let room {
          sharedSections(room)
        } else {
          Section {
            Button {
              Task { await share() }
            } label: {
              Label(shareTitle, systemImage: "person.crop.circle.badge.plus")
            }
            .disabled(working || rooms.accounts.isEmpty)
          } footer: {
            Text(
              "People you invite see it and, as editors, change it with you. It stays encrypted: the server can't read it."
            )
          }
        }
        if let error {
          Section { Text(error).foregroundStyle(.red) }
        }
      }
      .navigationTitle("Share")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .confirmationAction) { Button("Done", action: close) }
        if working { ToolbarItem(placement: .topBarLeading) { ProgressView() } }
      }
      .confirmationDialog(
        endTitle, isPresented: $confirmingEnd, titleVisibility: .visible
      ) {
        Button(room?.role == .owner ? "Delete for Everyone" : "Leave", role: .destructive) {
          Task { await end() }
        }
      } message: {
        Text("Your copy stays on this device, no longer shared.")
      }
      .confirmationDialog(
        "Remove \(removing.map { name($0) } ?? "")?", isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } }),
        titleVisibility: .visible
      ) {
        Button("Remove", role: .destructive) {
          if let member = removing { Task { await remove(member) } }
        }
      } message: {
        Text("They keep what they have, but don't see changes from now on.")
      }
      .task(id: room?.roomID) {
        guard let room else { return }
        await rooms.sync(room)
      }
    }
  }

  private var subtitle: String {
    guard let room else { return String(localized: "Not shared", comment: "Share sheet: the item isn't shared.") }
    if room.rootUUID != rootID {
      return String(
        localized: "Shared with \u{201C}\(room.name)\u{201D}",
        comment: "Share sheet: the item is shared as part of a shared folder or space; the argument is its name.")
    }
    let count = room.info?.members.count ?? 1
    let others = count - 1
    let who =
      others == 0
      ? String(localized: "Shared with no one yet", comment: "Share sheet: shared, but no one else joined yet.")
      : String(localized: "Shared with \(others) people", comment: "Share sheet: how many other people it's shared with.")
    return "\(who) · \(room.role.displayName)"
  }

  private var rootID: String {
    switch target {
    case .space(let space): space.id
    case .folder(let node): node.id
    case .easel(let id, _): id
    }
  }

  private var endTitle: String {
    guard let room else { return "" }
    return room.role == .owner
      ? String(
        localized: "Delete \u{201C}\(room.name)\u{201D} for everyone?",
        comment: "Share sheet confirmation title: the owner ends sharing; the argument is the item's name.")
      : String(
        localized: "Leave \u{201C}\(room.name)\u{201D}?",
        comment: "Share sheet confirmation title: a member stops sharing; the argument is the item's name.")
  }

  @ViewBuilder private func sharedSections(_ room: RoomSync) -> some View {
    if room.role == .owner {
      Section {
        Picker("Role", selection: $role) {
          Text("Editor").tag(RoomRole.editor)
          Text("Viewer").tag(RoomRole.viewer)
        }
        Picker("Expires", selection: $expiry) {
          ForEach(Expiry.allCases) { Text($0.title).tag($0) }
        }
        Picker("Can Be Used By", selection: $uses) {
          ForEach(Uses.allCases) { Text($0.title).tag($0) }
        }
        if let link, let url = URL(string: link) {
          Button {
            UIPasteboard.general.url = url
            copied = true
          } label: {
            Label(copied ? "Copied" : "Copy Invite Link", systemImage: copied ? "checkmark" : "doc.on.doc")
          }
          ShareLink(item: url, subject: Text(title), message: Text("Join \u{201C}\(title)\u{201D} in Compass")) {
            Label("Send Invite Link…", systemImage: "square.and.arrow.up")
          }
        } else {
          Button {
            Task { await makeLink(room) }
          } label: {
            Label("Create Invite Link", systemImage: "link.badge.plus")
          }
          .disabled(working)
        }
      } header: {
        Text("Invite Link")
      } footer: {
        Text("Anyone with the link can join as \(role.displayName.lowercased()) until it expires.")
      }
      .onChange(of: role) { link = nil; copied = false }
      .onChange(of: expiry) { link = nil; copied = false }
      .onChange(of: uses) { link = nil; copied = false }
      if rooms.canInviteByEmail(room) {
        Section {
          TextField("Email", text: $email)
            .keyboardType(.emailAddress)
            .textContentType(.emailAddress)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
          Picker("Role", selection: $emailRole) {
            Text("Editor").tag(RoomRole.editor)
            Text("Viewer").tag(RoomRole.viewer)
          }
          Button("Invite") { Task { await invite(room) } }
            .disabled(working || !email.contains("@"))
          if let emailResult { Text(emailResult).font(.footnote).foregroundStyle(.secondary) }
        } header: {
          Text("Invite by Email")
        }
      }
    }
    Section("People") {
      ForEach(room.info?.members ?? []) { member in
        memberRow(member, in: room)
      }
      if room.info == nil {
        HStack {
          ProgressView()
          Text("Loading…").foregroundStyle(.secondary)
        }
      }
    }
    if room.role == .owner, let invites = room.info?.invites?.filter({ !$0.revoked }), !invites.isEmpty {
      Section("Open Links") {
        ForEach(invites) { invite in
          HStack {
            VStack(alignment: .leading) {
              Text(invite.role.displayName)
              Text(inviteDetail(invite)).font(.footnote).foregroundStyle(.secondary)
            }
            Spacer()
            Button("Turn Off", role: .destructive) {
              Task {
                await run { try await rooms.revokeInvite(room, invite: invite.id) }
              }
            }
            .buttonStyle(.borderless)
          }
        }
      }
    }
    if room.rootUUID == rootID {
      Section {
        Button(room.role == .owner ? "Delete for Everyone" : "Leave", role: .destructive) { confirmingEnd = true }
      } footer: {
        Text(room.role == .owner
          ? "Everyone keeps a copy that's no longer shared."
          : "You keep a copy that's no longer shared.")
      }
    }
  }

  private func memberRow(_ member: RoomMember, in room: RoomSync) -> some View {
    let me = rooms.isMe(member, in: room)
    return HStack(spacing: 12) {
      Image(systemName: "person.crop.circle.fill")
        .font(.title2)
        .foregroundStyle(.secondary)
      VStack(alignment: .leading, spacing: 2) {
        Text(
          me
            ? String(localized: "\(name(member)) (You)", comment: "Share sheet: your own row in the member list.")
            : name(member))
        let invited = String(localized: "Invited", comment: "Share sheet: a member invited by email who hasn't joined yet.")
        let detail = [member.email, member.pending ? invited : ""].filter { !$0.isEmpty }.joined(separator: " · ")
        if !detail.isEmpty { Text(detail).font(.footnote).foregroundStyle(.secondary) }
      }
      Spacer()
      if room.role == .owner, member.role != .owner {
        Menu {
          Picker("Role", selection: Binding(
            get: { member.role },
            set: { new in Task { await run { try await rooms.setRole(new, member: member.clientID, in: room) } } })
          ) {
            Text("Editor").tag(RoomRole.editor)
            Text("Viewer").tag(RoomRole.viewer)
          }
          Button("Remove", role: .destructive) { removing = member }
        } label: {
          Text(member.role.displayName).font(.subheadline)
        }
      } else {
        Text(member.role.displayName).font(.subheadline).foregroundStyle(.secondary)
      }
    }
  }

  private func name(_ member: RoomMember) -> String {
    guard let room else { return member.name }
    return rooms.displayName(of: member, in: room)
  }


  private func inviteDetail(_ invite: RoomInvite) -> String {
    var parts: [String] = []
    if invite.expires > 0 {
      let date = Date(timeIntervalSince1970: TimeInterval(invite.expires) / 1000)
      let until = date.formatted(date: .abbreviated, time: .shortened)
      parts.append(
        date < Date()
          ? String(localized: "Expired", comment: "Share sheet: an invite link that has expired.")
          : String(localized: "Until \(until)", comment: "Share sheet: when an invite link expires; the argument is a date."))
    } else {
      parts.append(String(localized: "No expiry", comment: "Share sheet: an invite link that never expires."))
    }
    if let left = invite.usesLeft {
      parts.append(
        left == 0
          ? String(localized: "Used up", comment: "Share sheet: an invite link with no uses left.")
          : String(localized: "\(left) left", comment: "Share sheet: how many more people can use an invite link."))
    }
    return parts.joined(separator: " · ")
  }

  // MARK: Actions

  private func run(_ body: () async throws -> Void) async {
    working = true
    error = nil
    defer { working = false }
    do {
      try await body()
    } catch let e as RoomError {
      error = e.description
    } catch {
      self.error = error.localizedDescription
    }
  }

  private func share() async {
    await run {
      let account: RoomsManager.Account?
      switch target {
      case .space(let space): account = rooms.account(for: space)
      case .folder(let node): account = rooms.account(for: store.space(of: node))
      case .easel: account = rooms.accounts[""] ?? rooms.account(for: nil)
      }
      guard let account else { return }
      try await rooms.share(kind: target.kind, rootUUID: rootID, name: title, account: account)
    }
  }

  private func makeLink(_ room: RoomSync) async {
    await run {
      link = try await rooms.inviteLink(room, role: role, expires: expiry.date, uses: uses.count)
      copied = false
    }
  }

  private func invite(_ room: RoomSync) async {
    await run {
      let count = try await rooms.invite(email: email, role: emailRole, to: room)
      emailResult =
        count == 0
        ? String(
          localized: "No one with that email has signed in to Compass on this server.",
          comment: "Share sheet: inviting by email found no one.")
        : String(
          localized: "Invited \(email). They'll see the invitation in Compass.",
          comment: "Share sheet: an email invitation was sent; the argument is the address.")
      if count > 0 { email = "" }
    }
  }

  private func remove(_ member: RoomMember) async {
    guard let room else { return }
    removing = nil
    await run { try await rooms.remove(member: member.clientID, from: room) }
  }

  private func end() async {
    guard let room else { return }
    await run {
      if room.role == .owner {
        try await rooms.deleteForEveryone(room)
      } else {
        try await rooms.leave(room)
      }
    }
    if error == nil { close() }
  }
}

/// The people badge of shared items.
struct RoomBadge: View {
  var role: RoomRole?

  static func symbol(_ kind: RoomKind) -> String {
    switch kind {
    case .space: "rectangle.stack.badge.person.crop"
    case .folder: "folder.badge.person.crop"
    case .easel: "scribble.variable"
    }
  }

  var body: some View {
    Image(systemName: role == .viewer ? "eye" : "person.2.fill")
      .font(.system(size: 11, weight: .semibold))
      .opacity(0.55)
      .accessibilityLabel(role == .viewer ? "Shared with you, view only" : "Shared")
  }
}

extension RoomRole {
  /// The role's name in the share sheet and the join preview.
  var displayName: String {
    switch self {
    case .owner: String(localized: "Owner", comment: "A shared item's member role.")
    case .editor: String(localized: "Editor", comment: "A shared item's member role.")
    case .viewer: String(localized: "Viewer", comment: "A shared item's member role.")
    }
  }
}
