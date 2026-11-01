// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/sync_settings.h"

#include "base/strings/strcat.h"
#include "base/values.h"
#include "compass/components/sync/pref_names.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/sync/service/sync_user_settings.h"

namespace compass::sync {

std::string_view SyncDataTypeName(SyncDataType type) {
  switch (type) {
    case SyncDataType::kPasswords:
      return "passwords";
    case SyncDataType::kHistory:
      return "history";
    case SyncDataType::kBookmarks:
      return "bookmarks";
    case SyncDataType::kAutofill:
      return "autofill";
    case SyncDataType::kExtensions:
      return "extensions";
    case SyncDataType::kSettings:
      return "settings";
    case SyncDataType::kReadingList:
      return "reading_list";
    case SyncDataType::kOpenTabs:
      return "open_tabs";
    case SyncDataType::kSpaces:
      return "spaces";
  }
  return "";
}

std::optional<SyncDataType> SyncDataTypeFromName(std::string_view name) {
  for (SyncDataType type : kAllSyncDataTypes) {
    if (SyncDataTypeName(type) == name) {
      return type;
    }
  }
  return std::nullopt;
}

std::optional<syncer::UserSelectableType> GetSelectableType(SyncDataType type) {
  switch (type) {
    case SyncDataType::kPasswords:
      return syncer::UserSelectableType::kPasswords;
    case SyncDataType::kHistory:
      return syncer::UserSelectableType::kHistory;
    case SyncDataType::kBookmarks:
      return syncer::UserSelectableType::kBookmarks;
    case SyncDataType::kAutofill:
      return syncer::UserSelectableType::kAutofill;
    case SyncDataType::kExtensions:
      return syncer::UserSelectableType::kExtensions;
    case SyncDataType::kSettings:
      return syncer::UserSelectableType::kPreferences;
    case SyncDataType::kReadingList:
      return syncer::UserSelectableType::kReadingList;
    case SyncDataType::kOpenTabs:
      return syncer::UserSelectableType::kTabs;
    case SyncDataType::kSpaces:
      return std::nullopt;
  }
  return std::nullopt;
}

syncer::UserSelectableTypeSet GetUnwantedSelectableTypes() {
  // docs/design/sync.md, "What syncs" and "Slice 2 notes". WebAuthn
  // credentials ride with Passwords; their controller is left out instead
  // (patches/chrome-browser-sync-sync_service_factory.cc.patch).
  return {
      syncer::UserSelectableType::kApps,
      syncer::UserSelectableType::kPayments,
      syncer::UserSelectableType::kProductComparison,
      syncer::UserSelectableType::kSavedTabGroups,
      syncer::UserSelectableType::kThemes,
  };
}

void RegisterSyncSettingsPrefs(PrefRegistrySimple* registry) {
  registry->RegisterListPref(prefs::kDisabledTypes);
}

SyncDataTypeSet GetDisabledSyncDataTypes(const PrefService* prefs) {
  SyncDataTypeSet disabled;
  for (const base::Value& item : prefs->GetList(prefs::kDisabledTypes)) {
    if (const std::string* name = item.GetIfString()) {
      if (std::optional<SyncDataType> type = SyncDataTypeFromName(*name)) {
        disabled.insert(*type);
      }
    }
  }
  return disabled;
}

void SetSyncDataTypeEnabled(PrefService* prefs,
                            SyncDataType type,
                            bool enabled) {
  ScopedListPrefUpdate update(prefs, prefs::kDisabledTypes);
  base::ListValue& list = update.Get();
  const std::string name(SyncDataTypeName(type));
  list.EraseValue(base::Value(name));
  if (!enabled) {
    list.Append(name);
  }
}

syncer::UserSelectableTypeSet ComputeSelectedTypes(
    syncer::UserSelectableTypeSet registered,
    const SyncDataTypeSet& disabled) {
  syncer::UserSelectableTypeSet types = registered;
  types.RemoveAll(GetUnwantedSelectableTypes());
  for (SyncDataType type : disabled) {
    if (std::optional<syncer::UserSelectableType> selectable =
            GetSelectableType(type)) {
      types.Remove(*selectable);
    }
  }
  return types;
}

void HoldSelectableTypes(syncer::SyncUserSettings* settings) {
  settings->SetSelectedTypes(/*sync_everything=*/false,
                             syncer::UserSelectableTypeSet());
}

void SelectSelectableTypes(syncer::SyncUserSettings* settings,
                           const SyncDataTypeSet& disabled) {
  settings->SetSelectedTypes(
      /*sync_everything=*/false,
      ComputeSelectedTypes(settings->GetRegisteredSelectableTypes(), disabled));
}

std::string GetDeviceKind(syncer::DeviceInfo::OsType os,
                          syncer::DeviceInfo::FormFactor form_factor) {
  using FormFactor = syncer::DeviceInfo::FormFactor;
  using OsType = syncer::DeviceInfo::OsType;
  const bool tablet = form_factor == FormFactor::kTablet;
  const bool phone = form_factor == FormFactor::kPhone;
  switch (os) {
    case OsType::kMac:
      return "Mac";
    case OsType::kWindows:
      return "Windows PC";
    case OsType::kLinux:
      return "Linux computer";
    case OsType::kChromeOsAsh:
    case OsType::kChromeOsLacros:
      return "Chromebook";
    case OsType::kIOS:
      return tablet ? "iPad" : "iPhone";
    case OsType::kAndroid:
      return tablet ? "Android tablet" : "Android phone";
    case OsType::kFuchsia:
    case OsType::kUnknown:
      break;
  }
  if (phone) {
    return "Phone";
  }
  if (tablet) {
    return "Tablet";
  }
  return "Computer";
}

std::u16string GetSyncMenuLabel(const SyncStatus& status) {
  switch (status.limit) {
    case SyncLimit::kServerFull:
      return u"Sync: server full";
    case SyncLimit::kThrottled:
      return u"Sync: paused by server";
    case SyncLimit::kHistoryFull:
      if (status.state == SyncState::kOn) {
        return u"Synced (history full)";
      }
      break;
    case SyncLimit::kNone:
      break;
  }
  switch (status.state) {
    case SyncState::kOff:
      return u"Sync is off";
    case SyncState::kNeedsRestart:
      return u"Sync: restart to finish";
    case SyncState::kStarting:
      return u"Syncing…";
    case SyncState::kOn:
      return u"Synced";
    case SyncState::kError:
      return u"Sync error";
  }
  return u"Sync";
}

SyncLimit GetSyncLimit(bool throttled, bool server_full, bool history_full) {
  if (server_full) {
    return SyncLimit::kServerFull;
  }
  if (throttled) {
    return SyncLimit::kThrottled;
  }
  if (history_full) {
    return SyncLimit::kHistoryFull;
  }
  return SyncLimit::kNone;
}

void ApplySyncLimit(SyncLimit limit, SyncStatus& status) {
  if (status.state != SyncState::kOn || limit == SyncLimit::kNone) {
    return;
  }
  status.limit = limit;
  switch (limit) {
    case SyncLimit::kServerFull:
      status.state = SyncState::kError;
      status.message =
          "The sync server is full for this chain: new changes stay on this "
          "device. Remove data you don't need, or ask the server's owner to "
          "raise its limits.";
      return;
    case SyncLimit::kThrottled:
      status.state = SyncState::kError;
      status.message =
          "The sync server is holding this device back (too many devices in "
          "the chain, or too many requests). Compass tries again by itself.";
      return;
    case SyncLimit::kHistoryFull:
      status.message = base::StrCat(
          {status.message, status.message.empty() ? "" : " ",
           "History is full on the server: new visits stay on this device "
           "until older ones expire."});
      return;
    case SyncLimit::kNone:
      return;
  }
}

}  // namespace compass::sync
