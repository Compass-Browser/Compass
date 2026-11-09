// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/claude_tabs_android.h"

#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/android/tab_android.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/android/tab_model/tab_model.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"
#include "compass/browser/claude/claude_prefs.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace compass {

namespace {

// The window's tab model for |profile|'s regular tabs that's on screen, or
// any.
TabModel* ActiveModel(Profile* profile) {
  TabModel* any = nullptr;
  for (TabModel* model : TabModelList::models()) {
    if (model->IsOffTheRecord() || model->GetProfile() != profile) {
      continue;
    }
    if (model->IsActiveModel()) {
      return model;
    }
    any = any ? any : model;
  }
  return any;
}

}  // namespace

ClaudeTabsAndroid::ClaudeTabsAndroid(Profile* profile) : profile_(profile) {}

ClaudeTabsAndroid::~ClaudeTabsAndroid() = default;

bool ClaudeTabsAndroid::BrowserScopeAllowed() {
  return profile_->GetPrefs()->GetBoolean(claude::prefs::kBrowserScope);
}

std::string ClaudeTabsAndroid::ListTabs(const Context& context) {
  std::string text;
  int window = 0;
  for (TabModel* model : TabModelList::models()) {
    if (model->IsOffTheRecord() || model->GetProfile() != profile_ ||
        model->GetTabCount() == 0) {
      continue;
    }
    text += "Window " + base::NumberToString(++window) + ":\n";
    for (int i = 0; i < model->GetTabCount(); ++i) {
      TabAndroid* tab = model->GetTabAt(i);
      if (!tab) {
        continue;
      }
      text += "- [" + base::NumberToString(tab->GetAndroidId()) + "] " +
              base::UTF16ToUTF8(tab->GetTitle()) + " — " +
              tab->GetURL().spec();
      if (i == model->GetActiveIndex()) {
        text += " (on screen)";
      }
      if (context.target && tab->GetContents() == context.target) {
        text += " (Claude's tab)";
      }
      if (!tab->GetContents()) {
        text += " (not loaded)";
      }
      text += "\n";
    }
  }
  return text.empty() ? "No tabs are open." : text;
}

TabAndroid* ClaudeTabsAndroid::FindTab(const std::string& id) {
  int tab_id = 0;
  if (!base::StringToInt(id, &tab_id)) {
    return nullptr;
  }
  for (TabModel* model : TabModelList::models()) {
    if (model->IsOffTheRecord() || model->GetProfile() != profile_) {
      continue;
    }
    for (int i = 0; i < model->GetTabCount(); ++i) {
      TabAndroid* tab = model->GetTabAt(i);
      if (tab && tab->GetAndroidId() == tab_id) {
        return tab;
      }
    }
  }
  return nullptr;
}

content::WebContents* ClaudeTabsAndroid::FindLiveTab(const std::string& id) {
  TabAndroid* tab = FindTab(id);
  return tab ? tab->GetContents() : nullptr;
}

base::expected<content::WebContents*, std::string> ClaudeTabsAndroid::LoadTab(
    const std::string& id) {
  TabAndroid* tab = FindTab(id);
  if (!tab) {
    return base::unexpected("There's no tab " + id + ". list_tabs lists them.");
  }
  if (!tab->GetContents()) {
    // Android loads a tab when it's shown.
    return base::unexpected(
        "Tab " + id +
        " isn't loaded: Android loads a tab when it's shown. Ask the user to "
        "open it, or open its URL with open_tab.");
  }
  return tab->GetContents();
}

content::WebContents* ClaudeTabsAndroid::OpenTab(const GURL& url,
                                                 bool active) {
  TabModel* model = ActiveModel(profile_);
  if (!model) {
    return nullptr;
  }
  tabs::TabInterface* tab =
      model->OpenTab(url, model->GetTabCount(), /*foreground=*/active);
  return tab ? tab->GetContents() : nullptr;
}

std::string ClaudeTabsAndroid::IdOf(content::WebContents* contents) {
  TabAndroid* tab = contents ? TabAndroid::FromWebContents(contents) : nullptr;
  return tab ? base::NumberToString(tab->GetAndroidId()) : std::string();
}

void ClaudeTabsAndroid::RunTool(const std::string& name,
                                base::DictValue input,
                                const Context& context,
                                base::OnceCallback<void(Result)> done) {
  Result result;
  if (name == "close_tab") {
    const std::string* id = input.FindString("tab_id");
    TabAndroid* tab = id ? FindTab(*id) : nullptr;
    TabModel* model = tab ? TabModelList::GetTabModelForTabAndroid(tab) : nullptr;
    if (!tab || !model) {
      result.result = claude::ToolResult::Error(
          "There's no such tab. list_tabs lists them.");
    } else {
      model->CloseTab(tab->GetHandle());
      result.result = claude::ToolResult::Text("Closed tab " + *id + ".");
    }
  } else {
    result.result = claude::ToolResult::Error(
        name + " isn't available in Compass for Android.");
  }
  std::move(done).Run(std::move(result));
}

}  // namespace compass
