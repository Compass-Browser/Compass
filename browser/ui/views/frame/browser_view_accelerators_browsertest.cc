// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compass's shortcuts and the user's on Windows and Linux
// (docs/design/shortcuts.md, "Windows and Linux"): upstream's accelerator
// table, adjusted (shortcuts_table.h), as the windows register it. macOS's
// tests are in shortcuts_mac_browsertest.mm.

#include <algorithm>
#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "base/test/run_until.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/accelerator_table.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "compass/browser/shortcuts/shortcuts_table.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "compass/browser/ui/views/frame/browser_view_accelerators.h"
#include "compass/browser/ui/webui/settings/compass_settings_ui.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/focus/focus_manager.h"
#include "url/gurl.h"

namespace compass {
namespace {

constexpr int kNoCommand = 0;

ui::Accelerator Ctrl(ui::KeyboardCode key) {
  return ui::Accelerator(key, ui::EF_CONTROL_DOWN);
}
ui::Accelerator CtrlShift(ui::KeyboardCode key) {
  return ui::Accelerator(key, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN);
}

class CompassShortcutsBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    // The windows follow the user's shortcuts once a settings page opened.
    BrowserViewAccelerators::Watch();
  }

  void TearDownOnMainThread() override {
    ShortcutSettings::Get().SetRecording(false);
    ShortcutSettings::Get().ResetAll();
    InProcessBrowserTest::TearDownOnMainThread();
  }

  // The command |accelerator| runs in the window, as when typed: what the
  // table has for it, if the window registered it.
  int CommandForKeys(const ui::Accelerator& accelerator) {
    BrowserView* view = BrowserView::GetBrowserViewForBrowser(browser());
    if (!view->GetFocusManager()->IsAcceleratorRegistered(accelerator)) {
      return kNoCommand;
    }
    for (const AcceleratorMapping& mapping : GetAcceleratorList()) {
      if (ui::Accelerator(mapping.keycode, mapping.modifiers) ==
          accelerator) {
        return mapping.command_id;
      }
    }
    return kNoCommand;
  }

  // Clicks the keys of |id|'s row on the settings page, which then records.
  void StartRecording(content::WebContents* contents, const std::string& id) {
    ASSERT_TRUE(content::ExecJs(
        contents,
        content::JsReplace("document.querySelector($1).click()",
                           "li[data-id=\"" + id + "\"] .keys")));
    ASSERT_TRUE(base::test::RunUntil(
        [] { return ShortcutSettings::Get().recording(); }));
  }

  void PressCtrlShiftK(content::WebContents* contents) {
    content::SimulateKeyPress(contents, ui::DomKey::FromCharacter('K'),
                              ui::DomCode::US_K, ui::VKEY_K,
                              /*control=*/true, /*shift=*/true,
                              /*alt=*/false, /*command=*/false);
  }
};

// Ctrl+Shift+C copies the page's link, Ctrl+S opens or closes the sidebar,
// Ctrl+Shift+M mutes the tab, Ctrl+Shift+E chats with Claude, Shift+Alt+N
// opens a little window and Ctrl+Shift+R reloads without the cache.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, ArcShortcuts) {
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_C)), IDC_COPY_URL);
  EXPECT_EQ(CommandForKeys(Ctrl(ui::VKEY_S)),
            IDC_TOGGLE_VERTICAL_TABS_COLLAPSE);
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_M)), kToggleTabMuteCommand);
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_E)), kToggleClaudeCommand);
  EXPECT_EQ(CommandForKeys(ui::Accelerator(
                ui::VKEY_N, ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN)),
            kNewLittleWindowCommand);
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_R)),
            IDC_RELOAD_BYPASSING_CACHE);
  // What they replace: inspect element keeps Ctrl+Shift+I's developer tools.
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_I)), IDC_DEV_TOOLS);
  // The page lists the defaults.
  EXPECT_EQ(GetDefaultAccelerators(IDC_COPY_URL),
            std::vector<ui::Accelerator>{CtrlShift(ui::VKEY_C)});
}

// Ctrl+Shift+M, pressed in the window, mutes the active tab, and unmutes it.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, MuteTabShortcut) {
  BrowserView* view = BrowserView::GetBrowserViewForBrowser(browser());
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_FALSE(contents->IsAudioMuted());
  EXPECT_TRUE(view->AcceleratorPressed(CtrlShift(ui::VKEY_M)));
  EXPECT_TRUE(contents->IsAudioMuted());
  EXPECT_TRUE(view->AcceleratorPressed(CtrlShift(ui::VKEY_M)));
  EXPECT_FALSE(contents->IsAudioMuted());
}

// The user's shortcuts (docs/design/shortcuts.md "Customizing") take over
// from the defaults in every window, and go away again.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, CustomShortcuts) {
  ShortcutSettings& settings = ShortcutSettings::Get();
  // Copy URL moves from Ctrl+Shift+C to Ctrl+Shift+K.
  settings.SetCustom("copy-url", CtrlShift(ui::VKEY_K));
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_K)), IDC_COPY_URL);
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_C)), kNoCommand);
  // A little window: Ctrl+Shift+L.
  settings.SetCustom("little-window", CtrlShift(ui::VKEY_L));
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_L)), kNewLittleWindowCommand);
  EXPECT_EQ(CommandForKeys(ui::Accelerator(
                ui::VKEY_N, ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN)),
            kNoCommand);
  // None: Reload loses Ctrl+R (and F5).
  settings.SetCustom("reload", ui::Accelerator());
  EXPECT_EQ(CommandForKeys(Ctrl(ui::VKEY_R)), kNoCommand);
  EXPECT_TRUE(GetAccelerators(IDC_RELOAD).empty());
  EXPECT_TRUE(
      std::ranges::contains(GetDefaultAccelerators(IDC_RELOAD), Ctrl(ui::VKEY_R)));

  settings.ResetAll();
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_C)), IDC_COPY_URL);
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_K)), kNoCommand);
  EXPECT_EQ(CommandForKeys(Ctrl(ui::VKEY_R)), IDC_RELOAD);
  EXPECT_EQ(CommandForKeys(ui::Accelerator(
                ui::VKEY_N, ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN)),
            kNewLittleWindowCommand);
}

// While the settings page records keys, the windows hold none, so every key
// reaches the page.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, RecordingHoldsKeysBack) {
  ShortcutSettings::Get().SetRecording(true);
  EXPECT_EQ(CommandForKeys(Ctrl(ui::VKEY_T)), kNoCommand);
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_C)), kNoCommand);
  ShortcutSettings::Get().SetRecording(false);
  EXPECT_EQ(CommandForKeys(Ctrl(ui::VKEY_T)), IDC_NEW_TAB);
}

// The space shortcuts follow the user's keys.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, SpaceShortcuts) {
  views::FocusManager* focus_manager =
      BrowserView::GetBrowserViewForBrowser(browser())->GetFocusManager();
  const ui::Accelerator ctrl_shift_1 = CtrlShift(ui::VKEY_1);
  const ui::Accelerator ctrl_alt_1(ui::VKEY_1,
                                   ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN);
  EXPECT_TRUE(focus_manager->IsAcceleratorRegistered(ctrl_shift_1));
  ShortcutSettings::Get().SetCustom(SpaceShortcutId(0), ctrl_alt_1);
  EXPECT_FALSE(focus_manager->IsAcceleratorRegistered(ctrl_shift_1));
  EXPECT_TRUE(focus_manager->IsAcceleratorRegistered(ctrl_alt_1));
}

// The settings page lists every shortcut macOS's does but undo and redo of
// closing tabs (Ctrl+Z belongs to the page here).
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, ListsBrowserCommands) {
  std::vector<std::string> ids;
  for (const ListedShortcut& shortcut : ListShortcuts()) {
    ids.push_back(shortcut.id);
  }
  for (const char* id : {"new-tab", "open-location", "toggle-sidebar",
                         "copy-url", "little-window", "claude", "next-tab",
                         "mute-tab", "reload", "new-window", "history",
                         "devtools", "view-source"}) {
    EXPECT_TRUE(std::ranges::contains(ids, id)) << id;
  }
  EXPECT_TRUE(std::ranges::contains(ids, SpaceShortcutId(0)));
  EXPECT_FALSE(std::ranges::contains(ids, "undo-close-tab"));
}

// chrome://compass-settings records a shortcut's keys as they're pressed,
// offers to take keys another shortcut has, and refuses a built-in one's.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, SettingsPage) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://compass-settings/")));
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  // Resolves with what |script| returns once it's truthy.
  auto wait_for = [&](const std::string& script) {
    return content::EvalJs(contents, R"(
      new Promise(resolve => {
        const check = () => {
          const value = ()" + script + R"()();
          if (value) {
            resolve(value);
          } else {
            setTimeout(check, 20);
          }
        };
        check();
      }))");
  };
  EXPECT_EQ(wait_for("() => document.querySelector("
                     "'li[data-id=\"copy-url\"] .keys')?.textContent"),
            base::UTF16ToUTF8(CtrlShift(ui::VKEY_C).GetShortcutText()));

  // Click, then Ctrl+Shift+K.
  StartRecording(contents, "copy-url");
  PressCtrlShiftK(contents);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return ShortcutSettings::Get().GetCustom("copy-url") ==
           CtrlShift(ui::VKEY_K);
  }));
  EXPECT_FALSE(ShortcutSettings::Get().recording());
  EXPECT_EQ(CommandForKeys(CtrlShift(ui::VKEY_K)), IDC_COPY_URL);

  // Reload takes Ctrl+Shift+K once the user agrees; Copy URL has none left.
  StartRecording(contents, "reload");
  PressCtrlShiftK(contents);
  EXPECT_EQ(wait_for(R"(() => {
              const button = [...document.querySelectorAll(
                  'li[data-id="reload"] .message button')]
                  .find(b => b.textContent === 'Use here');
              button?.click();
              return !!button;
            })"),
            true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return ShortcutSettings::Get().GetCustom("reload") ==
           CtrlShift(ui::VKEY_K);
  }));
  EXPECT_EQ(ShortcutSettings::Get().GetCustom("copy-url"), ui::Accelerator());

  // Ctrl+W is Close Tab's, which isn't customizable: refused, still
  // recording.
  StartRecording(contents, "history");
  content::SimulateKeyPress(contents, ui::DomKey::FromCharacter('w'),
                            ui::DomCode::US_W, ui::VKEY_W, /*control=*/true,
                            /*shift=*/false, /*alt=*/false,
                            /*command=*/false);
  EXPECT_EQ(wait_for("() => document.querySelector("
                     "'li[data-id=\"history\"] .message.error')"
                     "?.textContent.includes('Already used by')"),
            true);
  EXPECT_FALSE(ShortcutSettings::Get().IsCustomized("history"));
  EXPECT_TRUE(ShortcutSettings::Get().recording());
  // Esc stops.
  content::SimulateKeyPress(contents, ui::DomKey::ESCAPE, ui::DomCode::ESCAPE,
                            ui::VKEY_ESCAPE, /*control=*/false,
                            /*shift=*/false, /*alt=*/false,
                            /*command=*/false);
  ASSERT_TRUE(base::test::RunUntil(
      [] { return !ShortcutSettings::Get().recording(); }));
}

}  // namespace
}  // namespace compass
