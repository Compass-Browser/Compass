// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/tab_switcher/switcher_keys.h"

#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>

namespace compass {

namespace {

// A local monitor sees the app's events before they're sent, so the page
// doesn't get the switcher's keys.
class SwitcherKeysMac : public SwitcherKeys {
 public:
  explicit SwitcherKeysMac(Delegate* delegate) {
    Delegate* target = delegate;
    monitor_ = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged |
                                             NSEventMaskKeyDown
                                     handler:^NSEvent*(NSEvent* event) {
                                       return Handle(target, event);
                                     }];
  }

  ~SwitcherKeysMac() override {
    if (monitor_) {
      [NSEvent removeMonitor:monitor_];
    }
  }

 private:
  static NSEvent* Handle(Delegate* delegate, NSEvent* event) {
    if (event.type == NSEventTypeFlagsChanged) {
      if (!(event.modifierFlags & NSEventModifierFlagControl)) {
        delegate->OnControlReleased();
      }
      return event;
    }
    // W by the character it types, whatever the keyboard's layout.
    if ([event.charactersIgnoringModifiers.lowercaseString
            isEqualToString:@"w"]) {
      delegate->OnClose();
      return nil;
    }
    switch (event.keyCode) {
      case kVK_Escape:
        delegate->OnCancel();
        return nil;
      case kVK_Return:
      case kVK_ANSI_KeypadEnter:
      case kVK_Space:
        delegate->OnCommit();
        return nil;
      case kVK_LeftArrow:
        delegate->OnMove(-1);
        return nil;
      case kVK_RightArrow:
        delegate->OnMove(1);
        return nil;
      default:
        // Ctrl+Tab again goes on to its command, which moves the switcher.
        return event;
    }
  }

  id __strong monitor_ = nil;
};

}  // namespace

// static
std::unique_ptr<SwitcherKeys> SwitcherKeys::Watch(Delegate* delegate) {
  return std::make_unique<SwitcherKeysMac>(delegate);
}

// static
void SwitcherKeys::Track() {}

// static
bool SwitcherKeys::IsControlDown() {
  return NSEvent.modifierFlags & NSEventModifierFlagControl;
}

}  // namespace compass
