// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassSync
import Foundation
import Testing

struct SyncOutcomeTests {
  let work = "2b5c3e1a-0000-4000-8000-00000000000b"
  let side = "2b5c3e1a-0000-4000-8000-00000000000c"

  @Test func everyAccountSynced() {
    let outcome = SyncOutcome(results: ["": .synced, work: .synced])
    #expect(outcome == SyncOutcome())
    #expect(outcome.homeSynced)
  }

  /// A profile the Mac made and hasn't started (its account has no NIGORI)
  /// waits; sync as a whole is on.
  @Test func aProfileNotStartedWaits() {
    let outcome = SyncOutcome(results: ["": .synced, work: .notStarted, side: .synced])
    #expect(outcome.failure == nil)
    #expect(outcome.profileProblems == [work: SyncOutcome.waitingMessage])
  }

  @Test func aProfileFailureIsOnlyThatProfiles() {
    let outcome = SyncOutcome(results: ["": .synced, work: .failed("Can't reach the sync server.")])
    #expect(outcome.homeSynced)
    #expect(outcome.profileProblems == [work: "Can't reach the sync server."])
  }

  @Test func theHomeDecides() {
    let failed = SyncOutcome(results: ["": .failed("Can't reach the sync server."), work: .synced])
    #expect(failed.failure == "Can't reach the sync server.")
    #expect(failed.profileProblems.isEmpty)
    let notStarted = SyncOutcome(results: ["": .notStarted, work: .notStarted])
    #expect(notStarted.failure == SyncOutcome.homeNotStartedMessage)
    #expect(notStarted.profileProblems == [work: SyncOutcome.waitingMessage])
  }
}

struct AccountLinkerTests {
  @Test func urlAndStatuses() {
    #expect(
      AccountLinker.url(URL(string: "https://sync.example.org/v2/")!).absoluteString
        == "https://sync.example.org/v2/compass/account/link")
    #expect(AccountLinker.result(status: 204) == .linked)
    // Servers without linking: nothing to do.
    #expect(AccountLinker.result(status: 404) == .linked)
    #expect(AccountLinker.result(status: 401) == .refused)
    #expect(AccountLinker.result(status: 429) == .failed("HTTP 429"))
  }
}
