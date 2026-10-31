// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SHORTCUTS_SHORTCUT_SETTINGS_H_
#define COMPASS_BROWSER_SHORTCUTS_SHORTCUT_SETTINGS_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/values.h"
#include "ui/base/accelerators/accelerator.h"

class PrefRegistrySimple;
class PrefService;

namespace compass {

// The shortcuts the user changed on chrome://compass-settings
// (docs/design/shortcuts.md "Customizing"), browser-wide, in Local State:
// {"<id>": {"key": <ui::KeyboardCode>, "modifiers": <ui::EF_* flags>}}, and
// {} for "no shortcut". Shortcuts that aren't in it keep their defaults.
inline constexpr char kShortcutsPref[] = "compass.shortcuts";

// Called from RegisterLocalState() (see patches/).
void RegisterShortcutPrefs(PrefRegistrySimple* registry);

// Ids of the customizable shortcuts that aren't browser commands (those are
// listed by the platform, shortcuts_mac.h).
// Ctrl+1–9 (Ctrl+Shift+1–9 off macOS): switch to the nth space.
inline constexpr int kSpaceShortcutCount = 9;
std::string SpaceShortcutId(int index);
// Ctrl+Option+Cmd+P: lets clicks through picture-in-picture windows, from any
// app (docs/design/picture-in-picture.md "Click-through").
inline constexpr char kPipClickThroughShortcutId[] = "pip-click-through";

ui::Accelerator GetDefaultSpaceAccelerator(int index);
ui::Accelerator GetDefaultPipClickThroughAccelerator();

// The accelerators in force for the nth space and for click-through: the
// user's if they changed it (none if they removed it), else the default.
std::vector<ui::Accelerator> GetSpaceAccelerators(int index);
std::vector<ui::Accelerator> GetPipClickThroughAccelerators();

// Whether the keys can be a shortcut: a key with Command, Control or Option
// (on macOS only those go through the menus' key equivalents), and not a
// modifier alone.
bool IsValidShortcut(const ui::Accelerator& accelerator);

// Pref encoding. A ui::Accelerator() (VKEY_UNKNOWN) is "no shortcut".
base::Value::Dict AcceleratorToValue(const ui::Accelerator& accelerator);
std::optional<ui::Accelerator> AcceleratorFromValue(
    const base::Value::Dict& value);

// The user's shortcuts, for every part of the browser that has shortcuts to
// follow. Read from kShortcutsPref once Local State is there, and written
// back on every change. Only it changes the pref, so it doesn't watch it
// (and holds no pointer to Local State, which goes away before it does).
class ShortcutSettings {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // The user's shortcuts changed.
    virtual void OnShortcutsChanged() {}
    // The settings page started or stopped recording keys.
    virtual void OnRecordingChanged() {}
  };

  static ShortcutSettings& Get();

  ShortcutSettings();
  ShortcutSettings(const ShortcutSettings&) = delete;
  ShortcutSettings& operator=(const ShortcutSettings&) = delete;
  ~ShortcutSettings();

  // nullopt when the user didn't change |id|'s shortcut; a ui::Accelerator()
  // when they removed it.
  std::optional<ui::Accelerator> GetCustom(std::string_view id) const;
  bool IsCustomized(std::string_view id) const;

  // Sets |id|'s shortcut (ui::Accelerator() for none), or with nullopt goes
  // back to its default. Observers hear of it at once.
  void SetCustom(std::string_view id,
                 std::optional<ui::Accelerator> accelerator);
  void ResetAll();

  // While the settings page records keys, the browser's shortcuts don't run,
  // so they reach the page (shortcuts_mac.h, PipWindows).
  bool recording() const { return recording_; }
  void SetRecording(bool recording);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Reads and writes |prefs| instead of Local State (nullptr: memory only),
  // starting from what it holds.
  void SetPrefServiceForTesting(PrefService* prefs);

 private:
  // Local State, or the test's prefs, if kShortcutsPref is registered there.
  PrefService* GetPrefs() const;
  // Reads the pref the first time it can (Get()).
  void MaybeLoad();
  void Load(PrefService* prefs);
  void Save();
  void NotifyChanged();

  // What kShortcutsPref holds, decoded.
  std::map<std::string, ui::Accelerator, std::less<>> custom_;
  bool loaded_ = false;
  bool testing_ = false;
  raw_ptr<PrefService> test_prefs_ = nullptr;
  bool recording_ = false;
  base::ObserverList<Observer> observers_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_SHORTCUTS_SHORTCUT_SETTINGS_H_
