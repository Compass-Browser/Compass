// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import SyncProto

/// Link rules in COMPASS_SPACES (docs/design/link-routing.md, "Sync"): one
/// `CompassLinkRule` per domain, keyed by it, in the account of the rule's
/// profile, as the desktop's `CompassSpacesSyncBridge::AddLocalLinkRules()`.
extension CompassSpacesBridge {
  /// The entity's UUID (and client tag) for a rule's domain, as the
  /// desktop's `LinkRuleKey()`.
  public static func linkRuleUUID(domain: String) -> String {
    derivedUUID("compass-link-rule:" + domain)
  }

  /// The profile of the rules this account carries (`LinkRule.profile`).
  @MainActor var linkRuleProfile: String { isHome ? "" : accountProfile }

  /// A rule from the wire this version applies: a space, a domain that
  /// normalizes to itself and the key that goes with it. Others (a later
  /// version's form without `space_uuid`) stay in the mirror only.
  static func isUsable(_ r: SyncPb_CompassLinkRule) -> Bool {
    !r.spaceUuid.isEmpty && LinkRouting.normalizedDomain(r.domain) == r.domain
      && r.uuid == linkRuleUUID(domain: r.domain)
  }

  /// What we'd commit for a rule: the mirror with our fields over it, or
  /// nil while its space stays on this device.
  @MainActor
  func desired(_ rule: LinkRule) -> SyncPb_CompassSpacesSpecifics? {
    if isLocalSpace(id: rule.spaceID) { return nil }
    let id = Self.linkRuleUUID(domain: rule.domain)
    var specifics = mirror[id] ?? SyncPb_CompassSpacesSpecifics()
    var r = specifics.linkRule
    r.uuid = id
    r.domain = rule.domain
    r.spaceUuid = rule.spaceID
    specifics.linkRule = r
    return specifics
  }

  /// Rules set, changed or removed here, and the rules of spaces whose
  /// sync was turned on or off: commits or deletes them.
  @MainActor
  func linkRulesChanged(_ ids: Set<LinkRule.ID>, spaces: Set<String>) {
    let profile = linkRuleProfile
    var domains = Set(ids.filter { $0.profile == profile }.map(\.domain))
    for rule in store.linkRules where rule.profile == profile && spaces.contains(rule.spaceID) {
      domains.insert(rule.domain)
    }
    for domain in domains {
      let id = Self.linkRuleUUID(domain: domain)
      if let rule = store.linkRule(domain: domain, profile: profile), let specifics = desired(rule) {
        commit(id, specifics)
      } else if case .linkRule? = mirror[id]?.entity {
        deleteFromServer([id])
      }
    }
  }

  /// Rules from the server: set or removed here, without echo.
  @MainActor
  func applyLinkRules(_ put: [SyncPb_CompassLinkRule], deleted: [SyncPb_CompassLinkRule]) {
    let profile = linkRuleProfile
    store.applyingRemote {
      for r in deleted { store.rawRemoveLinkRule(LinkRule.ID(profile: profile, domain: r.domain)) }
      for r in put where Self.isUsable(r) {
        store.rawSetLinkRule(LinkRule(domain: r.domain, spaceID: r.spaceUuid, profile: profile))
      }
    }
  }

  /// This account's rules for the initial merge.
  @MainActor
  var localLinkRules: [(id: String, specifics: SyncPb_CompassSpacesSpecifics?)] {
    store.linkRules.filter { $0.profile == linkRuleProfile }.map {
      (Self.linkRuleUUID(domain: $0.domain), desired($0))
    }
  }
}
