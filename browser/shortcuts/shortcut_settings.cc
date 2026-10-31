// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/shortcuts/shortcut_settings.h"

#include <utility>

#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace compass {

namespace {

constexpr char kKeyField[] = "key";
constexpr char kModifiersField[] = "modifiers";

// The modifiers a shortcut can have.
constexpr int kShortcutModifiers = ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN |
                                   ui::EF_ALT_DOWN | ui::EF_COMMAND_DOWN;

#if BUILDFLAG(IS_MAC)
constexpr int kSpaceModifiers = ui::EF_CONTROL_DOWN;
#else
constexpr int kSpaceModifiers = ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN;
#endif

bool IsModifierKey(ui::KeyboardCode key) {
  switch (key) {
    case ui::VKEY_SHIFT:
    case ui::VKEY_LSHIFT:
    case ui::VKEY_RSHIFT:
    case ui::VKEY_CONTROL:
    case ui::VKEY_LCONTROL:
    case ui::VKEY_RCONTROL:
    case ui::VKEY_MENU:
    case ui::VKEY_LMENU:
    case ui::VKEY_RMENU:
    case ui::VKEY_LWIN:
    case ui::VKEY_RWIN:
    case ui::VKEY_CAPITAL:
    case ui::VKEY_NUMLOCK:
    case ui::VKEY_ALTGR:
      return true;
    default:
      return false;
  }
}

// The user's accelerator for |id| if they changed it, else |fallback|.
std::vector<ui::Accelerator> AcceleratorsFor(std::string_view id,
                                             const ui::Accelerator& fallback) {
  std::optional<ui::Accelerator> custom =
      ShortcutSettings::Get().GetCustom(id);
  const ui::Accelerator accelerator = custom.value_or(fallback);
  if (accelerator.key_code() == ui::VKEY_UNKNOWN) {
    return {};
  }
  return {accelerator};
}

}  // namespace

void RegisterShortcutPrefs(PrefRegistrySimple* registry) {
  registry->RegisterDictionaryPref(kShortcutsPref);
}

std::string SpaceShortcutId(int index) {
  return "space-" + base::NumberToString(index + 1);
}

ui::Accelerator GetDefaultSpaceAccelerator(int index) {
  return ui::Accelerator(static_cast<ui::KeyboardCode>(ui::VKEY_1 + index),
                         kSpaceModifiers);
}

ui::Accelerator GetDefaultPipClickThroughAccelerator() {
  return ui::Accelerator(
      ui::VKEY_P, ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN | ui::EF_COMMAND_DOWN);
}

std::vector<ui::Accelerator> GetSpaceAccelerators(int index) {
  return AcceleratorsFor(SpaceShortcutId(index),
                         GetDefaultSpaceAccelerator(index));
}

std::vector<ui::Accelerator> GetPipClickThroughAccelerators() {
  return AcceleratorsFor(kPipClickThroughShortcutId,
                         GetDefaultPipClickThroughAccelerator());
}

bool IsValidShortcut(const ui::Accelerator& accelerator) {
  return accelerator.key_code() != ui::VKEY_UNKNOWN &&
         !IsModifierKey(accelerator.key_code()) &&
         (accelerator.modifiers() &
          (ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN | ui::EF_COMMAND_DOWN));
}

base::Value::Dict AcceleratorToValue(const ui::Accelerator& accelerator) {
  base::Value::Dict value;
  if (accelerator.key_code() != ui::VKEY_UNKNOWN) {
    value.Set(kKeyField, static_cast<int>(accelerator.key_code()));
    value.Set(kModifiersField, accelerator.modifiers() & kShortcutModifiers);
  }
  return value;
}

std::optional<ui::Accelerator> AcceleratorFromValue(
    const base::Value::Dict& value) {
  if (value.empty()) {
    return ui::Accelerator();
  }
  std::optional<int> key = value.FindInt(kKeyField);
  std::optional<int> modifiers = value.FindInt(kModifiersField);
  if (!key || !modifiers || *key <= 0 || *key > 0xFF) {
    return std::nullopt;
  }
  ui::Accelerator accelerator(static_cast<ui::KeyboardCode>(*key),
                              *modifiers & kShortcutModifiers);
  if (!IsValidShortcut(accelerator)) {
    return std::nullopt;
  }
  return accelerator;
}

// static
ShortcutSettings& ShortcutSettings::Get() {
  static base::NoDestructor<ShortcutSettings> instance;
  instance->MaybeLoad();
  return *instance;
}

ShortcutSettings::ShortcutSettings() = default;
ShortcutSettings::~ShortcutSettings() = default;

std::optional<ui::Accelerator> ShortcutSettings::GetCustom(
    std::string_view id) const {
  auto it = custom_.find(id);
  if (it == custom_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool ShortcutSettings::IsCustomized(std::string_view id) const {
  return custom_.contains(id);
}

void ShortcutSettings::SetCustom(std::string_view id,
                                 std::optional<ui::Accelerator> accelerator) {
  if (accelerator) {
    custom_[std::string(id)] = *accelerator;
  } else {
    auto it = custom_.find(id);
    if (it == custom_.end()) {
      return;
    }
    custom_.erase(it);
  }
  Save();
  NotifyChanged();
}

void ShortcutSettings::ResetAll() {
  if (custom_.empty()) {
    return;
  }
  custom_.clear();
  Save();
  NotifyChanged();
}

void ShortcutSettings::SetRecording(bool recording) {
  if (recording_ == recording) {
    return;
  }
  recording_ = recording;
  for (Observer& observer : observers_) {
    observer.OnRecordingChanged();
  }
}

void ShortcutSettings::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void ShortcutSettings::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void ShortcutSettings::SetPrefServiceForTesting(PrefService* prefs) {
  testing_ = true;
  test_prefs_ = prefs;
  custom_.clear();
  loaded_ = false;
  if (prefs) {
    Load(prefs);
    loaded_ = true;
  }
  NotifyChanged();
}

PrefService* ShortcutSettings::GetPrefs() const {
  PrefService* prefs = nullptr;
  if (testing_) {
    prefs = test_prefs_;
  } else if (g_browser_process) {
    prefs = g_browser_process->local_state();
  }
  // Not registered: tests with a Local State of their own.
  return prefs && prefs->FindPreference(kShortcutsPref) ? prefs : nullptr;
}

void ShortcutSettings::MaybeLoad() {
  if (loaded_) {
    return;
  }
  PrefService* prefs = GetPrefs();
  if (!prefs) {
    return;
  }
  loaded_ = true;
  Load(prefs);
  if (!custom_.empty()) {
    NotifyChanged();
  }
}

void ShortcutSettings::Load(PrefService* prefs) {
  custom_.clear();
  for (const auto [id, value] : prefs->GetDict(kShortcutsPref)) {
    if (!value.is_dict()) {
      continue;
    }
    if (std::optional<ui::Accelerator> accelerator =
            AcceleratorFromValue(value.GetDict())) {
      custom_[id] = *accelerator;
    }
  }
}

void ShortcutSettings::Save() {
  PrefService* prefs = GetPrefs();
  if (!prefs) {
    return;
  }
  base::Value::Dict dict;
  for (const auto& [id, accelerator] : custom_) {
    dict.Set(id, AcceleratorToValue(accelerator));
  }
  prefs->SetDict(kShortcutsPref, std::move(dict));
}

void ShortcutSettings::NotifyChanged() {
  for (Observer& observer : observers_) {
    observer.OnShortcutsChanged();
  }
}

}  // namespace compass
