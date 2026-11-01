// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if canImport(WebKit)
  import Foundation
  import Testing
  import WebKit

  @testable import CompassModel

  /// The real lists, converted and compiled by WebKit itself (which rejects
  /// a whole list over one bad regex). Online, so only with
  /// COMPASS_LIVE_FILTER_LISTS=1.
  @Suite(.enabled(if: ProcessInfo.processInfo.environment["COMPASS_LIVE_FILTER_LISTS"] == "1"))
  struct FilterListsLiveTests {
    @MainActor @Test(arguments: FilterList.allCases)
    func compiles(_ list: FilterList) async throws {
      let (data, _) = try await URLSession.shared.data(from: list.source)
      let text = String(decoding: data, as: UTF8.self)
      let start = Date()
      let result = ABPConverter.convert(text)
      let json = try result.json()
      print("\(list.name): \(result.converted) converted, \(result.skipped) skipped, \(result.rules.count) rules, \(json.utf8.count / 1024) KB, converted in \(Date().timeIntervalSince(start))s")
      #expect(result.rules.count < 150_000)
      let compileStart = Date()
      let ruleList = try await WKContentRuleListStore.default().compileContentRuleList(
        forIdentifier: "test-\(list.rawValue)", encodedContentRuleList: json)
      print("\(list.name): compiled in \(Date().timeIntervalSince(compileStart))s")
      #expect(ruleList != nil)
    }
  }
#endif
