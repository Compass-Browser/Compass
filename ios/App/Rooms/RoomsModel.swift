// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassEasels
import CompassModel
import CompassRooms
import CompassShared
import CompassSync
import Foundation
import Observation
import SwiftUI
import UIKit

/// Sharing with other people in the app (docs/design/ios.md, "Rooms"): the
/// rooms of the chain's accounts (CompassRooms' RoomsManager), started and
/// stopped by SyncController, and the share sheet and join preview,
/// presented by UIKit over everything (not from a List row, whose rebuild
/// would take the sheet with it: ios.md, "Presentations").
@MainActor
@Observable
final class RoomsModel {
  static let shared = RoomsModel()

  private(set) var manager: RoomsManager?
  @ObservationIgnored private var stateDirectory: URL?
  @ObservationIgnored private var presenting = false

  static let nameKey = "CompassSharingName"

  /// "Your Name for Sharing".
  var displayName: String {
    get { UserDefaults.standard.string(forKey: Self.nameKey) ?? "" }
    set {
      UserDefaults.standard.set(newValue, forKey: Self.nameKey)
      manager?.setDisplayName(newValue)
    }
  }

  // MARK: Hooks from SyncController

  /// Sync started with a chain: rooms follow its accounts.
  func syncStarted(directory: SpaceDirectory, stateDirectory: URL) {
    self.stateDirectory = stateDirectory
    manager = RoomsManager(
      directory: directory, easels: EaselStore.shared, stateDirectory: stateDirectory,
      displayName: UserDefaults.standard.string(forKey: Self.nameKey) ?? "")
    manager?.setActive(UIApplication.shared.applicationState != .background)
  }

  /// An account of the chain started (its bridge carries its mounts).
  func accountStarted(
    profile: String, bridge: CompassSpacesBridge, chain: Chain, server: URL, cacheGUID: String
  ) {
    guard let manager else { return }
    manager.addAccount(
      RoomsManager.Account(
        profile: profile, bridge: bridge, serverURL: server, authKey: chain.authKey(profileUUID: profile),
        identity: RoomCrypto.identityKey(chain: chain, profileUUID: profile), cacheGUID: cacheGUID))
    Task { await manager.checkServer(profile) }
  }

  /// Sync left: rooms stop; local copies stay, unshared.
  func syncStopped() {
    manager?.stop()
    manager = nil
    if let stateDirectory { try? FileManager.default.removeItem(at: stateDirectory) }
  }

  func setActive(_ active: Bool) { manager?.setActive(active) }

  /// A `room` event from an account's change stream.
  func handle(_ event: SyncNotifier.RoomEvent, account: String) { manager?.handle(event, account: account) }

  // MARK: What's shared

  func room(rootedAt node: SidebarNode) -> RoomSync? { manager?.room(rootedAt: node) }
  func room(ofSpace space: Space) -> RoomSync? { manager?.room(ofSpace: space.id) }
  func room(ofEasel id: String) -> RoomSync? { manager?.room(ofEasel: id) }
  func canEdit(_ node: SidebarNode) -> Bool { manager?.canEdit(node) ?? true }
  func canEdit(easel id: String) -> Bool { manager?.canEdit(easel: id) ?? true }
  var isAvailable: Bool { manager?.accounts.isEmpty == false }

  // MARK: Share sheet

  /// What the share sheet is about.
  enum Target {
    case space(Space)
    case folder(SidebarNode)
    case easel(String, title: String)

    var kind: RoomKind {
      switch self {
      case .space: .space
      case .folder: .folder
      case .easel: .easel
      }
    }
  }

  func share(space: Space) { present(.space(space)) }
  func share(folder: SidebarNode) { present(.folder(folder)) }
  func share(easel id: String) {
    present(.easel(id, title: EaselStore.shared.easel(id)?.title ?? "Easel"))
  }

  private func present(_ target: Target) {
    guard let manager, !presenting else { return }
    presenting = true
    var controller: UIViewController?
    let view = ShareSheet(target: target, rooms: manager, close: { [weak self] in
      self?.presenting = false
      controller?.dismiss(animated: true)
    })
    let hosting = UIHostingController(rootView: view)
    hosting.presentationController?.delegate = Dismissal.shared
    controller = hosting
    presenting = Presenter.present(hosting)
  }

  // MARK: Joining

  /// An invite link (a tab's navigation, a compass://join deep link, or a
  /// link from another app): the join preview. Whether it was one.
  @discardableResult
  func open(_ url: URL) -> Bool {
    guard let link = Self.inviteLink(url) else { return false }
    presentJoin(link)
    return true
  }

  /// The invite in `url`, if it's an invite link.
  static func inviteLink(_ url: URL) -> RoomInviteLink? {
    let text = url.absoluteString
    if text.lowercased().hasPrefix("https://www.compassbrowser.org/join#") {
      return RoomInviteLink(link: "https://compassbrowser.org/join#" + (url.fragment(percentEncoded: true) ?? ""))
    }
    return RoomInviteLink(link: text)
  }

  func presentJoin(_ link: RoomInviteLink) {
    guard !presenting else { return }
    presenting = true
    var controller: UIViewController?
    let view = JoinSheet(link: link, invitation: nil, close: { [weak self] in
      self?.presenting = false
      controller?.dismiss(animated: true)
    })
    let hosting = UIHostingController(rootView: view)
    hosting.presentationController?.delegate = Dismissal.shared
    controller = hosting
    presenting = Presenter.present(hosting)
  }

  /// An email invitation: the same preview, accepting it.
  func presentInvitation(_ invitation: RoomsManager.Invitation) {
    guard !presenting else { return }
    presenting = true
    var controller: UIViewController?
    let view = JoinSheet(link: nil, invitation: invitation, close: { [weak self] in
      self?.presenting = false
      controller?.dismiss(animated: true)
    })
    let hosting = UIHostingController(rootView: view)
    hosting.presentationController?.delegate = Dismissal.shared
    controller = hosting
    presenting = Presenter.present(hosting)
  }

  /// A sheet swiped away.
  @MainActor
  private final class Dismissal: NSObject, UIAdaptivePresentationControllerDelegate {
    static let shared = Dismissal()
    func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
      RoomsModel.shared.presenting = false
    }
  }
}
