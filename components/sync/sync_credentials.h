// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_SYNC_CREDENTIALS_H_
#define COMPASS_COMPONENTS_SYNC_SYNC_CREDENTIALS_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "compass/components/sync/chain.h"

class PrefService;

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
  void Clear();
  bool HasKey() const;

  // A fresh go-sync token, or nullopt without a key. Any thread.
  std::optional<std::string> MakeToken(base::Time now) const;

  // The engine's way to try the server again at once: |retry| runs on
  // |runner| (the sync sequence). Set when the engine builds its scheduler
  // (CompassEngineComponentsFactory) by |owner|, which clears it as it goes
  // (unless a newer engine's replaced it meanwhile).
  void SetRetryHook(const void* owner,
                    scoped_refptr<base::SequencedTaskRunner> runner,
                    base::RepeatingClosure retry);
  void ClearRetryHook(const void* owner);
  // After the chain signed in (docs/design/sync.md, "Company sign-in
  // (OIDC)"): the engine backs off after the server's 401s, for up to ten
  // minutes; this ends the wait. Any thread; nothing without an engine.
  void RetryNow();

 private:
  friend class base::RefCountedThreadSafe<SyncCredentials>;
  ~SyncCredentials();

  mutable base::Lock lock_;
  std::optional<AuthKey> key_ GUARDED_BY(lock_);
  const void* retry_owner_ GUARDED_BY(lock_) = nullptr;
  scoped_refptr<base::SequencedTaskRunner> retry_runner_ GUARDED_BY(lock_);
  base::RepeatingClosure retry_ GUARDED_BY(lock_);
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_SYNC_CREDENTIALS_H_
