// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_COMMAND_BAR_OTHER_DEVICE_TABS_H_
#define COMPASS_BROWSER_UI_WEBUI_COMMAND_BAR_OTHER_DEVICE_TABS_H_

#include <cstddef>
#include <string>
#include <vector>

#include "url/gurl.h"

namespace sync_sessions {
class OpenTabsUIDelegate;
}  // namespace sync_sessions

// "Tabs on other devices" in the command bar (docs/design/command-bar.md,
// "Results"; docs/design/sync.md, "UI"): the open tabs the
// SESSIONS type brings from the chain's other devices.
namespace compass {

struct OtherDeviceTab {
  std::u16string title;
  GURL url;
};

struct OtherDevice {
  std::u16string name;
  // Most recently used first.
  std::vector<OtherDeviceTab> tabs;
};

// The other devices' tabs, the most recently active device first. Empty if
// open tabs don't sync (|delegate| null). A URL open twice on a device is
// listed once.
std::vector<OtherDevice> GetOtherDeviceTabs(
    sync_sessions::OpenTabsUIDelegate* delegate);

struct OtherDeviceTabMatch {
  // Indexes into the devices and that device's tabs.
  size_t device = 0;
  size_t tab = 0;
};

// The tabs whose title, URL or device name contain every word of |query|
// (case-insensitive), grouped by device in |devices|' order, at most
// |limit|. An empty query matches nothing: the bar's empty state has its
// own rows.
std::vector<OtherDeviceTabMatch> MatchOtherDeviceTabs(
    const std::vector<OtherDevice>& devices,
    const std::u16string& query,
    size_t limit);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_COMMAND_BAR_OTHER_DEVICE_TABS_H_
