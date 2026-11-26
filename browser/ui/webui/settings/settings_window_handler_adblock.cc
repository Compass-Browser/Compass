// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings window's Privacy › Ad blocker (docs/design/ad-blocker.md):
// its lists, through compass::adblock::AdBlockService. The switch is a pref
// (kAllowedPrefs).

#include <utility>
#include <vector>

#include "compass/browser/adblock/adblock_service.h"
#include "compass/browser/ui/webui/settings/settings_window_handler.h"
#include "compass/strings/grit/compass_strings.h"
#include "ui/base/l10n/l10n_util.h"

namespace compass {

namespace {

double ToMs(base::Time time) {
  return time.is_null() ? 0 : time.InMillisecondsFSinceUnixEpoch();
}

}  // namespace

void SettingsWindowHandler::GetFilterLists(GetFilterListsCallback callback) {
  std::vector<sidebar::mojom::FilterListPtr> lists;
  if (adblock::AdBlockService* service = adblock::AdBlockService::Get()) {
    for (const adblock::AdBlockService::ListInfo& info : service->GetLists()) {
      lists.push_back(sidebar::mojom::FilterList::New(
          info.list.id, info.list.title, info.list.url, info.list.builtin,
          info.list.enabled, info.state.url_rules, info.state.css_rules,
          info.state.skipped, ToMs(info.state.updated),
          ToMs(info.state.checked), info.state.error, info.updating));
    }
  }
  std::move(callback).Run(std::move(lists));
}

void SettingsWindowHandler::SetFilterListEnabled(const std::string& id,
                                                 bool enabled) {
  if (adblock::AdBlockService* service = adblock::AdBlockService::Get()) {
    service->SetListEnabled(id, enabled);
  }
}

void SettingsWindowHandler::AddFilterList(const std::string& url,
                                          AddFilterListCallback callback) {
  adblock::AdBlockService* service = adblock::AdBlockService::Get();
  std::move(callback).Run(
      service ? service->AddList(url)
              : l10n_util::GetStringUTF8(IDS_COMPASS_ADBLOCK_NOT_RUNNING));
}

void SettingsWindowHandler::RemoveFilterList(const std::string& id) {
  if (adblock::AdBlockService* service = adblock::AdBlockService::Get()) {
    service->RemoveList(id);
  }
}

void SettingsWindowHandler::UpdateFilterListsNow() {
  if (adblock::AdBlockService* service = adblock::AdBlockService::Get()) {
    service->UpdateNow();
  }
}

void SettingsWindowHandler::OnFilterListsChanged() {
  page_->FilterListsChanged();
}

}  // namespace compass
