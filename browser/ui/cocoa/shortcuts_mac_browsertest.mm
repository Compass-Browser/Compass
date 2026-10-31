// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>

#include <string>

#include "base/strings/utf_string_conversions.h"
#include "base/test/test_future.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/global_keyboard_shortcuts_mac.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/test/test_clipboard.h"
#include "ui/events/keycodes/keyboard_code_conversion_mac.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/animation_test_api.h"
#include "url/gurl.h"

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

}  // namespace
}  // namespace compass
