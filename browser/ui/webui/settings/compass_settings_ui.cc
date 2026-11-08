// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/settings/compass_settings_ui.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/webui/webui_util.h"

#if BUILDFLAG(IS_MAC)
#include "compass/browser/ui/cocoa/shortcuts_mac.h"
#elif BUILDFLAG(IS_WIN)
#include "chrome/browser/ui/accelerator_table.h"
#include "compass/browser/shortcuts/shortcuts_win.h"
#include "compass/browser/ui/views/frame/browser_view_accelerators_win.h"
#endif

namespace compass {

namespace {

ui::Accelerator ToAccelerator(const sidebar::mojom::ShortcutKeys& keys) {
  if (keys.key_code <= 0 || keys.key_code > 0xFF) {
    return ui::Accelerator();
  }
  int modifiers = 0;
  if (keys.shift) {
    modifiers |= ui::EF_SHIFT_DOWN;
  }
  if (keys.ctrl) {
    modifiers |= ui::EF_CONTROL_DOWN;
  }
  if (keys.alt) {
    modifiers |= ui::EF_ALT_DOWN;
  }
  if (keys.meta) {
    modifiers |= ui::EF_COMMAND_DOWN;
  }
  return ui::Accelerator(static_cast<ui::KeyboardCode>(keys.key_code),
                         modifiers);
}

#if BUILDFLAG(IS_WIN)
// Names for the keys upstream's text leaves blank on Windows (it names only
// F1, F4, F6 and F11 among the function keys, and maps the rest through the
// keyboard layout, which has no character for them) or writes like main
// keyboard keys (the numpad's).
std::u16string WinKeyName(ui::KeyboardCode key) {
  if (key >= ui::VKEY_F1 && key <= ui::VKEY_F24) {
    return u"F" + base::NumberToString16(key - ui::VKEY_F1 + 1);
  }
  if (key >= ui::VKEY_NUMPAD0 && key <= ui::VKEY_NUMPAD9) {
    return u"Num " + base::NumberToString16(key - ui::VKEY_NUMPAD0);
  }
  switch (key) {
    case ui::VKEY_ADD:
      return u"Num +";
    case ui::VKEY_SUBTRACT:
      return u"Num -";
    case ui::VKEY_MULTIPLY:
      return u"Num *";
    case ui::VKEY_DIVIDE:
      return u"Num /";
    case ui::VKEY_DECIMAL:
      return u"Num .";
    case ui::VKEY_BROWSER_BACK:
      return u"Browser Back";
    case ui::VKEY_BROWSER_FORWARD:
      return u"Browser Forward";
    case ui::VKEY_BROWSER_REFRESH:
      return u"Browser Refresh";
    case ui::VKEY_BROWSER_STOP:
      return u"Browser Stop";
    case ui::VKEY_BROWSER_SEARCH:
      return u"Browser Search";
    case ui::VKEY_BROWSER_FAVORITES:
      return u"Browser Favorites";
    case ui::VKEY_BROWSER_HOME:
      return u"Browser Home";
    default:
      return std::u16string();
  }
}
#endif

std::string ToText(const ui::Accelerator& accelerator) {
#if BUILDFLAG(IS_WIN)
  const std::u16string name = WinKeyName(accelerator.key_code());
  if (!name.empty()) {
    // Upstream's modifiers, before a key that has a name of its own.
    std::vector<std::u16string> parts =
        ui::Accelerator(ui::VKEY_A, accelerator.modifiers())
            .GetShortcutVectorRepresentation();
    parts.back() = name;
    return base::UTF16ToUTF8(base::JoinString(parts, u"+"));
  }
#endif
  return base::UTF16ToUTF8(accelerator.GetShortcutText());
}

std::vector<std::string> ToTexts(
    const std::vector<ui::Accelerator>& accelerators) {
  std::vector<std::string> texts;
  for (const ui::Accelerator& accelerator : accelerators) {
    texts.push_back(ToText(accelerator));
  }
  return texts;
}

const ListedShortcut* FindListed(const std::vector<ListedShortcut>& listed,
                                 std::string_view id) {
  auto it = std::ranges::find(listed, id, &ListedShortcut::id);
  return it == listed.end() ? nullptr : &*it;
}

// What giving |id| |accelerator| would do: why it can't (error), or which
// listed shortcut has it now (conflict).
struct Check {
  std::string error;
  raw_ptr<const ListedShortcut> conflict = nullptr;
};

Check CheckKeys(const std::vector<ListedShortcut>& listed,
                std::string_view id,
                const ui::Accelerator& accelerator) {
  Check check;
  if (!FindListed(listed, id)) {
    check.error = "There's no such shortcut.";
    return check;
  }
  if (!IsValidShortcut(accelerator)) {
#if BUILDFLAG(IS_MAC)
    // Command, Control or Option.
    check.error = "Use the key with \u2318, \u2303 or \u2325.";
#else
    check.error = "Use the key with Ctrl or Alt.";
#endif
    return check;
  }
#if BUILDFLAG(IS_WIN)
  // Click-through is a global hot key, and those can't take the Windows key
  // here (PipWindows::UpdateListening()).
  if (id == kPipClickThroughShortcutId && accelerator.IsCmdDown()) {
    check.error =
        "Use the key with Ctrl, Alt or Shift, not the Windows key.";
    return check;
  }
#endif
  for (const ListedShortcut& shortcut : listed) {
    if (shortcut.id != id && std::ranges::contains(shortcut.keys, accelerator)) {
      check.conflict = &shortcut;
      return check;
    }
  }
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  if (std::optional<std::u16string> built_in =
          FindBuiltInShortcut(accelerator)) {
    check.error = "Already used by " + base::UTF16ToUTF8(*built_in) + ".";
  }
#endif
  return check;
}

}  // namespace

ListedShortcut::ListedShortcut() = default;
ListedShortcut::ListedShortcut(const ListedShortcut&) = default;
ListedShortcut& ListedShortcut::operator=(const ListedShortcut&) = default;
ListedShortcut::~ListedShortcut() = default;

std::vector<ListedShortcut> ListShortcuts() {
  ShortcutSettings& settings = ShortcutSettings::Get();
  std::vector<ListedShortcut> listed;
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#if BUILDFLAG(IS_WIN)
  // The table's first load keeps the defaults (AdjustAcceleratorList()).
  GetAcceleratorList();
#endif
  for (const CustomizableCommand& command : GetCustomizableCommands()) {
    ListedShortcut& shortcut = listed.emplace_back();
    shortcut.id = command.id;
    shortcut.section = command.section;
    shortcut.label = command.label;
    shortcut.keys = GetAccelerators(command.command);
    shortcut.default_keys = GetDefaultAccelerators(command.command);
    shortcut.customized = settings.IsCustomized(command.id);
  }
#endif
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  {
    ListedShortcut& shortcut = listed.emplace_back();
    shortcut.id = kPipClickThroughShortcutId;
    shortcut.section = "Compass";
    shortcut.label = "Let clicks through picture-in-picture";
    shortcut.keys = GetPipClickThroughAccelerators();
    shortcut.default_keys = {GetDefaultPipClickThroughAccelerator()};
    shortcut.customized = settings.IsCustomized(shortcut.id);
  }
#endif
  for (int n = 0; n < kSpaceShortcutCount; ++n) {
    ListedShortcut& shortcut = listed.emplace_back();
    shortcut.id = SpaceShortcutId(n);
    shortcut.section = "Spaces";
    shortcut.label = "Switch to space " + base::NumberToString(n + 1);
    shortcut.keys = GetSpaceAccelerators(n);
    shortcut.default_keys = {GetDefaultSpaceAccelerator(n)};
    shortcut.customized = settings.IsCustomized(shortcut.id);
  }
  return listed;
}

CompassSettingsUIConfig::CompassSettingsUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, kSettingsHost) {}

CompassSettingsUI::CompassSettingsUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kSettingsHost);
  // Shares the sidebar's resource pak; only the default page differs.
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_SETTINGS_HTML);
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

CompassSettingsUI::~CompassSettingsUI() = default;

void CompassSettingsUI::BindInterface(
    mojo::PendingReceiver<sidebar::mojom::SettingsHandler> receiver) {
  handler_.reset();
  handler_ = std::make_unique<ShortcutsPageHandler>(std::move(receiver));
}

ShortcutsPageHandler::ShortcutsPageHandler(
    mojo::PendingReceiver<sidebar::mojom::SettingsHandler> receiver)
    : receiver_(this, std::move(receiver)) {
#if BUILDFLAG(IS_WIN)
  // The windows follow what changes here.
  BrowserViewAccelerators::Watch();
#endif
}

ShortcutsPageHandler::~ShortcutsPageHandler() {
  // The page went away while recording.
  SetRecording(false);
}

void ShortcutsPageHandler::GetShortcuts(GetShortcutsCallback callback) {
  std::vector<sidebar::mojom::ShortcutPtr> result;
  for (const ListedShortcut& listed : ListShortcuts()) {
    auto shortcut = sidebar::mojom::Shortcut::New();
    shortcut->id = listed.id;
    shortcut->section = listed.section;
    shortcut->label = listed.label;
    shortcut->keys = ToTexts(listed.keys);
    shortcut->default_keys = ToTexts(listed.default_keys);
    shortcut->customized = listed.customized;
    result.push_back(std::move(shortcut));
  }
  std::move(callback).Run(std::move(result));
}

void ShortcutsPageHandler::SetRecording(bool recording) {
  if (recording_ == recording) {
    return;
  }
  recording_ = recording;
  ShortcutSettings::Get().SetRecording(recording);
}

void ShortcutsPageHandler::CheckShortcut(const std::string& id,
                                      sidebar::mojom::ShortcutKeysPtr keys,
                                      CheckShortcutCallback callback) {
  const ui::Accelerator accelerator = ToAccelerator(*keys);
  const std::vector<ListedShortcut> listed = ListShortcuts();
  const Check check = CheckKeys(listed, id, accelerator);
  auto result = sidebar::mojom::ShortcutCheck::New();
  result->text = accelerator.key_code() == ui::VKEY_UNKNOWN
                     ? std::string()
                     : ToText(accelerator);
  result->error = check.error;
  if (check.conflict) {
    result->conflict_id = check.conflict->id;
    result->conflict_label = check.conflict->label;
  }
  std::move(callback).Run(std::move(result));
}

void ShortcutsPageHandler::SetShortcut(const std::string& id,
                                    sidebar::mojom::ShortcutKeysPtr keys,
                                    SetShortcutCallback callback) {
  const ui::Accelerator accelerator = ToAccelerator(*keys);
  const std::vector<ListedShortcut> listed = ListShortcuts();
  const Check check = CheckKeys(listed, id, accelerator);
  if (!check.error.empty()) {
    std::move(callback).Run();
    return;
  }
  ShortcutSettings& settings = ShortcutSettings::Get();
  if (check.conflict) {
    // It keeps its next keys, if it has others (next tab has several).
    std::vector<ui::Accelerator> rest = check.conflict->keys;
    std::erase(rest, accelerator);
    settings.SetCustom(check.conflict->id,
                       rest.empty() ? ui::Accelerator() : rest.front());
  }
  const ListedShortcut* shortcut = FindListed(listed, id);
  if (shortcut->default_keys == std::vector<ui::Accelerator>{accelerator}) {
    // Its own default: not a change.
    settings.SetCustom(id, std::nullopt);
  } else {
    settings.SetCustom(id, accelerator);
  }
  std::move(callback).Run();
}

void ShortcutsPageHandler::RemoveShortcut(const std::string& id,
                                       RemoveShortcutCallback callback) {
  const std::vector<ListedShortcut> listed = ListShortcuts();
  if (const ListedShortcut* shortcut = FindListed(listed, id)) {
    ShortcutSettings::Get().SetCustom(
        id, shortcut->default_keys.empty()
                ? std::nullopt
                : std::optional<ui::Accelerator>(ui::Accelerator()));
  }
  std::move(callback).Run();
}

void ShortcutsPageHandler::ResetShortcut(const std::string& id,
                                      ResetShortcutCallback callback) {
  const std::vector<ListedShortcut> listed = ListShortcuts();
  const ListedShortcut* shortcut = FindListed(listed, id);
  if (shortcut) {
    // Its defaults come back: other shortcuts given them since lose them.
    for (const ui::Accelerator& accelerator : shortcut->default_keys) {
      for (const ListedShortcut& other : listed) {
        if (other.id != id && other.customized &&
            std::ranges::contains(other.keys, accelerator)) {
          RemoveShortcut(other.id, base::DoNothing());
        }
      }
    }
    ShortcutSettings::Get().SetCustom(id, std::nullopt);
  }
  std::move(callback).Run();
}

void ShortcutsPageHandler::ResetAllShortcuts(
    ResetAllShortcutsCallback callback) {
  ShortcutSettings::Get().ResetAll();
  std::move(callback).Run();
}

WEB_UI_CONTROLLER_TYPE_IMPL(CompassSettingsUI)

}  // namespace compass
