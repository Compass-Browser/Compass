// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compass syncs History end-to-end encrypted through local sync
// (docs/design/sync.md, "History"). Upstream never encrypts HISTORY
// and HISTORY_DELETE_DIRECTIVES (kNeverEncrypted: Google's server reads
// history) and leaves them out of local sync. Here both become encryptable,
// so a custom passphrase ("encrypt everything") covers them, and supported
// in local sync. The table entries stay as upstream has them, so this file
// doesn't conflict with patches to the table (a new data type).
//
// SEND_TAB_TO_SELF ("Send to Your Devices", docs/design/send-tab.md) is
// supported in local sync too: upstream leaves it out, and without it the
// type never starts and no menu offers sending.
//
// Outside local sync nothing changes: LocalSyncSupportedTypes() is only read
// with local sync on, and EncryptableUserTypes() only with a custom
// passphrase, under which the history controllers stop history unless local
// sync is on (patches/components-history-core-browser-sync-*.patch).

#include "components/sync/base/data_type.h"

namespace syncer {
DataTypeSet EncryptableUserTypes_ChromiumImpl();
DataTypeSet LocalSyncSupportedTypes_ChromiumImpl();
}  // namespace syncer

#define EncryptableUserTypes EncryptableUserTypes_ChromiumImpl
#define LocalSyncSupportedTypes LocalSyncSupportedTypes_ChromiumImpl
#include "src/components/sync/base/data_type.cc"
#undef LocalSyncSupportedTypes
#undef EncryptableUserTypes

namespace syncer {

DataTypeSet EncryptableUserTypes() {
  DataTypeSet types = EncryptableUserTypes_ChromiumImpl();
  types.Put(HISTORY);
  types.Put(HISTORY_DELETE_DIRECTIVES);
  return types;
}

DataTypeSet LocalSyncSupportedTypes() {
  DataTypeSet types = LocalSyncSupportedTypes_ChromiumImpl();
  types.Put(HISTORY);
  types.Put(HISTORY_DELETE_DIRECTIVES);
  types.Put(SEND_TAB_TO_SELF);
  return types;
}

}  // namespace syncer
