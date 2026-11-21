// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import CompassRooms
import Foundation

/// Launch switches for simulator runs and screenshots (debug builds):
///
/// - `-CompassRoomsJoin <link>`: the join preview of an invite link, as
///   if a tab had opened it.
/// - `-CompassRoomsAutoJoin YES` with it: join at once (into the first
///   space), no preview; the sidebar then shows the shared folder.
/// - `-CompassRoomsShareDemo YES`: a folder "Trip Ideas" with two tabs in
///   the first space, shared, a link made, and its share sheet shown.
///
/// Sync must be on (`-CompassServer` and `-CompassJoinRecovery`).
extension RoomsModel {
  func runDebugSwitches(_ app: AppModel) {
    #if DEBUG
      let defaults = UserDefaults.standard
      let join = defaults.string(forKey: "CompassRoomsJoin")
      let autoJoin = defaults.bool(forKey: "CompassRoomsAutoJoin")
      let shareDemo = defaults.bool(forKey: "CompassRoomsShareDemo")
      guard join != nil || shareDemo else { return }
      Task { @MainActor in
        // Sync starts and the accounts come up first.
        for _ in 0..<40 where manager?.accounts.isEmpty ?? true { try? await Task.sleep(for: .milliseconds(250)) }
        try? await Task.sleep(for: .seconds(2))
        guard let manager else {
          print("CompassRooms: sync is off")
          return
        }
        if let join, let link = RoomInviteLink(link: join) {
          if autoJoin {
            let space = app.store.spaces.first
            do {
              let room = try await manager.join(link, account: manager.account(for: space)!, space: space)
              print("CompassRoomsJoin: joined \(room.roomID) as \(room.role.rawValue)")
              app.selectedSpaceID = space?.id
            } catch {
              print("CompassRoomsJoin: \(error)")
            }
          } else {
            presentJoin(link)
          }
        }
        if shareDemo, let space = app.store.spaces.first, let account = manager.account(for: space) {
          let folder =
            space.pinned.children.first { $0.isFolder && $0.title == "Trip Ideas" }
            ?? {
              let f = app.store.addFolder(name: "Trip Ideas", to: space.pinned)!
              app.store.addTab(url: "https://www.lonelyplanet.com/japan", title: "Japan travel guide", to: f)
              app.store.addTab(url: "https://www.japan-guide.com/e/e2164.html", title: "Kyoto", to: f)
              return f
            }()
          do {
            var room = manager.room(rootedAt: folder)
            if room == nil {
              room = try await manager.share(kind: .folder, rootUUID: folder.id, name: folder.title, account: account)
            }
            guard let room else { return }
            _ = try await manager.inviteLink(room, role: .editor, expires: Date().addingTimeInterval(7 * 86400), uses: nil)
            share(folder: folder)
          } catch {
            print("CompassRoomsShareDemo: \(error)")
          }
        }
      }
    #endif
  }
}
