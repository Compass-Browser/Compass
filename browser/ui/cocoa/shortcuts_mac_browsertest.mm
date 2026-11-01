// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>

#include <string>

#include "base/strings/utf_string_conversions.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/global_keyboard_shortcuts_mac.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/test/test_clipboard.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_code_conversion_mac.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/animation_test_api.h"
#include "ui/views/focus/focus_manager.h"
#include "url/gurl.h"

#include "compass/browser/shortcuts/shortcut_settings.h"
#include "compass/browser/ui/cocoa/shortcuts_mac.h"
#include "compass/browser/ui/compass_command_ids.h"

namespace compass {
namespace {

// The command the key with |key_code| and |modifiers| runs, as when typed:
// the main menu's key equivalents first, then the shortcuts outside it.
int CommandForKeys(int key_code, NSEventModifierFlags modifiers) {
  unichar shifted_character;
  unichar character;
  ui::MacKeyCodeForWindowsKeyCode(ui::KeyboardCodeFromKeyCode(key_code),
                                  modifiers, &shifted_character, &character);
  NSEvent* event =
      [NSEvent keyEventWithType:NSEventTypeKeyDown
                              location:NSZeroPoint
                         modifierFlags:modifiers
                             timestamp:0.0
                          windowNumber:0
                               context:nil
                            characters:[NSString
                                           stringWithFormat:@"%C", character]
           charactersIgnoringModifiers:[NSString stringWithFormat:@"%C",
                                                                  shifted_character]
                             isARepeat:NO
                               keyCode:key_code];
  return CommandForKeyEvent(event).chrome_command;
}

constexpr NSEventModifierFlags kCommand = NSEventModifierFlagCommand;
constexpr NSEventModifierFlags kCommandShift =
    NSEventModifierFlagCommand | NSEventModifierFlagShift;

class CompassShortcutsBrowserTest : public InProcessBrowserTest {
 protected:
  void TearDownOnMainThread() override {
    ShortcutSettings::Get().SetRecording(false);
    ShortcutSettings::Get().ResetAll();
    InProcessBrowserTest::TearDownOnMainThread();
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

  void PressCommandShiftK(content::WebContents* contents) {
    content::SimulateKeyPress(contents, ui::DomKey::FromCharacter('K'),
                              ui::DomCode::US_K, ui::VKEY_K,
                              /*control=*/false, /*shift=*/true,
                              /*alt=*/false, /*command=*/true);
  }

  // The sidebar collapses as it starts to, without frames (see
  // compass_sidebar_browsertest.cc).
  gfx::AnimationTestApi::RenderModeResetter animation_mode_ =
      gfx::AnimationTestApi::SetRichAnimationRenderMode(
          gfx::Animation::RichAnimationRenderMode::FORCE_DISABLED);
};

// Cmd+Shift+C copies the page's link, Cmd+S opens or closes the sidebar and
// Cmd+Shift+R reloads without the cache.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, ArcShortcuts) {
  EXPECT_EQ(CommandForKeys(kVK_ANSI_C, kCommandShift), IDC_COPY_URL);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_S, kCommand),
            IDC_TOGGLE_VERTICAL_TABS_COLLAPSE);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_R, kCommandShift),
            IDC_RELOAD_BYPASSING_CACHE);
  // What they replace: inspect element keeps Cmd+Option+C; Save Page As and
  // Cmd+Shift+L no longer have a shortcut.
  EXPECT_EQ(CommandForKeys(kVK_ANSI_C, kCommand | NSEventModifierFlagOption),
            IDC_DEV_TOOLS_INSPECT);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_L, kCommandShift), NO_COMMAND);
  // Cmd+Option+N, upstream's new split tab, opens a little window.
  EXPECT_EQ(CommandForKeys(kVK_ANSI_N, kCommand | NSEventModifierFlagOption),
            kNewLittleWindowCommand);
}

// Cmd+Shift+M mutes the active tab, and unmutes it. It was upstream's
// profile menu, which no longer has a shortcut.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, MuteTabShortcut) {
  EXPECT_EQ(CommandForKeys(kVK_ANSI_M, kCommandShift), kToggleTabMuteCommand);
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_FALSE(contents->IsAudioMuted());
  EXPECT_TRUE(chrome::ExecuteCommand(browser(), kToggleTabMuteCommand));
  EXPECT_TRUE(contents->IsAudioMuted());
  EXPECT_TRUE(chrome::ExecuteCommand(browser(), kToggleTabMuteCommand));
  EXPECT_FALSE(contents->IsAudioMuted());
}

IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, CopyLinkCopiesThePageURL) {
  // Not the user's own clipboard.
  ui::TestClipboard* clipboard = ui::TestClipboard::CreateForCurrentThread();
  const GURL url("data:text/html,<p>Hello");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  chrome::ExecuteCommand(browser(), IDC_COPY_URL);
  base::test::TestFuture<std::u16string> text;
  clipboard->ReadText(ui::ClipboardBuffer::kCopyPaste,
                      /*data_dst=*/std::nullopt, text.GetCallback());
  EXPECT_EQ(text.Get(), base::UTF8ToUTF16(url.spec()));
  ui::Clipboard::DestroyClipboardForCurrentThread();
}

IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, ToggleSidebar) {
  auto* controller = tabs::VerticalTabStripStateController::From(browser());
  ASSERT_TRUE(controller);
  const bool collapsed = controller->IsCollapsed();
  chrome::ExecuteCommand(browser(), IDC_TOGGLE_VERTICAL_TABS_COLLAPSE);
  EXPECT_NE(controller->IsCollapsed(), collapsed);
  chrome::ExecuteCommand(browser(), IDC_TOGGLE_VERTICAL_TABS_COLLAPSE);
  EXPECT_EQ(controller->IsCollapsed(), collapsed);
}

// The user's shortcuts (docs/design/shortcuts.md "Customizing") take over
// from the defaults, in the main menu and outside it, and go away again.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, CustomShortcuts) {
  // A page that can be saved: Save Page As is disabled on about:blank, and a
  // disabled menu item's keys run nothing.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<p>Hello")));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return chrome::IsCommandEnabled(browser(), IDC_SAVE_PAGE); }));
  ShortcutSettings& settings = ShortcutSettings::Get();
  const ui::Accelerator cmd_shift_k(ui::VKEY_K,
                                    ui::EF_COMMAND_DOWN | ui::EF_SHIFT_DOWN);
  const ui::Accelerator cmd_shift_l(ui::VKEY_L,
                                    ui::EF_COMMAND_DOWN | ui::EF_SHIFT_DOWN);

  // Outside the menu: Copy URL moves from Cmd+Shift+C to Cmd+Shift+K.
  settings.SetCustom("copy-url", cmd_shift_k);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_K, kCommandShift), IDC_COPY_URL);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_C, kCommandShift), NO_COMMAND);
  // In the menu: the sidebar toggle moves back to Cmd+Shift+L, and upstream's
  // menus show it.
  settings.SetCustom("toggle-sidebar", cmd_shift_l);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_L, kCommandShift),
            IDC_TOGGLE_VERTICAL_TABS_COLLAPSE);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_S, kCommand), NO_COMMAND);
  ui::Accelerator shown;
  EXPECT_TRUE(GetDefaultMacAcceleratorForCommandId(
      IDC_TOGGLE_VERTICAL_TABS_COLLAPSE, &shown));
  EXPECT_EQ(shown, cmd_shift_l);
  // Save Page As, which has no shortcut, gets Cmd+S.
  settings.SetCustom("save-page",
                     ui::Accelerator(ui::VKEY_S, ui::EF_COMMAND_DOWN));
  EXPECT_EQ(CommandForKeys(kVK_ANSI_S, kCommand), IDC_SAVE_PAGE);
  // None: Reload loses Cmd+R.
  settings.SetCustom("reload", ui::Accelerator());
  EXPECT_EQ(CommandForKeys(kVK_ANSI_R, kCommand), NO_COMMAND);
  EXPECT_FALSE(GetDefaultMacAcceleratorForCommandId(IDC_RELOAD, &shown));
  EXPECT_TRUE(GetAccelerators(IDC_RELOAD).empty());
  EXPECT_EQ(GetDefaultAccelerators(IDC_RELOAD),
            std::vector<ui::Accelerator>{
                ui::Accelerator(ui::VKEY_R, ui::EF_COMMAND_DOWN)});

  settings.ResetAll();
  EXPECT_EQ(CommandForKeys(kVK_ANSI_C, kCommandShift), IDC_COPY_URL);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_K, kCommandShift), NO_COMMAND);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_S, kCommand),
            IDC_TOGGLE_VERTICAL_TABS_COLLAPSE);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_L, kCommandShift), NO_COMMAND);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_R, kCommand), IDC_RELOAD);
}

// While the settings page records keys, none of them runs a command.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, RecordingHoldsKeysBack) {
  ShortcutSettings::Get().SetRecording(true);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_T, kCommand), NO_COMMAND);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_C, kCommandShift), NO_COMMAND);
  ShortcutSettings::Get().SetRecording(false);
  EXPECT_EQ(CommandForKeys(kVK_ANSI_T, kCommand), IDC_NEW_TAB);
}

// The space shortcuts follow the user's keys.
IN_PROC_BROWSER_TEST_F(CompassShortcutsBrowserTest, SpaceShortcuts) {
  views::FocusManager* focus_manager =
      BrowserView::GetBrowserViewForBrowser(browser())->GetFocusManager();
  const ui::Accelerator ctrl_1(ui::VKEY_1, ui::EF_CONTROL_DOWN);
  const ui::Accelerator ctrl_option_1(ui::VKEY_1,
                                      ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN);
  EXPECT_TRUE(focus_manager->IsAcceleratorRegistered(ctrl_1));
  ShortcutSettings::Get().SetCustom(SpaceShortcutId(0), ctrl_option_1);
  EXPECT_FALSE(focus_manager->IsAcceleratorRegistered(ctrl_1));
  EXPECT_TRUE(focus_manager->IsAcceleratorRegistered(ctrl_option_1));
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
  const ui::Accelerator copy_url_default(
      ui::VKEY_C, ui::EF_COMMAND_DOWN | ui::EF_SHIFT_DOWN);
  EXPECT_EQ(wait_for("() => document.querySelector("
                     "'li[data-id=\"copy-url\"] .keys')?.textContent"),
            base::UTF16ToUTF8(copy_url_default.GetShortcutText()));

  // Click, then Cmd+Shift+K.
  const ui::Accelerator cmd_shift_k(ui::VKEY_K,
                                    ui::EF_COMMAND_DOWN | ui::EF_SHIFT_DOWN);
  StartRecording(contents, "copy-url");
  PressCommandShiftK(contents);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return ShortcutSettings::Get().GetCustom("copy-url") == cmd_shift_k;
  }));
  EXPECT_FALSE(ShortcutSettings::Get().recording());

  // Reload takes Cmd+Shift+K once the user agrees; Copy URL has none left.
  StartRecording(contents, "reload");
  PressCommandShiftK(contents);
  EXPECT_EQ(wait_for(R"(() => {
              const button = [...document.querySelectorAll(
                  'li[data-id="reload"] .message button')]
                  .find(b => b.textContent === 'Use here');
              button?.click();
              return !!button;
            })"),
            true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return ShortcutSettings::Get().GetCustom("reload") == cmd_shift_k;
  }));
  EXPECT_EQ(ShortcutSettings::Get().GetCustom("copy-url"), ui::Accelerator());

  // Cmd+Q is Quit's, which isn't customizable: refused, still recording.
  StartRecording(contents, "history");
  content::SimulateKeyPress(contents, ui::DomKey::FromCharacter('q'),
                            ui::DomCode::US_Q, ui::VKEY_Q, /*control=*/false,
                            /*shift=*/false, /*alt=*/false, /*command=*/true);
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
