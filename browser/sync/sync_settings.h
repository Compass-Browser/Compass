// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_SYNC_SETTINGS_H_
#define COMPASS_BROWSER_SYNC_SYNC_SETTINGS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/flat_set.h"
#include "compass/browser/sync/sync_controller.h"
#include "components/sync/base/user_selectable_type.h"
#include "components/sync_device_info/device_info.h"

class PrefRegistrySimple;
class PrefService;

namespace syncer {
class SyncUserSettings;
}  // namespace syncer

// What chrome://compass-sync's switches mean for Chromium's sync, and the
// small pieces of the page and the app menu that don't need a profile
// (docs/design/sync.md, "UI"). Kept apart from
// CompassSyncService so they're unit-testable.
namespace compass::sync {

// The switches, in the page's order.
inline constexpr SyncDataType kAllSyncDataTypes[] = {
    SyncDataType::kPasswords,   SyncDataType::kHistory,
    SyncDataType::kBookmarks,   SyncDataType::kAutofill,
    SyncDataType::kExtensions,  SyncDataType::kSettings,
    SyncDataType::kReadingList, SyncDataType::kOpenTabs,
    SyncDataType::kSpaces,
};

// The pref's name for a switch: "passwords", "open_tabs"...
std::string_view SyncDataTypeName(SyncDataType type);
std::optional<SyncDataType> SyncDataTypeFromName(std::string_view name);

// The upstream type a switch selects; nullopt for kSpaces (COMPASS_SPACES,
// gated by its controller instead).
std::optional<syncer::UserSelectableType> GetSelectableType(SyncDataType type);

// The selectable types Compass never syncs (docs/design/sync.md, "What
// syncs"); off and not on the page.
syncer::UserSelectableTypeSet GetUnwantedSelectableTypes();

using SyncDataTypeSet = base::flat_set<SyncDataType>;

// The switches turned off, from prefs::kDisabledTypes (names; unknown ones
// are skipped, so a newer version's switches survive a downgrade).
void RegisterSyncSettingsPrefs(PrefRegistrySimple* registry);
SyncDataTypeSet GetDisabledSyncDataTypes(const PrefService* prefs);
void SetSyncDataTypeEnabled(PrefService* prefs,
                            SyncDataType type,
                            bool enabled);

// What to select once the chain's passphrase is in place: the registered
// types, minus the unwanted ones and those whose switch is off. Selectable
// types without a switch (cookies, say) follow upstream's default: on.
syncer::UserSelectableTypeSet ComputeSelectedTypes(
    syncer::UserSelectableTypeSet registered,
    const SyncDataTypeSet& disabled);

// Nothing selected: the state until the passphrase is in place.
void HoldSelectableTypes(syncer::SyncUserSettings* settings);
// The switches applied (ComputeSelectedTypes()).
void SelectSelectableTypes(syncer::SyncUserSettings* settings,
                           const SyncDataTypeSet& disabled);

// "Mac", "Windows PC", "iPhone", "Android tablet"...
std::string GetDeviceKind(syncer::DeviceInfo::OsType os,
                          syncer::DeviceInfo::FormFactor form_factor);

// The app menu's sync row: "Synced", "Syncing…", "Sync error", "Sync is
// off", "Restart to sync"; with a limit, "Sync: server full", "Sync: paused
// by server", "Synced (history full)".
std::u16string GetSyncMenuLabel(const SyncStatus& status);

// Turns |status| into the removed-device state (docs/design/sync.md,
// "Removing a device"): an error whose way out is leaving.
void ApplyDeviceRemoved(SyncStatus& status);

// What identifies a physical device across the chain's profile accounts
// (docs/design/sync.md, "Removing a device"). DEVICE_INFO has no ID shared
// between a device's profiles: each profile has its own cache GUID, and
// local sync's signin_scoped_device_id is "local_device" everywhere.
struct DeviceSignature {
  std::string name;
  int os_type = 0;
  int form_factor = 0;
  std::string manufacturer;
  std::string model;
  std::string hardware_class;

  friend bool operator==(const DeviceSignature&,
                         const DeviceSignature&) = default;
};

DeviceSignature GetDeviceSignature(const syncer::DeviceInfo& info);

// A device of an account, by cache GUID.
struct SignedDevice {
  std::string guid;
  DeviceSignature signature;
};

// The cache GUID in |others| (another profile account's devices) of the
// physical device that is |guid| in |mine|: the one entry with the same
// signature, if exactly one entry of each list has it (two devices alike,
// say two phones of the same model with the default name, aren't told
// apart: empty). Empty if |guid| isn't in |mine| or its name is empty.
std::string FindSameDevice(const std::string& guid,
                           const std::vector<SignedDevice>& mine,
                           const std::vector<SignedDevice>& others);

// The limit in the way, the worst first (slice 7). |throttled|: the engine
// has types throttled by the server; the others from the server's quota
// status.
SyncLimit GetSyncLimit(bool throttled, bool server_full, bool history_full);

// Puts |limit| into a status that is otherwise on (SyncState::kOn): the
// server full or throttling is an error with its own message; history full
// keeps it on and says so after the last-synced line. Other states are left
// alone (their error comes first).
void ApplySyncLimit(SyncLimit limit, SyncStatus& status);

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_SYNC_SETTINGS_H_
