// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/little_window/little_window_hot_key.h"

#include <vector>

#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "compass/browser/ui/views/little_window/little_window.h"
#include "ui/events/keycodes/keyboard_codes.h"

#if BUILDFLAG(IS_MAC)
#include "compass/browser/ui/cocoa/shortcuts_mac.h"
#elif BUILDFLAG(IS_WIN)
#include "compass/browser/shortcuts/shortcuts_win.h"
#endif

namespace compass {

// static
LittleWindowHotKey& LittleWindowHotKey::Get() {
  static base::NoDestructor<LittleWindowHotKey> instance;
  return *instance;
}

LittleWindowHotKey::LittleWindowHotKey() = default;
LittleWindowHotKey::~LittleWindowHotKey() = default;

void LittleWindowHotKey::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  settings_observation_.Observe(&ShortcutSettings::Get());
  Update();
}

// static
ui::Accelerator LittleWindowHotKey::GetAccelerator() {
  const std::vector<ui::Accelerator> accelerators =
      GetAccelerators(kNewLittleWindowCommand);
  return accelerators.empty() ? ui::Accelerator() : accelerators.front();
}

void LittleWindowHotKey::Update() {
  ui::GlobalAcceleratorListener* listener =
      ui::GlobalAcceleratorListener::GetInstance();
  if (!listener) {
    return;
  }
  const ui::Accelerator accelerator = GetAccelerator();
  bool usable = accelerator.key_code() != ui::VKEY_UNKNOWN;
#if BUILDFLAG(IS_WIN)
  // Windows' hot keys here take Shift, Ctrl and Alt only: a shortcut with
  // the Windows key would be registered without it.
  usable = usable && !accelerator.IsCmdDown();
#endif
  // Not while the settings page records keys: the hot key would take them.
  const bool listen =
      started_ && usable && !ShortcutSettings::Get().recording();
  if (listening_ && (!listen || accelerator != listening_accelerator_)) {
    listener->UnregisterAccelerator(listening_accelerator_, this);
    listening_ = false;
  }
  if (listen && !listening_) {
    // False if another app holds the keys: then only Compass's own windows
    // have the shortcut, as before.
    listening_ = listener->RegisterAccelerator(accelerator, this);
    listening_accelerator_ = accelerator;
  }
}

void LittleWindowHotKey::Open() {
  BrowserWindowInterface* last = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        last = browser;
        return false;
      });
  if (last && last->IsActive()) {
    OpenNewLittleWindow(last);
    return;
  }
  Profile* profile = last ? last->GetProfile()
                          : ProfileManager::GetLastUsedProfileAllowedByPolicy();
  if (profile) {
    OpenNewLittleWindowOverOtherApp(profile);
  }
}

void LittleWindowHotKey::OnKeyPressed(const ui::Accelerator& accelerator) {
  if (listening_ && accelerator == listening_accelerator_) {
    Open();
  }
}

void LittleWindowHotKey::OnShortcutsChanged() {
  Update();
}

void LittleWindowHotKey::OnRecordingChanged() {
  Update();
}

}  // namespace compass
