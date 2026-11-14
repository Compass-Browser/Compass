// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_HOT_KEY_H_
#define COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_HOT_KEY_H_

#include <string>

#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/accelerators/global_accelerator_listener/global_accelerator_listener.h"

namespace compass {

// The new little window shortcut (Cmd+Option+N, Shift+Alt+N on Windows and
// Linux, or the user's) from any app: a system hot key through upstream's
// GlobalAcceleratorListener (a Carbon hot key on macOS, RegisterHotKey() on
// Windows, a key grab on Linux's X11; Wayland has none), as
// picture-in-picture's click-through. The hot key takes the keys while
// Compass is in front too, so it opens the little window there as the
// window's shortcut did. docs/design/little-window.md, "From any app".
class LittleWindowHotKey : public ui::GlobalAcceleratorListener::Observer,
                           public ShortcutSettings::Observer {
 public:
  static LittleWindowHotKey& Get();

  LittleWindowHotKey(const LittleWindowHotKey&) = delete;
  LittleWindowHotKey& operator=(const LittleWindowHotKey&) = delete;

  // Listens from now on, for as long as Compass runs. Called as Compass's
  // first window is built (CompassSidebarView).
  void Start();

  bool listening() const { return listening_; }
  const ui::Accelerator& listening_accelerator() const {
    return listening_accelerator_;
  }

  // What the keys do: with a Compass window active, a little window from it
  // (OpenNewLittleWindow()); otherwise one of the last active window's
  // profile (else the last used profile) over the app in front
  // (OpenNewLittleWindowOverOtherApp()).
  void Open();

  // ui::GlobalAcceleratorListener::Observer:
  void OnKeyPressed(const ui::Accelerator& accelerator) override;
  void ExecuteCommand(const std::string& accelerator_group_id,
                      const std::string& command_id) override {}

  // ShortcutSettings::Observer:
  void OnShortcutsChanged() override;
  void OnRecordingChanged() override;

 private:
  friend class base::NoDestructor<LittleWindowHotKey>;
  LittleWindowHotKey();
  ~LittleWindowHotKey() override;

  // The shortcut in force; ui::Accelerator() if the user removed it.
  static ui::Accelerator GetAccelerator();
  // Registers or drops the hot key to match the shortcut and the settings
  // page's recording.
  void Update();

  bool started_ = false;
  bool listening_ = false;
  ui::Accelerator listening_accelerator_;
  base::ScopedObservation<ShortcutSettings, ShortcutSettings::Observer>
      settings_observation_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_HOT_KEY_H_
