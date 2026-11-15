// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassModel
import Foundation
import Testing

/// Link rules (docs/design/link-routing.md), as the desktop's
/// LinkRulesTest.
@MainActor
struct LinkRoutingTests {
  private func url(_ text: String) -> URL { URL(string: text)! }

  // MARK: Domains

  @Test func domainsAreNormalized() {
    #expect(LinkRouting.normalizedDomain("GitHub.COM") == "github.com")
    #expect(LinkRouting.normalizedDomain("  github.com. ") == "github.com")
    #expect(LinkRouting.normalizedDomain("*.github.com") == "github.com")
    #expect(LinkRouting.normalizedDomain("www.github.com") == "github.com")
    #expect(LinkRouting.normalizedDomain("gist.github.com") == "gist.github.com")
    #expect(LinkRouting.normalizedDomain("github.com:8443") == "github.com")
    #expect(LinkRouting.normalizedDomain("https://www.github.com/compass?a#b") == "github.com")
    #expect(LinkRouting.normalizedDomain("HTTP://Docs.Example.com:8080/") == "docs.example.com")
    #expect(LinkRouting.normalizedDomain("github.com/compass") == "github.com")
    // "www." goes only with a domain after it.
    #expect(LinkRouting.normalizedDomain("www.com") == "www.com")
    #expect(LinkRouting.normalizedDomain("www.co.uk") == nil)
    // Unknown suffixes and private registries are fine.
    #expect(LinkRouting.normalizedDomain("router.internal") == "router.internal")
    #expect(LinkRouting.normalizedDomain("compass.github.io") == "compass.github.io")
    #expect(LinkRouting.normalizedDomain("bbc.co.uk") == "bbc.co.uk")
  }

  @Test func notDomainsAreRefused() {
    for text in [
      "", "  ", ".", ".github.com", "git..hub.com", "git hub.com", "ex!ample.com", "gi*thub.com", "*",
      "localhost", "com", "co.uk", "com.au", "10.0.0.1", "https://192.168.1.1/a", "[::1]", "http://[::1]/",
      "ftp://github.com", "file:///tmp/a", String(repeating: "a", count: 64) + ".com",
    ] {
      #expect(LinkRouting.normalizedDomain(text) == nil, "\(text)")
    }
  }

  @Test func idnDomainsArePunycode() {
    #expect(LinkRouting.normalizedDomain("bücher.de") == "xn--bcher-kva.de")
    #expect(LinkRouting.normalizedDomain("BÜCHER.de") == "xn--bcher-kva.de")
    #expect(LinkRouting.normalizedDomain("www.bücher.de") == "xn--bcher-kva.de")
    #expect(LinkRouting.normalizedDomain("日本語。jp") == "xn--wgv71a119e.jp")
    #expect(LinkRouting.normalizedDomain("münchen-ost.de") == "xn--mnchen-ost-9db.de")
    #expect(LinkRouting.normalizedDomain("xn--bcher-kva.de") == "xn--bcher-kva.de")
    // Percent-encoded, as some URL parsers leave a host.
    #expect(LinkRouting.normalizedDomain("b%C3%BCcher.de") == "xn--bcher-kva.de")
  }

  @Test func punycodeDisplaysAsUnicode() {
    #expect(LinkRouting.displayDomain("xn--bcher-kva.de") == "bücher.de")
    #expect(LinkRouting.displayDomain("www.xn--wgv71a119e.jp") == "www.日本語.jp")
    #expect(LinkRouting.displayDomain("xn--mnchen-ost-9db.de") == "münchen-ost.de")
    #expect(LinkRouting.displayDomain("github.com") == "github.com")
    // Not punycode after all: as it is.
    #expect(LinkRouting.displayDomain("xn--!!.com") == "xn--!!.com")
  }

  @Test func suggestionsAreTheNormalizedHost() {
    #expect(LinkRouting.suggestedDomain(for: url("https://www.github.com/a")) == "github.com")
    #expect(LinkRouting.suggestedDomain(for: url("https://docs.github.com/a")) == "docs.github.com")
    #expect(LinkRouting.suggestedDomain(for: url("http://10.0.0.1/")) == nil)
    #expect(LinkRouting.suggestedDomain(for: url("http://localhost:8080/")) == nil)
  }

  // MARK: Matching

  @Test func aDomainMatchesItsSubdomains() {
    let rules = [LinkRule(domain: "github.com", spaceID: "work")]
    for text in ["https://github.com", "http://gist.github.com/x", "https://api.eu.github.com:8080/", "https://www.github.com"] {
      #expect(LinkRouting.rule(for: url(text), in: rules)?.spaceID == "work", "\(text)")
    }
    // Look-alikes.
    for text in ["https://notgithub.com", "https://github.com.evil.org", "https://github.co", "https://com"] {
      #expect(LinkRouting.rule(for: url(text), in: rules) == nil, "\(text)")
    }
  }

  @Test func theMostSpecificRuleWins() {
    let rules = [
      LinkRule(domain: "github.com", spaceID: "work"),
      LinkRule(domain: "gist.github.com", spaceID: "notes"),
      LinkRule(domain: "a.gist.github.com", spaceID: "school"),
    ]
    #expect(LinkRouting.rule(for: url("https://github.com/compass"), in: rules)?.spaceID == "work")
    #expect(LinkRouting.rule(for: url("https://gist.github.com/1"), in: rules)?.spaceID == "notes")
    #expect(LinkRouting.rule(for: url("https://x.gist.github.com"), in: rules)?.spaceID == "notes")
    #expect(LinkRouting.rule(for: url("https://b.a.gist.github.com"), in: rules)?.spaceID == "school")
    // The order of the rules doesn't matter.
    #expect(LinkRouting.rule(for: url("https://gist.github.com"), in: rules.reversed())?.spaceID == "notes")
  }

  @Test func matchingIgnoresCasePortsAndPaths() {
    let rules = [LinkRule(domain: "github.com", spaceID: "work")]
    #expect(LinkRouting.rule(for: url("HTTPS://GitHub.COM:443/Compass?q=1#top"), in: rules)?.spaceID == "work")
    #expect(LinkRouting.rule(for: url("https://github.com./"), in: rules)?.spaceID == "work")
  }

  @Test func idnLinksMatchIdnRules() {
    let rules = [LinkRule(domain: LinkRouting.normalizedDomain("bücher.de")!, spaceID: "books")]
    #expect(LinkRouting.rule(for: url("https://xn--bcher-kva.de/"), in: rules)?.spaceID == "books")
    #expect(LinkRouting.rule(for: url("https://shop.xn--bcher-kva.de/"), in: rules)?.spaceID == "books")
    if let unicode = URL(string: "https://www.bücher.de/a") {
      #expect(LinkRouting.rule(for: unicode, in: rules)?.spaceID == "books")
    }
  }

  @Test func onlyWebPagesOnADomainMatch() {
    let rules = [LinkRule(domain: "github.com", spaceID: "work"), LinkRule(domain: "0.1", spaceID: "ip")]
    for text in [
      "ftp://github.com/a", "mailto:me@github.com", "compass://open?url=https://github.com", "file:///tmp/a",
      "https://10.0.0.1/", "http://[::1]/",
    ] {
      #expect(LinkRouting.rule(for: url(text), in: rules) == nil, "\(text)")
    }
  }

  // MARK: The store

  @Test func oneRulePerDomain() {
    let store = SpaceStore()
    let work = store.addSpace(name: "Work")
    let home = store.addSpace(name: "Home")
    var changes: [SpaceStoreChange] = []
    store.onLocalChange = { changes.append($0) }
    #expect(store.setLinkRule("https://www.GitHub.com/x", to: work) == "github.com")
    #expect(store.linkRules == [LinkRule(domain: "github.com", spaceID: work.id)])
    #expect(changes.last?.linkRules == [LinkRule.ID(profile: "", domain: "github.com")])
    // Pointing it at another space changes it.
    store.setLinkRule("github.com", to: home)
    #expect(store.linkRules == [LinkRule(domain: "github.com", spaceID: home.id)])
    #expect(changes.count == 2)
    // The same again: no change.
    store.setLinkRule("github.com", to: home)
    #expect(changes.count == 2)
    #expect(store.setLinkRule("not a domain", to: work) == nil)
    store.setLinkRule("b.com", to: home)
    store.setLinkRule("a.com", to: work)
    #expect(store.linkRules.map(\.domain) == ["a.com", "b.com", "github.com"])
    store.removeLinkRule("b.com", profile: "")
    #expect(store.linkRules.map(\.domain) == ["a.com", "github.com"])
    #expect(changes.last?.linkRules == [LinkRule.ID(profile: "", domain: "b.com")])
  }

  @Test func routingPicksTheRulesSpace() {
    let store = SpaceStore()
    let personal = store.addSpace(name: "Personal")
    let work = store.addSpace(name: "Work")
    store.setLinkRule("google.com", to: personal)
    store.setLinkRule("docs.google.com", to: work)
    #expect(store.opening(forLinkFromAnotherApp: url("https://docs.google.com/d"), littleWindows: true) == .space(work.id))
    #expect(store.opening(forLinkFromAnotherApp: url("https://mail.google.com"), littleWindows: true) == .space(personal.id))
    // No rule: as before the rules.
    #expect(store.opening(forLinkFromAnotherApp: url("https://example.com"), littleWindows: true) == .littleWindow)
    #expect(store.opening(forLinkFromAnotherApp: url("https://example.com"), littleWindows: false) == .shownSpace)
  }

  /// A rule outlives its space and routes nothing meanwhile; one more
  /// specific to a deleted space doesn't hide its parent's.
  @Test func aDeletedSpacesRuleIsIgnored() {
    let store = SpaceStore()
    let personal = store.addSpace(name: "Personal")
    let work = store.addSpace(name: "Work")
    store.setLinkRule("github.com", to: personal)
    store.setLinkRule("gist.github.com", to: work)
    store.deleteSpace(work)
    #expect(store.linkRules.count == 2)
    #expect(store.linkSpace(for: url("https://gist.github.com/1"))?.id == personal.id)
    store.deleteSpace(personal)
    #expect(store.opening(forLinkFromAnotherApp: url("https://github.com"), littleWindows: true) == .littleWindow)
    // A rule from another device for a space not here (yet).
    store.applyingRemote { store.rawSetLinkRule(LinkRule(domain: "linear.app", spaceID: "not-here")) }
    #expect(store.linkSpace(for: url("https://linear.app")) == nil)
  }

  @Test func neverIntoIncognito() {
    let store = SpaceStore()
    let personal = store.addSpace(name: "Personal")
    #expect(store.setLinkRule("github.com", to: store.incognito) == nil)
    #expect(store.linkRules.isEmpty)
    // A rule naming "incognito" (from the wire) routes nothing.
    store.applyingRemote { store.rawSetLinkRule(LinkRule(domain: "github.com", spaceID: store.incognito.id)) }
    #expect(store.opening(forLinkFromAnotherApp: url("https://github.com"), littleWindows: false) == .shownSpace)
    store.setLinkRule("github.com", to: personal)
    #expect(store.linkSpace(for: url("https://github.com"))?.isIncognito == false)
  }

  @Test func eachProfileHasItsRules() {
    let store = SpaceStore()
    let home = store.addSpace(name: "Home")
    let work = store.addSpace(name: "Work", profile: "11111111-2222-4333-8444-555555555555")
    store.setLinkRule("github.com", to: home)
    store.setLinkRule("github.com", to: work)
    #expect(store.linkRules.count == 2)
    // The shown profile's rule, else the home's.
    #expect(store.linkSpace(for: url("https://github.com"))?.id == home.id)
    #expect(store.linkSpace(for: url("https://github.com"), preferring: work.profileUUID)?.id == work.id)
    #expect(store.linkSpace(for: url("https://github.com"), preferring: "")?.id == home.id)
    store.removeLinkRule("github.com", profile: work.profileUUID)
    #expect(store.linkSpace(for: url("https://github.com"), preferring: work.profileUUID)?.id == home.id)
  }

  @Test func rulesAreSaved() throws {
    let store = SpaceStore()
    let work = store.addSpace(name: "Work")
    let other = store.addSpace(name: "Other", profile: "11111111-2222-4333-8444-555555555555")
    store.setLinkRule("github.com", to: work)
    store.setLinkRule("bücher.de", to: other)
    store.deleteSpace(work)
    let copy = SpaceStore()
    try copy.load(snapshot: store.encodedSnapshot())
    #expect(copy.linkRules == store.linkRules)
    // Files from before the rules load with none.
    let plain = SpaceStore()
    plain.addSpace(name: "Old")
    let older = SpaceStore()
    try older.load(snapshot: plain.encodedSnapshot())
    let none = older.linkRules.isEmpty
    #expect(none)
  }
}
