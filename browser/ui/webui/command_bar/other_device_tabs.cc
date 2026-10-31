// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/command_bar/other_device_tabs.h"

#include <algorithm>
#include <set>

#include "base/i18n/case_conversion.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "components/sessions/core/serialized_navigation_entry.h"
#include "components/sessions/core/session_types.h"
#include "components/sync_sessions/open_tabs_ui_delegate.h"
#include "components/sync_sessions/synced_session.h"

namespace compass {

std::vector<OtherDevice> GetOtherDeviceTabs(
    sync_sessions::OpenTabsUIDelegate* delegate) {
  std::vector<OtherDevice> devices;
  if (!delegate) {
    return devices;
  }
  std::vector<raw_ptr<const sync_sessions::SyncedSession, VectorExperimental>>
      sessions;
  // Most recent first.
  delegate->GetAllForeignSessions(&sessions);
  for (const sync_sessions::SyncedSession* session : sessions) {
    std::vector<const sessions::SessionTab*> tabs;
    // Recent first, without tabs that can't sync or the new tab page.
    if (!delegate->GetForeignSessionTabs(session->GetSessionTag(), &tabs)) {
      continue;
    }
    OtherDevice device;
    device.name = base::UTF8ToUTF16(session->GetSessionName());
    std::set<GURL> seen;
    for (const sessions::SessionTab* tab : tabs) {
      if (tab->navigations.empty()) {
        continue;
      }
      const sessions::SerializedNavigationEntry& entry =
          tab->navigations[tab->normalized_navigation_index()];
      const GURL& url = entry.virtual_url();
      if (!url.is_valid() || !seen.insert(url).second) {
        continue;
      }
      device.tabs.push_back({entry.title(), url});
    }
    if (!device.tabs.empty()) {
      devices.push_back(std::move(device));
    }
  }
  return devices;
}

std::vector<OtherDeviceTabMatch> MatchOtherDeviceTabs(
    const std::vector<OtherDevice>& devices,
    const std::u16string& query,
    size_t limit) {
  std::vector<OtherDeviceTabMatch> matches;
  const std::vector<std::u16string> words =
      base::SplitString(base::i18n::ToLower(query), u" ", base::TRIM_WHITESPACE,
                        base::SPLIT_WANT_NONEMPTY);
  if (words.empty()) {
    return matches;
  }
  for (size_t d = 0; d < devices.size(); ++d) {
    const OtherDevice& device = devices[d];
    const std::u16string name = base::i18n::ToLower(device.name);
    for (size_t t = 0; t < device.tabs.size(); ++t) {
      if (matches.size() == limit) {
        return matches;
      }
      const OtherDeviceTab& tab = device.tabs[t];
      const std::u16string haystack =
          base::i18n::ToLower(tab.title + u" " +
                              base::UTF8ToUTF16(tab.url.spec())) +
          u" " + name;
      if (std::ranges::all_of(words, [&](const std::u16string& word) {
            return haystack.find(word) != std::u16string::npos;
          })) {
        matches.push_back({d, t});
      }
    }
  }
  return matches;
}

}  // namespace compass
