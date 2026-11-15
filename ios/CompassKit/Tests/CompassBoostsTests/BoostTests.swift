// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassBoosts

#if canImport(JavaScriptCore)
  import JavaScriptCore
#endif

struct BoostMatchTests {
  @Test func parsesAndNormalizes() throws {
    #expect(BoostMatch("example.com")?.pattern == "example.com")
    #expect(BoostMatch("  Example.COM. ")?.pattern == "example.com")
    #expect(BoostMatch("https://github.com/settings*")?.pattern == "github.com/settings")
    #expect(BoostMatch("*.example.com")?.pattern == "*.example.com")
    #expect(BoostMatch("example.com/")?.pattern == "example.com")
    let sub = try #require(BoostMatch("*.Example.com/a/b"))
    #expect(sub.subdomains && sub.host == "example.com" && sub.path == "/a/b")
    for bad in ["", "*", "*.", "exa mple.com", "example.com:8080", "ex*ample.com", "a..b", "café.com",
      "example.com/?q", "example.com/a#b", "*.*.example.com"]
    {
      #expect(BoostMatch(bad) == nil, "\(bad)")
    }
  }

  @Test func matchesHostsAndPaths() throws {
    let plain = try #require(BoostMatch("example.com"))
    #expect(plain.matches(URL(string: "https://example.com/")!))
    #expect(plain.matches(URL(string: "http://www.example.com/x?y")!))
    #expect(plain.matches(URL(string: "https://EXAMPLE.com:8443/x")!))
    #expect(!plain.matches(URL(string: "https://mail.example.com/")!))
    #expect(!plain.matches(URL(string: "https://notexample.com/")!))
    #expect(!plain.matches(URL(string: "ftp://example.com/")!))
    #expect(!plain.matches(URL(string: "file:///example.com")!))

    let wild = try #require(BoostMatch("*.example.com"))
    #expect(wild.matches(URL(string: "https://example.com")!))
    #expect(wild.matches(URL(string: "https://a.b.example.com/")!))
    #expect(!wild.matches(URL(string: "https://badexample.com/")!))

    let path = try #require(BoostMatch("github.com/settings"))
    #expect(path.matches(URL(string: "https://github.com/settings")!))
    #expect(path.matches(URL(string: "https://github.com/settings/profile")!))
    #expect(!path.matches(URL(string: "https://github.com/")!))
    #expect(!path.matches(URL(string: "https://github.com/user/settings")!))
    #expect(path.matchesHost("www.github.com"))
  }

  @Test func suggestsTheSite() {
    #expect(BoostMatch.suggested(for: URL(string: "https://www.example.com/a")!) == "example.com")
    #expect(BoostMatch.suggested(for: URL(string: "https://news.ycombinator.com/")!) == "news.ycombinator.com")
    #expect(BoostMatch.suggested(for: URL(string: "about:blank")!) == nil)
  }

  #if canImport(JavaScriptCore)
    /// The script's check says what `matches` says.
    @Test func javaScriptAgreesWithSwift() throws {
      let patterns = ["example.com", "*.example.com", "github.com/settings", "*.github.com/a"]
      let urls = [
        "https://example.com/", "http://www.example.com/x", "https://a.example.com/", "https://badexample.com/",
        "https://github.com/settings/x", "https://github.com/", "https://gist.github.com/a/b", "ftp://example.com/",
        "https://example.com./",
      ]
      let context = try #require(JSContext())
      for pattern in patterns {
        let match = try #require(BoostMatch(pattern))
        for spec in urls {
          let url = try #require(URL(string: spec))
          let location: [String: String] = [
            "protocol": (url.scheme ?? "") + ":", "hostname": url.host ?? "",
            "pathname": url.path(percentEncoded: true).isEmpty ? "/" : url.path(percentEncoded: true),
          ]
          context.setObject(location, forKeyedSubscript: "location" as NSString)
          let js = context.evaluateScript(match.javaScriptCondition)?.toBool()
          #expect(js == match.matches(url), "\(pattern) \(spec)")
        }
      }
    }

    @Test func scriptsParse() throws {
      let boost = Boost(
        name: "Quote \" and </script>", match: "example.com", css: "body { content: \"\\\" \u{2028}\"; }",
        js: "// last line comment")
      let context = try #require(JSContext())
      var exception: JSValue?
      context.exceptionHandler = { _, value in exception = value }
      context.setObject(["protocol": "https:", "hostname": "other.org", "pathname": "/"], forKeyedSubscript: "location" as NSString)
      context.evaluateScript(try #require(BoostScripts.css(boost)))
      context.evaluateScript(try #require(BoostScripts.js(boost)))
      #expect(exception == nil)
      #expect(BoostScripts.css(Boost(name: "n", match: "example.com")) == nil)
    }
  #endif
}

struct BoostListTests {
  let now = Date(timeIntervalSince1970: 1_800_000_000)

  @Test func createUpdateRemoveRestore() throws {
    var list = BoostList()
    let made = try list.resolve(
      BoostDraft(name: " Hide ads ", match: "Example.com", css: ".ad{display:none}"), author: .claude, now: now
    ).get()
    #expect(made.name == "Hide ads" && made.match == "example.com" && made.author == .claude)
    #expect(made.jsWorld == .isolated && made.enabled && made.js.isEmpty)
    #expect(made.createTime == Boost.windowsMicros(now) && made.updateTime == made.createTime)
    let ok1 = list.put(made)
    #expect(ok1)

    // Absent fields stay; an update is newer, keeps its author.
    let later = now.addingTimeInterval(10)
    let edited = try list.resolve(
      BoostDraft(id: made.id.uppercased(), name: "Hide ads", match: "example.com", js: "x()", jsWorld: "main"),
      author: .user, now: later
    ).get()
    #expect(edited.id == made.id && edited.css == made.css && edited.js == "x()" && edited.jsWorld == .main)
    #expect(edited.author == .claude && edited.updateTime == Boost.windowsMicros(later))
    // Nothing changed: the same Boost, same times.
    let same = try list.resolve(BoostDraft(id: made.id, name: "Hide ads", match: "example.com"), author: .user).get()
    #expect(same == made)

    list.put(edited)
    #expect(list.matching(URL(string: "https://www.example.com/")!).map(\.id) == [made.id])
    let removedBoost = list.remove(made.id)
    let removed = try #require(removedBoost)
    #expect(list.boosts.isEmpty)
    let ok2 = list.restore(removed, now: now)
    #expect(ok2)
    #expect(list.find(made.id)?.js == "x()")
    #expect(list.find(made.id)!.updateTime >= removed.updateTime)
  }

  @Test func refusesBadDrafts() {
    let list = BoostList()
    #expect(list.resolve(BoostDraft(name: " ", match: "a.com"), author: .user) == .failure(.emptyName))
    #expect(
      list.resolve(BoostDraft(name: String(repeating: "n", count: 101), match: "a.com"), author: .user)
        == .failure(.nameTooLong))
    #expect(list.resolve(BoostDraft(name: "n", match: "a b"), author: .user) == .failure(.badMatch("a b")))
    #expect(
      list.resolve(BoostDraft(name: "n", match: "a.com", jsWorld: "page"), author: .user) == .failure(.badWorld("page")))
    #expect(list.resolve(BoostDraft(id: "nope", name: "n", match: "a.com"), author: .user) == .failure(.notFound("nope")))
    let big = String(repeating: "a", count: BoostList.maxBytes / 2 + 1)
    #expect(list.resolve(BoostDraft(name: "n", match: "a.com", css: big, js: big), author: .user) == .failure(.tooLarge))
    let exact = String(repeating: "a", count: BoostList.maxBytes / 2)
    #expect((try? list.resolve(BoostDraft(name: "n", match: "a.com", css: exact, js: exact), author: .user).get()) != nil)
  }

  @Test func caps() throws {
    var list = BoostList()
    for i in 0..<BoostList.maxBoosts {
      let boost = try list.resolve(BoostDraft(name: "b\(i)", match: "a.com"), author: .user).get()
      let ok3 = list.put(boost)
      #expect(ok3)
    }
    #expect(list.resolve(BoostDraft(name: "one more", match: "a.com"), author: .user) == .failure(.full))
    // Sync isn't capped.
    let remote = Boost(name: "remote", match: "a.com", createTime: 5, updateTime: 5)
    let ok4 = list.applyRemote([remote], deleted: [])
    #expect(ok4)
    #expect(list.boosts.count == BoostList.maxBoosts + 1)
  }

  @Test func approvalRouting() {
    let css = Boost(name: "n", match: "a.com", css: "x{}")
    #expect(!BoostList.needsApproval(css, before: nil))
    var withJS = css
    withJS.js = "go()"
    #expect(BoostList.needsApproval(withJS, before: nil))
    #expect(BoostList.needsApproval(withJS, before: css))
    // The same JavaScript, new CSS: no.
    var restyled = withJS
    restyled.css = "y{}"
    #expect(!BoostList.needsApproval(restyled, before: withJS))
    // Disabling: no. Elsewhere, or in the page's world: yes.
    var off = withJS
    off.enabled = false
    #expect(!BoostList.needsApproval(off, before: withJS))
    var moved = withJS
    moved.match = "*.a.com"
    #expect(BoostList.needsApproval(moved, before: withJS))
    var main = withJS
    main.jsWorld = .main
    #expect(BoostList.needsApproval(main, before: withJS))
    // Removing the JavaScript: no.
    #expect(!BoostList.needsApproval(css, before: withJS))
    #expect(BoostList.deleteNeedsApproval(withJS) && !BoostList.deleteNeedsApproval(css))
  }

  @Test func applyRemoteLaterWins() {
    let id = UUID().uuidString.lowercased()
    var list = BoostList(boosts: [Boost(id: id, name: "local", match: "a.com", createTime: 1, updateTime: 10)])
    let ok5 = !list.applyRemote([Boost(id: id, name: "older", match: "a.com", createTime: 1, updateTime: 5)], deleted: [])
    #expect(ok5)
    #expect(list.find(id)?.name == "local")
    let ok6 = list.applyRemote([Boost(id: id, name: "newer", match: "a.com", createTime: 1, updateTime: 20)], deleted: [])
    #expect(ok6)
    #expect(list.find(id)?.name == "newer")
    // Invalid ones are ignored: not a UUID, a pattern not normalized.
    let ok7 = !list.applyRemote([Boost(id: "x", name: "bad", match: "a.com")], deleted: [])
    #expect(ok7)
    let ok8 = !list.applyRemote([Boost(name: "bad", match: "A.com")], deleted: [])
    #expect(ok8)
    let ok9 = list.applyRemote([], deleted: [id])
    #expect(ok9)
    #expect(list.boosts.isEmpty)
  }

  @Test func jsonRoundTripWithSnakeCaseKeys() throws {
    let boost = Boost(
      name: "n", match: "*.a.com/p", css: "c", js: "j", jsWorld: .main, enabled: false, author: .claude, createTime: 7,
      updateTime: 9)
    let data = BoostList(boosts: [boost]).encoded()
    let raw = try #require(try JSONSerialization.jsonObject(with: data) as? [[String: Any]])
    #expect(raw.first?["js_world"] as? String == "main")
    #expect(raw.first?["create_time"] as? Int == 7 && raw.first?["update_time"] as? Int == 9)
    #expect(raw.first?["author"] as? String == "claude" && raw.first?["enabled"] as? Bool == false)
    #expect(BoostList.decode(data)?.boosts == [boost])

    // Defaults for what's missing; unknown values are the safe ones.
    let id = UUID().uuidString.lowercased()
    let sparse = Data(#"[{"id":"\#(id)","match":"a.com","js_world":"future","author":"robot"}]"#.utf8)
    let decoded = try #require(BoostList.decode(sparse)?.boosts.first)
    #expect(decoded.jsWorld == .isolated && decoded.author == .user && decoded.enabled && decoded.css.isEmpty)
  }

  @Test func toolOutput() throws {
    let a = Boost(name: "A", match: "example.com", css: "a{}", js: "b()")
    let b = Boost(name: "B", match: "other.org", css: "c{}")
    let list = BoostList(boosts: [a, b])
    let all = try #require(JSONSerialization.jsonObject(with: Data(BoostTools.list(list, host: nil).utf8)) as? [String: Any])
    let rows = try #require(all["boosts"] as? [[String: Any]])
    #expect(rows.count == 2 && rows[0]["css"] == nil && rows[0]["js_bytes"] as? Int == 3)
    let site = try #require(
      JSONSerialization.jsonObject(with: Data(BoostTools.list(list, host: "www.example.com").utf8)) as? [String: Any])
    let siteRows = try #require(site["boosts"] as? [[String: Any]])
    #expect(siteRows.count == 1 && siteRows[0]["js"] as? String == "b()")
    #expect(BoostTools.saved(a).contains("\"needs_reload\":true"))

    #expect(BoostTools.lineDiff(from: "a\nb\nc", to: "a\nB\nc") == "  a\n- b\n+ B\n  c")
    var changed = a
    changed.js = "b()\nc()"
    #expect(BoostTools.approvalDetail(changed, before: a).hasSuffix("  b()\n+ c()"))
    #expect(BoostTools.approvalDetail(a, before: nil).hasSuffix("\n\nb()"))
  }
}
