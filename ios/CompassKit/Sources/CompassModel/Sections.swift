// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The sidebar model, a port of the desktop's browser/sidebar/ (M3). The
// raw values are COMPASS_SPACES's wire values (compass_spaces_specifics.proto);
// 0 means "not set".

public enum Section: Int32, Sendable, CaseIterable {
  case pinned = 1
  case today = 2
  case favorites = 3
  case archive = 4
}

public enum NodeType: Int32, Sendable, CaseIterable {
  case tab = 1
  case folder = 2
  case split = 3
}

/// Space colors: compass_spaces_specifics.proto's SpaceColor values.
public enum SpaceColor: Int32, Sendable, CaseIterable {
  case grey = 1, blue, purple, pink, red, orange, yellow, green
}
