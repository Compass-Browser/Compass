// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "compass/browser/ui/cocoa/spaces_menu_mac.h"

#import <objc/runtime.h>

#include <vector>

#include "base/strings/sys_string_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/window_features.h"
#include "ui/base/accelerators/accelerator.h"
#import "ui/base/accelerators/platform_accelerator_cocoa.h"

namespace {

// The window the menu acts on: the last active one, unless it's the parking
// window or between spaces.
BrowserWindowInterface* TargetBrowser() {
  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  compass::WindowFeatures* features =
      browser ? compass::WindowFeatures::From(browser) : nullptr;
  if (!features || features->is_parking_window() || features->is_switching()) {
    return nullptr;
  }
  return browser;
}

char kControllerKey;

}  // namespace

// Fills the menu as it opens and runs its items.
@interface CompassSpacesMenuController : NSObject <NSMenuDelegate>
@end

@implementation CompassSpacesMenuController

- (void)menuNeedsUpdate:(NSMenu*)menu {
  [menu removeAllItems];
  BrowserWindowInterface* browser = TargetBrowser();
  if (browser) {
    compass::SpaceModel& shown =
        compass::WindowFeatures::From(browser)->space_model();
    const std::vector<compass::SpaceDirectory::Entry> spaces =
        compass::GetSwitchableSpaces(browser);
    for (size_t i = 0; i < spaces.size(); ++i) {
      compass::SpaceModel& space = *spaces[i].space;
      NSMenuItem* item =
          [[NSMenuItem alloc] initWithTitle:base::SysUTF16ToNSString(space.name())
                                     action:@selector(switchToSpace:)
                              keyEquivalent:@""];
      item.target = self;
      item.representedObject =
          base::SysUTF8ToNSString(space.id().AsLowercaseString());
      item.state =
          &space == &shown ? NSControlStateValueOn : NSControlStateValueOff;
      // The space's shortcut, the user's if they changed it.
      if (i < static_cast<size_t>(compass::kSpaceShortcutCount)) {
        const std::vector<ui::Accelerator> keys =
            compass::GetSpaceAccelerators(static_cast<int>(i));
        if (!keys.empty()) {
          KeyEquivalentAndModifierMask* equivalent =
              ui::GetKeyEquivalentAndModifierMaskFromAccelerator(keys.front());
          item.keyEquivalent = equivalent.keyEquivalent;
          item.keyEquivalentModifierMask = equivalent.modifierMask;
        }
      }
      [menu addItem:item];
    }
    [menu addItem:[NSMenuItem separatorItem]];
  }
  // Without a window to act on, New Space is there but greyed out.
  NSMenuItem* add =
      [[NSMenuItem alloc] initWithTitle:@"New Space"
                                 action:browser ? @selector(newSpace:) : nil
                          keyEquivalent:@""];
  add.target = self;
  [menu addItem:add];
  // One that stays on this Mac (docs/design/sync.md, "Spaces that don't
  // sync").
  NSMenuItem* add_local = [[NSMenuItem alloc]
      initWithTitle:@"New Space on This Device"
             action:browser ? @selector(newLocalSpace:) : nil
      keyEquivalent:@""];
  add_local.target = self;
  [menu addItem:add_local];
}

// The menu shows the space shortcuts but doesn't take them: the window's
// focus manager runs them (SpaceShortcuts), after the page had them.
- (BOOL)menuHasKeyEquivalent:(NSMenu*)menu
                    forEvent:(NSEvent*)event
                      target:(id*)target
                      action:(SEL*)action {
  return NO;
}

- (void)switchToSpace:(NSMenuItem*)sender {
  BrowserWindowInterface* browser = TargetBrowser();
  if (!browser) {
    return;
  }
  const compass::SpaceDirectory::Entry entry = compass::FindSwitchableSpace(
      browser, base::Uuid::ParseLowercase(
                   base::SysNSStringToUTF8(sender.representedObject)));
  if (entry.space) {
    compass::SwitchWindowToSpace(browser, entry.profile, *entry.space);
  }
}

- (void)newSpace:(id)sender {
  if (BrowserWindowInterface* browser = TargetBrowser()) {
    compass::WindowFeatures::From(browser)->AddSpaceAndSwitch();
  }
}

- (void)newLocalSpace:(id)sender {
  if (BrowserWindowInterface* browser = TargetBrowser()) {
    compass::WindowFeatures::From(browser)->AddSpaceAndSwitch(
        /*synced=*/false);
  }
}

@end

namespace compass {

NSMenuItem* BuildSpacesMenu(NSApplication* nsapp,
                            id app_delegate,
                            const std::u16string& product_name,
                            bool is_pwa,
                            bool is_rtl) {
  if (is_pwa) {
    return nil;
  }
  NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Spaces"];
  CompassSpacesMenuController* controller =
      [[CompassSpacesMenuController alloc] init];
  menu.delegate = controller;
  // The delegate is weak; the menu keeps it.
  objc_setAssociatedObject(menu, &kControllerKey, controller,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:@"Spaces"
                                                action:nil
                                         keyEquivalent:@""];
  item.tag = kSpacesMenuTag;
  item.submenu = menu;
  return item;
}

}  // namespace compass
