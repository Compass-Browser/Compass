// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_SWITCHER_KEYS_H_
#define COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_SWITCHER_KEYS_H_

#include <memory>

namespace compass {

// The keys around the recent tabs switcher (tab_switcher.h), per platform:
// the switcher stays up while Ctrl is held, whoever has the keyboard focus
// (often the page, whose key events the browser's views never see), so it
// watches the application's events directly: an NSEvent monitor on macOS,
// an aura::Env handler elsewhere.
class SwitcherKeys {
 public:
  class Delegate {
   public:
    virtual void OnControlReleased() = 0;
    // Esc: back to where you were.
    virtual void OnCancel() = 0;
    // Return, or Space: switch now.
    virtual void OnCommit() = 0;
    // The arrow keys: -1 for left, 1 for right.
    virtual void OnMove(int step) = 0;

   protected:
    virtual ~Delegate() = default;
  };

  // Watches for the switcher's keys until destroyed; they go to |delegate|
  // and nowhere else.
  static std::unique_ptr<SwitcherKeys> Watch(Delegate* delegate);

  // Starts noting whether Ctrl is held, where the platform can't say
  // (aura). Once per process is enough.
  static void Track();
  // Whether Ctrl is held right now.
  static bool IsControlDown();

  virtual ~SwitcherKeys() = default;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_SWITCHER_KEYS_H_
