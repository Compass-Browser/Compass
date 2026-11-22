// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ADBLOCK_FILTER_LISTS_H_
#define COMPASS_BROWSER_ADBLOCK_FILTER_LISTS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"

class GURL;
class PrefService;

// The ad blocker's filter lists (docs/design/ad-blocker.md): the built-in
// ones, the same as iOS (ios/CompassKit/.../FilterLists/FilterLists.swift),
// and the ones the user added by URL. Which lists are on and what each
// download left lives in Local State, shared by every profile, as the rules
// themselves are (one ruleset per browser).
namespace compass::adblock {

struct FilterList {
  FilterList();
  FilterList(const FilterList&);
  FilterList& operator=(const FilterList&);
  ~FilterList();

  std::string id;
  std::string title;
  std::string url;
  bool builtin = false;
  bool enabled = true;
};

// What the last downloads and the last ruleset build left for a list.
struct FilterListState {
  FilterListState();
  FilterListState(const FilterListState&);
  FilterListState& operator=(const FilterListState&);
  ~FilterListState();

  std::string etag;
  std::string last_modified;
  // The last time the list's server was asked, and the last time the text
  // changed (or was first downloaded). Null until then.
  base::Time checked;
  base::Time updated;
  // The last download's error, empty when it worked.
  std::string error;
  // From the last ruleset build: rules Compass uses (network and element
  // hiding) and the ones it can't (scriptlets, regular expressions, ...).
  int url_rules = 0;
  int css_rules = 0;
  int skipped = 0;
};

// The built-in lists, in the order they're shown and built (EasyList first,
// so it's the last to lose rules when the element hiding index is full).
inline constexpr char kEasyListId[] = "easylist";
inline constexpr char kEasyPrivacyId[] = "easyprivacy";
inline constexpr char kCookiesId[] = "cookies";

// Every list, built-ins first, then the user's in the order they were added.
std::vector<FilterList> GetFilterLists(const PrefService* local_state);

// The enabled lists of GetFilterLists().
std::vector<FilterList> GetEnabledFilterLists(const PrefService* local_state);

std::optional<FilterList> FindFilterList(const PrefService* local_state,
                                         std::string_view id);

void SetFilterListEnabled(PrefService* local_state,
                          std::string_view id,
                          bool enabled);

// Why a URL can't be added: not https, not a URL, or already a list. Empty
// when it can.
std::string ValidateCustomListUrl(const PrefService* local_state,
                                  std::string_view url);

// Adds the list at `url` (ValidateCustomListUrl() must pass) and returns its
// ID, "custom-" and a hash of the URL.
std::string AddCustomFilterList(PrefService* local_state, const GURL& url);

// Renames a user's list, from its "! Title:" header once it downloads.
void SetCustomFilterListTitle(PrefService* local_state,
                              std::string_view id,
                              std::string_view title);

// Removes a user's list and its state. Built-in lists can only be turned
// off.
void RemoveCustomFilterList(PrefService* local_state, std::string_view id);

FilterListState GetFilterListState(const PrefService* local_state,
                                   std::string_view id);
void SetFilterListState(PrefService* local_state,
                        std::string_view id,
                        const FilterListState& state);

// Where the ad blocker keeps its files: <user data>/Compass Ad Block.
base::FilePath GetAdBlockDir(const base::FilePath& user_data_dir);
// A list's last good download.
base::FilePath GetListTextPath(const base::FilePath& user_data_dir,
                               std::string_view id);

}  // namespace compass::adblock

#endif  // COMPASS_BROWSER_ADBLOCK_FILTER_LISTS_H_
