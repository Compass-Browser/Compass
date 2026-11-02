// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/claude_tabs.h"

#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/live_tab_registry.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"

namespace compass::claude {

namespace {

constexpr char kIdPrefix[] = "t";

std::string IdOfTab(const tabs::TabInterface* tab) {
  return kIdPrefix + base::NumberToString(tab->GetHandle().raw_value());
}

std::string SpaceName(SpaceService* service, const tabs::TabInterface* tab) {
  if (!service) {
    return std::string();
  }
  const SidebarNode* node = service->registry().GetNode(tab);
  SpaceModel* space = node ? service->space_list().FindSpaceOf(node) : nullptr;
  if (!space) {
    return "none";
  }
  if (space == &service->space_list().favorites()) {
    return "Favorites";
  }
  return base::UTF16ToUTF8(space->name());
}

}  // namespace

ClaudeTabs::ClaudeTabs(Profile* profile, BrowserWindowInterface* home)
    : profile_(profile), home_(home) {}

ClaudeTabs::~ClaudeTabs() = default;

bool ClaudeTabs::BrowserScopeAllowed() {
  return profile_->GetPrefs()->GetBoolean(prefs::kBrowserScope);
}

std::vector<TabInfo> ClaudeTabs::ListTabs() {
  SpaceService* service = SpaceService::GetIfExists(profile_);
  std::vector<TabInfo> shown;
  std::vector<TabInfo> hidden;
  int window_number = 0;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetProfile() != profile_) {
          return true;
        }
        WindowFeatures* features = WindowFeatures::From(browser);
        // The parking window, and windows swapped away for another
        // profile's space, hold tabs no window shows.
        const bool is_hidden =
            (service && service->IsParkingWindow(browser)) ||
            (features && features->swapped_away());
        if (!is_hidden &&
            browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
          return true;
        }
        std::string window;
        if (is_hidden) {
          window = "Not shown (spaces no window shows)";
        } else {
          window = "Window " + base::NumberToString(++window_number);
          if (features && !features->is_parking_window()) {
            window += " (shows space " +
                      base::UTF16ToUTF8(features->space_model().name()) + ")";
          }
        }
        TabStripModel* model = browser->GetTabStripModel();
        for (int i = 0; i < model->count(); ++i) {
          tabs::TabInterface* tab = model->GetTabAtIndex(i);
          content::WebContents* contents = tab->GetContents();
          TabInfo info;
          info.id = IdOfTab(tab);
          info.title = base::UTF16ToUTF8(contents->GetTitle());
          info.url = contents->GetVisibleURL().spec();
          info.window = window;
          info.space = SpaceName(service, tab);
          info.active = !is_hidden && i == model->active_index();
          info.contents = contents->GetWeakPtr();
          (is_hidden ? hidden : shown).push_back(std::move(info));
        }
        return true;
      });
  for (TabInfo& info : hidden) {
    shown.push_back(std::move(info));
  }
  return shown;
}

content::WebContents* ClaudeTabs::FindTab(const std::string& id) {
  int raw = 0;
  if (!id.starts_with(kIdPrefix) ||
      !base::StringToInt(id.substr(sizeof(kIdPrefix) - 1), &raw)) {
    return nullptr;
  }
  tabs::TabInterface* tab = tabs::TabHandle(raw).Get();
  if (!tab || !tab->GetContents() ||
      Profile::FromBrowserContext(tab->GetContents()->GetBrowserContext()) !=
          profile_) {
    return nullptr;
  }
  return tab->GetContents();
}

content::WebContents* ClaudeTabs::OpenTab(const GURL& url, bool active) {
  BrowserWindowInterface* home = Home();
  if (!home) {
    return nullptr;
  }
  // A new tab is a new Today entry of the window's space (TabStripBridge).
  NavigateParams params(home, url, ui::PAGE_TRANSITION_LINK);
  params.disposition = active ? WindowOpenDisposition::NEW_FOREGROUND_TAB
                              : WindowOpenDisposition::NEW_BACKGROUND_TAB;
  Navigate(&params);
  return params.navigated_or_inserted_contents;
}

std::string ClaudeTabs::IdOf(content::WebContents* contents) {
  const tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(contents);
  return tab ? IdOfTab(tab) : std::string();
}

void ClaudeTabs::CloseTab(content::WebContents* contents) {
  if (tabs::TabInterface* tab =
          tabs::TabInterface::MaybeGetFromContents(contents)) {
    tab->Close();
  }
}

BrowserWindowInterface* ClaudeTabs::Home() {
  BrowserWindowInterface* home = nullptr;
  BrowserWindowInterface* last_active = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetProfile() != profile_ ||
            browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
          return true;
        }
        WindowFeatures* features = WindowFeatures::From(browser);
        if (features && (features->is_parking_window() ||
                         features->swapped_away())) {
          return true;
        }
        if (!last_active) {
          last_active = browser;
        }
        if (browser == home_) {
          home = browser;
          return false;
        }
        return true;
      });
  return home ? home : last_active;
}

}  // namespace compass::claude
