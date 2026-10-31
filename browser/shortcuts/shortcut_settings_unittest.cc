// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/shortcuts/shortcut_settings.h"

#include "base/scoped_observation.h"
#include "base/values.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace compass {
namespace {

class CountingObserver : public ShortcutSettings::Observer {
 public:
  void OnShortcutsChanged() override { ++changes; }
  void OnRecordingChanged() override { ++recording_changes; }

  int changes = 0;
  int recording_changes = 0;
};

class ShortcutSettingsTest : public testing::Test {
 protected:
  void SetUp() override {
    RegisterShortcutPrefs(prefs_.registry());
    ShortcutSettings::Get().SetPrefServiceForTesting(&prefs_);
    observation_.Observe(&ShortcutSettings::Get());
  }

  void TearDown() override {
    observation_.Reset();
    ShortcutSettings::Get().SetPrefServiceForTesting(nullptr);
  }

  ShortcutSettings& settings() { return ShortcutSettings::Get(); }

  TestingPrefServiceSimple prefs_;
  CountingObserver observer_;
  base::ScopedObservation<ShortcutSettings, ShortcutSettings::Observer>
      observation_{&observer_};
};

ui::Accelerator CmdShiftK() {
  return ui::Accelerator(ui::VKEY_K, ui::EF_COMMAND_DOWN | ui::EF_SHIFT_DOWN);
}

TEST_F(ShortcutSettingsTest, DefaultsUntilChanged) {
  EXPECT_FALSE(settings().IsCustomized(SpaceShortcutId(0)));
  EXPECT_EQ(GetSpaceAccelerators(0),
            std::vector<ui::Accelerator>{GetDefaultSpaceAccelerator(0)});
  EXPECT_EQ(GetPipClickThroughAccelerators(),
            std::vector<ui::Accelerator>{
                GetDefaultPipClickThroughAccelerator()});
}

TEST_F(ShortcutSettingsTest, SetSavesAndNotifies) {
  settings().SetCustom("copy-url", CmdShiftK());
  EXPECT_EQ(observer_.changes, 1);
  EXPECT_EQ(settings().GetCustom("copy-url"), CmdShiftK());
  const base::Value::Dict& saved = prefs_.GetDict(kShortcutsPref);
  ASSERT_TRUE(saved.FindDict("copy-url"));
  EXPECT_EQ(saved.FindDict("copy-url")->FindInt("key"), ui::VKEY_K);
  EXPECT_EQ(saved.FindDict("copy-url")->FindInt("modifiers"),
            ui::EF_COMMAND_DOWN | ui::EF_SHIFT_DOWN);
}

TEST_F(ShortcutSettingsTest, NoneIsAnEmptyEntry) {
  settings().SetCustom(SpaceShortcutId(2), ui::Accelerator());
  EXPECT_TRUE(settings().IsCustomized(SpaceShortcutId(2)));
  EXPECT_TRUE(GetSpaceAccelerators(2).empty());
  const base::Value::Dict* saved =
      prefs_.GetDict(kShortcutsPref).FindDict(SpaceShortcutId(2));
  ASSERT_TRUE(saved);
  EXPECT_TRUE(saved->empty());
}

TEST_F(ShortcutSettingsTest, ResetGoesBackToTheDefault) {
  settings().SetCustom(SpaceShortcutId(0), CmdShiftK());
  EXPECT_EQ(GetSpaceAccelerators(0), std::vector<ui::Accelerator>{CmdShiftK()});
  settings().SetCustom(SpaceShortcutId(0), std::nullopt);
  EXPECT_FALSE(settings().IsCustomized(SpaceShortcutId(0)));
  EXPECT_EQ(GetSpaceAccelerators(0),
            std::vector<ui::Accelerator>{GetDefaultSpaceAccelerator(0)});
  EXPECT_TRUE(prefs_.GetDict(kShortcutsPref).empty());

  settings().SetCustom("a", CmdShiftK());
  settings().SetCustom("b", ui::Accelerator());
  settings().ResetAll();
  EXPECT_FALSE(settings().IsCustomized("a"));
  EXPECT_FALSE(settings().IsCustomized("b"));
  EXPECT_TRUE(prefs_.GetDict(kShortcutsPref).empty());
}

// What the pref held at startup.
TEST_F(ShortcutSettingsTest, ReadsThePref) {
  base::Value::Dict dict;
  dict.Set("copy-url", AcceleratorToValue(CmdShiftK()));
  // Not a shortcut: no modifier. Ignored.
  dict.Set("reload", AcceleratorToValue(ui::Accelerator(ui::VKEY_R, 0)));
  prefs_.SetDict(kShortcutsPref, std::move(dict));
  settings().SetPrefServiceForTesting(&prefs_);
  EXPECT_EQ(observer_.changes, 1);
  EXPECT_EQ(settings().GetCustom("copy-url"), CmdShiftK());
  EXPECT_FALSE(settings().IsCustomized("reload"));
}

TEST_F(ShortcutSettingsTest, Recording) {
  EXPECT_FALSE(settings().recording());
  settings().SetRecording(true);
  settings().SetRecording(true);
  EXPECT_TRUE(settings().recording());
  EXPECT_EQ(observer_.recording_changes, 1);
  settings().SetRecording(false);
  EXPECT_EQ(observer_.recording_changes, 2);
}

TEST(ShortcutValidityTest, NeedsCommandControlOrOption) {
  EXPECT_TRUE(IsValidShortcut(CmdShiftK()));
  EXPECT_TRUE(IsValidShortcut(ui::Accelerator(ui::VKEY_1, ui::EF_CONTROL_DOWN)));
  EXPECT_TRUE(IsValidShortcut(ui::Accelerator(ui::VKEY_F5, ui::EF_ALT_DOWN)));
  EXPECT_FALSE(IsValidShortcut(ui::Accelerator(ui::VKEY_K, 0)));
  EXPECT_FALSE(IsValidShortcut(ui::Accelerator(ui::VKEY_K, ui::EF_SHIFT_DOWN)));
  EXPECT_FALSE(IsValidShortcut(
      ui::Accelerator(ui::VKEY_SHIFT, ui::EF_COMMAND_DOWN | ui::EF_SHIFT_DOWN)));
  EXPECT_FALSE(IsValidShortcut(ui::Accelerator()));
}

TEST(ShortcutValueTest, RoundTrips) {
  EXPECT_EQ(AcceleratorFromValue(AcceleratorToValue(CmdShiftK())), CmdShiftK());
  EXPECT_EQ(AcceleratorFromValue(AcceleratorToValue(ui::Accelerator())),
            ui::Accelerator());
  base::Value::Dict broken;
  broken.Set("key", "K");
  EXPECT_EQ(AcceleratorFromValue(broken), std::nullopt);
}

}  // namespace
}  // namespace compass
