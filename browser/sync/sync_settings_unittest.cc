// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/sync_settings.h"

#include <string>

#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "compass/components/sync/pref_names.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "components/sync/test/test_sync_service.h"
#include "components/sync/test/test_sync_user_settings.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::sync {
namespace {

using syncer::UserSelectableType;
using syncer::UserSelectableTypeSet;

class SyncSettingsTest : public testing::Test {
 protected:
  SyncSettingsTest() { RegisterSyncSettingsPrefs(prefs_.registry()); }

  base::test::TaskEnvironment task_environment_;
  TestingPrefServiceSimple prefs_;
};

TEST_F(SyncSettingsTest, NamesRoundTrip) {
  for (SyncDataType type : kAllSyncDataTypes) {
    EXPECT_EQ(SyncDataTypeFromName(SyncDataTypeName(type)), type)
        << SyncDataTypeName(type);
  }
  EXPECT_FALSE(SyncDataTypeFromName("themes"));
  EXPECT_FALSE(SyncDataTypeFromName(""));
}

TEST_F(SyncSettingsTest, EverySwitchButSpacesIsASelectableType) {
  for (SyncDataType type : kAllSyncDataTypes) {
    const std::optional<UserSelectableType> selectable =
        GetSelectableType(type);
    EXPECT_EQ(selectable.has_value(), type != SyncDataType::kSpaces)
        << SyncDataTypeName(type);
    if (selectable) {
      // No switch turns on a type Compass never syncs.
      EXPECT_FALSE(GetUnwantedSelectableTypes().Has(*selectable))
          << SyncDataTypeName(type);
    }
  }
  EXPECT_EQ(GetSelectableType(SyncDataType::kSettings),
            UserSelectableType::kPreferences);
  EXPECT_EQ(GetSelectableType(SyncDataType::kOpenTabs),
            UserSelectableType::kTabs);
}

TEST_F(SyncSettingsTest, AllOnByDefault) {
  EXPECT_TRUE(GetDisabledSyncDataTypes(&prefs_).empty());
}

TEST_F(SyncSettingsTest, SwitchesPersist) {
  SetSyncDataTypeEnabled(&prefs_, SyncDataType::kBookmarks, false);
  SetSyncDataTypeEnabled(&prefs_, SyncDataType::kSpaces, false);
  // Twice is once.
  SetSyncDataTypeEnabled(&prefs_, SyncDataType::kBookmarks, false);
  EXPECT_EQ(GetDisabledSyncDataTypes(&prefs_),
            SyncDataTypeSet({SyncDataType::kBookmarks, SyncDataType::kSpaces}));
  EXPECT_EQ(prefs_.GetList(prefs::kDisabledTypes).size(), 2u);

  SetSyncDataTypeEnabled(&prefs_, SyncDataType::kBookmarks, true);
  EXPECT_EQ(GetDisabledSyncDataTypes(&prefs_),
            SyncDataTypeSet({SyncDataType::kSpaces}));
}

TEST_F(SyncSettingsTest, UnknownNamesInThePrefAreSkipped) {
  base::ListValue list;
  list.Append("history");
  list.Append("a_future_type");
  list.Append(3);
  prefs_.SetList(prefs::kDisabledTypes, std::move(list));
  EXPECT_EQ(GetDisabledSyncDataTypes(&prefs_),
            SyncDataTypeSet({SyncDataType::kHistory}));
}

TEST_F(SyncSettingsTest, SelectedTypesLeaveOutUnwantedAndDisabled) {
  const UserSelectableTypeSet all = UserSelectableTypeSet::All();
  UserSelectableTypeSet selected = ComputeSelectedTypes(all, {});
  EXPECT_FALSE(selected.HasAny(GetUnwantedSelectableTypes()));
  EXPECT_TRUE(selected.Has(UserSelectableType::kHistory));
  EXPECT_TRUE(selected.Has(UserSelectableType::kBookmarks));
  // A selectable type without a switch keeps upstream's default.
  EXPECT_TRUE(selected.Has(UserSelectableType::kCookies));

  selected = ComputeSelectedTypes(
      all, {SyncDataType::kBookmarks, SyncDataType::kOpenTabs,
            SyncDataType::kSpaces});
  EXPECT_FALSE(selected.Has(UserSelectableType::kBookmarks));
  EXPECT_FALSE(selected.Has(UserSelectableType::kTabs));
  EXPECT_TRUE(selected.Has(UserSelectableType::kPasswords));

  // Only registered types.
  EXPECT_EQ(
      ComputeSelectedTypes(
          {UserSelectableType::kBookmarks, UserSelectableType::kThemes}, {}),
      UserSelectableTypeSet({UserSelectableType::kBookmarks}));
}

// The service holds every type until the passphrase is in place, then
// selects what the switches say (CompassSyncService::HoldTypes() and
// SelectTypes()); a switch changed while held waits in the pref.
TEST_F(SyncSettingsTest, HoldThenSelect) {
  syncer::TestSyncService sync_service;
  syncer::TestSyncUserSettings* settings = sync_service.GetUserSettings();

  HoldSelectableTypes(settings);
  EXPECT_FALSE(settings->IsSyncEverythingEnabled());
  EXPECT_TRUE(settings->GetSelectedTypes().empty());

  // The user turns Bookmarks off while held: nothing is selected yet.
  SetSyncDataTypeEnabled(&prefs_, SyncDataType::kBookmarks, false);
  EXPECT_TRUE(settings->GetSelectedTypes().empty());

  SelectSelectableTypes(settings, GetDisabledSyncDataTypes(&prefs_));
  EXPECT_FALSE(settings->IsSyncEverythingEnabled());
  EXPECT_FALSE(
      settings->GetSelectedTypes().Has(UserSelectableType::kBookmarks));
  EXPECT_TRUE(settings->GetSelectedTypes().Has(UserSelectableType::kPasswords));
  EXPECT_FALSE(
      settings->GetSelectedTypes().HasAny(GetUnwantedSelectableTypes()));

  // Back on.
  SetSyncDataTypeEnabled(&prefs_, SyncDataType::kBookmarks, true);
  SelectSelectableTypes(settings, GetDisabledSyncDataTypes(&prefs_));
  EXPECT_TRUE(settings->GetSelectedTypes().Has(UserSelectableType::kBookmarks));

  // The Spaces switch isn't a selectable type: it changes nothing here.
  const UserSelectableTypeSet before = settings->GetSelectedTypes();
  SetSyncDataTypeEnabled(&prefs_, SyncDataType::kSpaces, false);
  SelectSelectableTypes(settings, GetDisabledSyncDataTypes(&prefs_));
  EXPECT_EQ(settings->GetSelectedTypes(), before);
}

TEST_F(SyncSettingsTest, DeviceKinds) {
  using FormFactor = syncer::DeviceInfo::FormFactor;
  using OsType = syncer::DeviceInfo::OsType;
  EXPECT_EQ(GetDeviceKind(OsType::kMac, FormFactor::kDesktop), "Mac");
  EXPECT_EQ(GetDeviceKind(OsType::kWindows, FormFactor::kDesktop),
            "Windows PC");
  EXPECT_EQ(GetDeviceKind(OsType::kIOS, FormFactor::kTablet), "iPad");
  EXPECT_EQ(GetDeviceKind(OsType::kIOS, FormFactor::kPhone), "iPhone");
  EXPECT_EQ(GetDeviceKind(OsType::kAndroid, FormFactor::kPhone),
            "Android phone");
  EXPECT_EQ(GetDeviceKind(OsType::kUnknown, FormFactor::kPhone), "Phone");
  EXPECT_EQ(GetDeviceKind(OsType::kUnknown, FormFactor::kUnknown), "Computer");
}

TEST_F(SyncSettingsTest, MenuLabels) {
  auto label = [](SyncState state) {
    SyncStatus status;
    status.state = state;
    return base::UTF16ToUTF8(GetSyncMenuLabel(status));
  };
  EXPECT_EQ(label(SyncState::kOff), "Sync is off");
  EXPECT_EQ(label(SyncState::kOn), "Synced");
  EXPECT_EQ(label(SyncState::kStarting), "Syncing…");
  EXPECT_EQ(label(SyncState::kError), "Sync error");
  EXPECT_EQ(label(SyncState::kNeedsRestart), "Sync: restart to finish");
}

TEST(SyncSettingsTest, Limits) {
  EXPECT_EQ(SyncLimit::kNone, GetSyncLimit(false, false, false));
  EXPECT_EQ(SyncLimit::kHistoryFull, GetSyncLimit(false, false, true));
  EXPECT_EQ(SyncLimit::kThrottled, GetSyncLimit(true, false, true));
  EXPECT_EQ(SyncLimit::kServerFull, GetSyncLimit(true, true, true));

  const auto on = [] {
    SyncStatus status;
    status.state = SyncState::kOn;
    status.message = "Last synced today.";
    return status;
  };
  const auto label = [](const SyncStatus& status) {
    return base::UTF16ToUTF8(GetSyncMenuLabel(status));
  };

  SyncStatus status = on();
  ApplySyncLimit(SyncLimit::kNone, status);
  EXPECT_EQ(SyncState::kOn, status.state);
  EXPECT_EQ("Last synced today.", status.message);
  EXPECT_EQ("Synced", label(status));

  status = on();
  ApplySyncLimit(SyncLimit::kHistoryFull, status);
  EXPECT_EQ(SyncState::kOn, status.state);
  EXPECT_EQ(
      "Last synced today. History is full on the server: new visits stay on "
      "this device until older ones expire.",
      status.message);
  EXPECT_EQ("Synced (history full)", label(status));

  status = on();
  ApplySyncLimit(SyncLimit::kServerFull, status);
  EXPECT_EQ(SyncState::kError, status.state);
  EXPECT_EQ(SyncLimit::kServerFull, status.limit);
  EXPECT_NE(std::string::npos, status.message.find("server is full"));
  EXPECT_EQ("Sync: server full", label(status));

  status = on();
  ApplySyncLimit(SyncLimit::kThrottled, status);
  EXPECT_EQ(SyncState::kError, status.state);
  EXPECT_EQ("Sync: paused by server", label(status));

  // Another error comes first.
  SyncStatus offline;
  offline.state = SyncState::kError;
  offline.message = "Can't reach the server.";
  ApplySyncLimit(SyncLimit::kServerFull, offline);
  EXPECT_EQ("Can't reach the server.", offline.message);
  EXPECT_EQ(SyncLimit::kNone, offline.limit);
  EXPECT_EQ("Sync error", label(offline));
}

}  // namespace
}  // namespace compass::sync
