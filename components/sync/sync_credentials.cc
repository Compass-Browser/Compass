// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/sync_credentials.h"

#include <map>

#include "base/no_destructor.h"

namespace compass::sync {
namespace {

using Registry = std::map<const PrefService*, scoped_refptr<SyncCredentials>>;

Registry& GetRegistry() {
  static base::NoDestructor<Registry> registry;
  return *registry;
}

}  // namespace

// static
scoped_refptr<SyncCredentials> SyncCredentials::ForPrefs(
    const PrefService* prefs) {
  scoped_refptr<SyncCredentials>& credentials = GetRegistry()[prefs];
  if (!credentials) {
    credentials = base::MakeRefCounted<SyncCredentials>();
  }
  return credentials;
}

// static
void SyncCredentials::Forget(const PrefService* prefs) {
  GetRegistry().erase(prefs);
}

SyncCredentials::SyncCredentials() = default;
SyncCredentials::~SyncCredentials() = default;

void SyncCredentials::SetKey(const AuthKey& key) {
  base::AutoLock lock(lock_);
  key_ = key;
}

void SyncCredentials::Clear() {
  base::AutoLock lock(lock_);
  key_.reset();
}

bool SyncCredentials::HasKey() const {
  base::AutoLock lock(lock_);
  return key_.has_value();
}

std::optional<std::string> SyncCredentials::MakeToken(base::Time now) const {
  std::optional<AuthKey> key;
  {
    base::AutoLock lock(lock_);
    key = key_;
  }
  if (!key) {
    return std::nullopt;
  }
  return MakeAccessToken(*key, now);
}

}  // namespace compass::sync
