// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/shortcuts/shortcuts_table.h"

#include <optional>
#include <vector>

#include "chrome/app/chrome_command_ids.h"
#include "components/prefs/testing_pref_service.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace compass {
namespace {

ui::Accelerator Ctrl(ui::KeyboardCode key) {
  return ui::Accelerator(key, ui::EF_CONTROL_DOWN);
}
ui::Accelerator CtrlShift(ui::KeyboardCode key) {
  return ui::Accelerator(key, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN);
}

// A few of upstream's entries, as GetAcceleratorList() has them.
std::vector<CommandAccelerator> UpstreamTable() {
  return {
      {CtrlShift(ui::VKEY_C), IDC_DEV_TOOLS_INSPECT},
      {CtrlShift(ui::VKEY_I), IDC_DEV_TOOLS},
      {Ctrl(ui::VKEY_S), IDC_SAVE_PAGE},
      {CtrlShift(ui::VKEY_M), IDC_SHOW_AVATAR_MENU},
      {Ctrl(ui::VKEY_R), IDC_RELOAD},
      {ui::Accelerator(ui::VKEY_F5, ui::EF_NONE), IDC_RELOAD},
      {Ctrl(ui::VKEY_W), IDC_CLOSE_TAB},
      {ui::Accelerator(ui::VKEY_N, ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN),
       kNewLittleWindowCommand},
  };
}

// The command |list| runs on |accelerator|, 0 if none.
int CommandFor(const std::vector<CommandAccelerator>& list,
               const ui::Accelerator& accelerator) {
  for (const CommandAccelerator& entry : list) {
    if (entry.accelerator == accelerator) {
      return entry.command;
    }
  }
  return 0;
}

class ShortcutsTableTest : public testing::Test {
 protected:
  void SetUp() override {
    RegisterShortcutPrefs(prefs_.registry());
    ShortcutSettings::Get().SetPrefServiceForTesting(&prefs_);
  }
  void TearDown() override {
    ShortcutSettings::Get().SetPrefServiceForTesting(nullptr);
  }

  TestingPrefServiceSimple prefs_;
};

// Compass's keys go over upstream's, the user's over those; the first
// table is the defaults the settings page shows. One test: the defaults
// are taken once per process.
TEST_F(ShortcutsTableTest, CompassKeysThenTheUsers) {
  std::vector<CommandAccelerator> list = UpstreamTable();
  AdjustAcceleratorList(list);
  EXPECT_EQ(CommandFor(list, CtrlShift(ui::VKEY_C)), IDC_COPY_URL);
  EXPECT_EQ(CommandFor(list, CtrlShift(ui::VKEY_I)), IDC_DEV_TOOLS);
  EXPECT_EQ(CommandFor(list, Ctrl(ui::VKEY_S)),
            IDC_TOGGLE_VERTICAL_TABS_COLLAPSE);
  EXPECT_EQ(CommandFor(list, CtrlShift(ui::VKEY_M)), kToggleTabMuteCommand);
  EXPECT_EQ(CommandFor(list, CtrlShift(ui::VKEY_E)), kToggleClaudeCommand);
  EXPECT_EQ(CommandFor(list, Ctrl(ui::VKEY_R)), IDC_RELOAD);
  EXPECT_EQ(GetDefaultAccelerators(IDC_COPY_URL),
            std::vector<ui::Accelerator>{CtrlShift(ui::VKEY_C)});
  EXPECT_EQ(GetDefaultAccelerators(IDC_RELOAD),
            (std::vector<ui::Accelerator>{
                Ctrl(ui::VKEY_R), ui::Accelerator(ui::VKEY_F5, ui::EF_NONE)}));
  // Close Tab isn't listed: the page refuses its keys.
  EXPECT_TRUE(FindBuiltInShortcut(Ctrl(ui::VKEY_W)).has_value());
  EXPECT_FALSE(FindBuiltInShortcut(Ctrl(ui::VKEY_R)).has_value());

  // The user's: Reload on Ctrl+Shift+K alone, no little window shortcut.
  ShortcutSettings::Get().SetCustom("reload", CtrlShift(ui::VKEY_K));
  ShortcutSettings::Get().SetCustom("little-window", ui::Accelerator());
  list = UpstreamTable();
  AdjustAcceleratorList(list);
  EXPECT_EQ(CommandFor(list, CtrlShift(ui::VKEY_K)), IDC_RELOAD);
  EXPECT_EQ(CommandFor(list, Ctrl(ui::VKEY_R)), 0);
  EXPECT_EQ(CommandFor(list, ui::Accelerator(ui::VKEY_F5, ui::EF_NONE)), 0);
  EXPECT_EQ(CommandFor(list, ui::Accelerator(ui::VKEY_N, ui::EF_SHIFT_DOWN |
                                                             ui::EF_ALT_DOWN)),
            0);
  EXPECT_EQ(GetAccelerators(IDC_RELOAD),
            std::vector<ui::Accelerator>{CtrlShift(ui::VKEY_K)});
  EXPECT_TRUE(GetAccelerators(kNewLittleWindowCommand).empty());
  // The defaults stay.
  EXPECT_EQ(GetDefaultAccelerators(IDC_RELOAD).size(), 2u);

  ShortcutSettings::Get().ResetAll();
  list = UpstreamTable();
  AdjustAcceleratorList(list);
  EXPECT_EQ(CommandFor(list, Ctrl(ui::VKEY_R)), IDC_RELOAD);
  EXPECT_EQ(CommandFor(list, CtrlShift(ui::VKEY_K)), 0);
}

// Every customizable command has an id of its own.
TEST_F(ShortcutsTableTest, CommandsAreUnique) {
  for (const CustomizableCommand& command : GetCustomizableCommands()) {
    EXPECT_EQ(FindCustomizableCommand(command.id), &command) << command.id;
  }
}

}  // namespace
}  // namespace compass
