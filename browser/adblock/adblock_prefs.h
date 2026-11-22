// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ADBLOCK_ADBLOCK_PREFS_H_
#define COMPASS_BROWSER_ADBLOCK_ADBLOCK_PREFS_H_

#include "base/files/file_path.h"
#include "components/subresource_filter/core/common/ruleset_config.h"

class PrefRegistrySimple;

// The ad blocker's Local State prefs (docs/design/ad-blocker.md). Local
// State, not the profile: the rules are one ruleset for the whole browser.
// Each profile turns blocking off for a site with its Ads site setting
// (ContentSettingsType::ADS).
namespace compass::adblock {

namespace prefs {

// The ad blocker's switch. Default on.
inline constexpr char kEnabled[] = "compass.adblock.enabled";

// The lists the user added: a list of {"id", "url", "title"}.
inline constexpr char kCustomLists[] = "compass.adblock.custom_lists";

// The IDs of the lists turned off, built-in or added.
inline constexpr char kDisabledLists[] = "compass.adblock.disabled_lists";

// What the downloads and the last build left for each list, by ID
// (FilterListState).
inline constexpr char kListState[] = "compass.adblock.list_state";

}  // namespace prefs

// The ruleset the ad blocker gives upstream's subresource filter, in place
// of Google's Safe Browsing one (patches/chrome-browser-browser_process_impl
// .cc.patch): its own version prefs and folder, so nothing of Google's
// component mixes with it.
inline constexpr subresource_filter::RulesetConfig kRulesetConfig = {
    .filter_tag = std::string_view("compass_adblock"),
    .top_level_directory = FILE_PATH_LITERAL("Compass Ad Block"),
    .uma_tag = std::string_view("SubresourceFilter")};

// Registered by patches/chrome-browser-prefs-browser_prefs.cc.patch.
void RegisterAdBlockLocalStatePrefs(PrefRegistrySimple* registry);

}  // namespace compass::adblock

#endif  // COMPASS_BROWSER_ADBLOCK_ADBLOCK_PREFS_H_
