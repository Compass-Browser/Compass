// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassModel
import Testing

struct CommandMatchTests {
  @Test func matchesEveryWord() {
    #expect(CommandMatch.matches("Settings", "sett"))
    #expect(CommandMatch.matches("Reopen Closed Tab", "tab clo"))
    #expect(CommandMatch.matches("Settings", ""))
    #expect(!CommandMatch.matches("Settings", "set x"))
  }

  @Test func namesByWholeLeadingWords() {
    #expect(CommandMatch.names("Settings", "settings"))
    #expect(CommandMatch.names("Settings", "  SETTINGS "))
    #expect(CommandMatch.names("New Incognito Tab", "new incognito"))
    #expect(CommandMatch.names("Find on Page…", "find on page"))
    #expect(!CommandMatch.names("Settings", "sett"))
    #expect(!CommandMatch.names("New Incognito Tab", "incognito"))
    #expect(!CommandMatch.names("Settings", "settings page"))
    #expect(!CommandMatch.names("Settings", ""))
  }
}
