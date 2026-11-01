// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import SwiftProtobuf
import SyncProto

/// Passwords use Chromium's own nested scheme (data_type_worker.cc,
/// EncryptPasswordSpecificsData / DecryptPasswordSpecifics): on the wire
/// `password.encrypted` holds the serialized PasswordSpecificsData and
/// `password.encrypted_notes_backup` its notes; the outer `encrypted` field
/// isn't used, and with a custom passphrase no unencrypted metadata is sent.
/// Locally the data lives in `client_only_encrypted_data`, which must never
/// reach the server.
extension Cryptographer {
  public func encrypt(password specifics: SyncPb_EntitySpecifics) -> SyncPb_EntitySpecifics {
    let data = specifics.password.clientOnlyEncryptedData
    var out = SyncPb_EntitySpecifics()
    out.password.encrypted = encrypt(try! data.serializedBytes())
    out.password.encryptedNotesBackup = encrypt(try! data.notes.serializedBytes())
    return out
  }

  /// The specifics with `client_only_encrypted_data` filled in, or nil.
  public func decrypt(password specifics: SyncPb_EntitySpecifics) -> SyncPb_EntitySpecifics? {
    guard specifics.password.hasEncrypted, let plaintext = decrypt(specifics.password.encrypted),
      var data = try? SyncPb_PasswordSpecificsData(serializedBytes: plaintext)
    else { return nil }
    // Older clients dropped notes; the backup has them.
    if data.notes.note.isEmpty, specifics.password.hasEncryptedNotesBackup,
      let notes = decrypt(specifics.password.encryptedNotesBackup),
      let parsed = try? SyncPb_PasswordSpecificsData.Notes(serializedBytes: notes)
    {
      data.notes = parsed
    }
    var out = SyncPb_EntitySpecifics()
    out.password.clientOnlyEncryptedData = data
    return out
  }
}
