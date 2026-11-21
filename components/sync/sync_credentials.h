// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_SYNC_CREDENTIALS_H_
#define COMPASS_COMPONENTS_SYNC_SYNC_CREDENTIALS_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "compass/components/sync/chain.h"

class PrefService;

namespace base {
class SequencedTaskRunner;
}  // namespace base

namespace compass::sync {

// A profile's chain key, shared between the browser side, which decrypts the
// seed (asynchronously, with os_crypt_async), and the engine's connection
// manager, which signs each request with it on the sync sequence.
class SyncCredentials : public base::RefCountedThreadSafe<SyncCredentials> {
 public:
  // The one for |prefs|'s profile, created on first use. Main thread.
  static scoped_refptr<SyncCredentials> ForPrefs(const PrefService* prefs);
  // Drops the registry's reference (the profile is going away). Main thread.
  static void Forget(const PrefService* prefs);

  SyncCredentials();
  SyncCredentials(const SyncCredentials&) = delete;
  SyncCredentials& operator=(const SyncCredentials&) = delete;

  void SetKey(const AuthKey& key);
  // Forgets the key and the removed state.
  void Clear();
  bool HasKey() const;

  // The server said this device was removed from the chain
  // (device_state.h): the engine's connection manager stops sending
  // requests, and the sync page says so. Any thread; sticky until Clear().
  void SetDeviceRemoved();
  bool IsDeviceRemoved() const;

  // Whether the server's last answer to the engine was 401: it refuses this
  // account (a private server that doesn't list it, or wants a sign-in).
  // Set by the connection manager on the sync sequence; a change runs the
  // callback SetRefusedCallback() gave, on its sequence. Cleared by any
  // other answer, and by Clear().
  void SetRefused(bool refused);
  bool IsRefused() const;
  // Main thread: |callback| runs on it when IsRefused() changes.
  void SetRefusedCallback(base::RepeatingClosure callback);

  // A fresh go-sync token, or nullopt without a key. Any thread.
  std::optional<std::string> MakeToken(base::Time now) const;

 private:
  friend class base::RefCountedThreadSafe<SyncCredentials>;
  ~SyncCredentials();

  mutable base::Lock lock_;
  std::optional<AuthKey> key_ GUARDED_BY(lock_);
  bool device_removed_ GUARDED_BY(lock_) = false;
  bool refused_ GUARDED_BY(lock_) = false;
  scoped_refptr<base::SequencedTaskRunner> refused_runner_ GUARDED_BY(lock_);
  base::RepeatingClosure refused_callback_ GUARDED_BY(lock_);
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_SYNC_CREDENTIALS_H_
