// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftProtobuf
import SyncProto

/// The sync data types the iPhone runs (docs/design/ios.md), with their
/// EntitySpecifics field numbers, which are also the type IDs on the wire
/// (DataTypeProgressMarker.data_type_id, the SSE events' "types").
public enum DataType: Int32, Sendable, CaseIterable {
  case nigori = 47745
  case deviceInfo = 154522
  case compassSpaces = 970000
  case passwords = 45873
  case history = 963985
  case historyDeleteDirectives = 150251
  case sessions = 50119
  /// Download only (ExtensionsBridge).
  case extensions = 48119

  /// An EntitySpecifics with only this type's field set (empty). Chromium
  /// hashes its serialization into client tag hashes and sends it in
  /// GetUpdates to name the type.
  public var emptySpecifics: SyncPb_EntitySpecifics {
    var s = SyncPb_EntitySpecifics()
    switch self {
    case .nigori: s.nigori = .init()
    case .deviceInfo: s.deviceInfo = .init()
    case .compassSpaces: s.compassSpaces = .init()
    case .passwords: s.password = .init()
    case .history: s.history = .init()
    case .historyDeleteDirectives: s.historyDeleteDirective = .init()
    case .sessions: s.session = .init()
    case .extensions: s.extension = .init()
    }
    return s
  }
}
