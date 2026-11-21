// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// One sync cycle across a device's accounts (docs/design/ios.md, "M8"):
/// only the home account decides whether sync as a whole works. A profile
/// account that fails, or that no device has started yet (a profile the Mac
/// made and hasn't synced: its account has no NIGORI until it does), is
/// noted on that profile and doesn't stop or fail the others.
public struct SyncOutcome: Equatable, Sendable {
  public enum AccountResult: Equatable, Sendable {
    case synced
    /// The account has no NIGORI: no device has started it yet.
    case notStarted
    case failed(String)
  }

  /// Settings' words for a profile account no device has started.
  public static let waitingMessage = "Waiting for the Mac to start syncing this profile."
  /// A 401 on a profile account: a private server that doesn't know it
  /// (it lets profile accounts in once linked, `AccountLinker`).
  public static let profileRefusedMessage =
    "The sync server refused this profile (it may serve only invited people). Ask the server's owner to allow it."
  /// A 401 on the home account.
  public static let homeRefusedMessage =
    "The sync server refused this device's chain (it may serve only invited people). Ask the server's owner to allow it."
  /// And for a home account without a NIGORI.
  public static let homeNotStartedMessage = "No device has started sync on this chain yet."

  /// The home account's failure: sync failed. Nil when the home synced.
  public var failure: String?
  /// Per profile account (not the home), why it didn't sync this time.
  public var profileProblems: [String: String] = [:]

  public init(failure: String? = nil, profileProblems: [String: String] = [:]) {
    self.failure = failure
    self.profileProblems = profileProblems
  }

  /// `results` by account ("" = the home's).
  public init(results: [String: AccountResult]) {
    for (profile, result) in results {
      let message: String? =
        switch result {
        case .synced: nil
        case .notStarted: profile.isEmpty ? Self.homeNotStartedMessage : Self.waitingMessage
        case .failed(let message): message
        }
      guard let message else { continue }
      if profile.isEmpty {
        failure = message
      } else {
        profileProblems[profile] = message
      }
    }
  }

  /// Whether the cycle counts as synced (the home account went through).
  public var homeSynced: Bool { failure == nil }
}
