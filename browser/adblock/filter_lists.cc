// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/filter_lists.h"

#include <algorithm>

#include "base/json/values_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "compass/browser/adblock/adblock_prefs.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "crypto/hash.h"
#include "url/gurl.h"

namespace compass::adblock {

namespace {

struct BuiltinList {
  const char* id;
  const char* title;
  const char* url;
};

// The same lists as iOS (FilterLists.swift).
constexpr BuiltinList kBuiltinLists[] = {
    {kEasyListId, "EasyList (ads)", "https://easylist.to/easylist/easylist.txt"},
    {kEasyPrivacyId, "EasyPrivacy (trackers)",
     "https://easylist.to/easylist/easyprivacy.txt"},
    {kCookiesId, "EasyList Cookie List",
     "https://secure.fanboy.co.nz/fanboy-cookiemonster.txt"},
};

constexpr char kCustomPrefix[] = "custom-";
constexpr size_t kMaxTitleLength = 80;

bool IsDisabled(const PrefService* local_state, std::string_view id) {
  for (const base::Value& value : local_state->GetList(prefs::kDisabledLists)) {
    if (value.is_string() && value.GetString() == id) {
      return true;
    }
  }
  return false;
}

std::string CustomListId(const GURL& url) {
  auto hash = crypto::hash::Sha256(url.spec());
  return kCustomPrefix +
         base::ToLowerASCII(base::HexEncode(base::span(hash).first<4>()));
}

// The title a list shows before its "! Title:" header is known: its host
// and file.
std::string DefaultTitle(const GURL& url) {
  std::string title(url.host());
  std::string_view path = url.path();
  if (size_t slash = path.rfind('/');
      slash != std::string_view::npos && slash + 1 < path.size()) {
    title += " · ";
    title += path.substr(slash + 1);
  }
  return title;
}

}  // namespace

FilterList::FilterList() = default;
FilterList::FilterList(const FilterList&) = default;
FilterList& FilterList::operator=(const FilterList&) = default;
FilterList::~FilterList() = default;

FilterListState::FilterListState() = default;
FilterListState::FilterListState(const FilterListState&) = default;
FilterListState& FilterListState::operator=(const FilterListState&) = default;
FilterListState::~FilterListState() = default;

std::vector<FilterList> GetFilterLists(const PrefService* local_state) {
  std::vector<FilterList> lists;
  for (const BuiltinList& builtin : kBuiltinLists) {
    FilterList& list = lists.emplace_back();
    list.id = builtin.id;
    list.title = builtin.title;
    list.url = builtin.url;
    list.builtin = true;
    list.enabled = !IsDisabled(local_state, list.id);
  }
  for (const base::Value& value : local_state->GetList(prefs::kCustomLists)) {
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      continue;
    }
    const std::string* id = dict->FindString("id");
    const std::string* url = dict->FindString("url");
    if (!id || !url || !base::StartsWith(*id, kCustomPrefix)) {
      continue;
    }
    FilterList& list = lists.emplace_back();
    list.id = *id;
    list.url = *url;
    const std::string* title = dict->FindString("title");
    list.title = title && !title->empty() ? *title : DefaultTitle(GURL(*url));
    list.enabled = !IsDisabled(local_state, list.id);
  }
  return lists;
}

std::vector<FilterList> GetEnabledFilterLists(const PrefService* local_state) {
  std::vector<FilterList> lists = GetFilterLists(local_state);
  std::erase_if(lists, [](const FilterList& list) { return !list.enabled; });
  return lists;
}

std::optional<FilterList> FindFilterList(const PrefService* local_state,
                                         std::string_view id) {
  for (FilterList& list : GetFilterLists(local_state)) {
    if (list.id == id) {
      return std::move(list);
    }
  }
  return std::nullopt;
}

void SetFilterListEnabled(PrefService* local_state,
                          std::string_view id,
                          bool enabled) {
  if (!FindFilterList(local_state, id) ||
      IsDisabled(local_state, id) == !enabled) {
    return;
  }
  ScopedListPrefUpdate update(local_state, prefs::kDisabledLists);
  if (enabled) {
    update->EraseIf([id](const base::Value& value) {
      return value.is_string() && value.GetString() == id;
    });
  } else {
    update->Append(std::string(id));
  }
}

std::string ValidateCustomListUrl(const PrefService* local_state,
                                  std::string_view url_text) {
  GURL url(base::TrimWhitespaceASCII(url_text, base::TRIM_ALL));
  if (!url.is_valid() || !url.has_host()) {
    return "Enter the list's full address, starting with https://";
  }
  if (!url.SchemeIs("https")) {
    return "Only https:// lists can be added";
  }
  for (const FilterList& list : GetFilterLists(local_state)) {
    if (GURL(list.url) == url) {
      return "That list is already added";
    }
  }
  return std::string();
}

std::string AddCustomFilterList(PrefService* local_state, const GURL& url) {
  std::string id = CustomListId(url);
  if (FindFilterList(local_state, id)) {
    return id;
  }
  base::DictValue dict;
  dict.Set("id", id);
  dict.Set("url", url.spec());
  ScopedListPrefUpdate(local_state, prefs::kCustomLists)
      ->Append(std::move(dict));
  return id;
}

void SetCustomFilterListTitle(PrefService* local_state,
                              std::string_view id,
                              std::string_view title) {
  std::string trimmed(base::TrimWhitespaceASCII(title, base::TRIM_ALL));
  if (trimmed.size() > kMaxTitleLength) {
    trimmed.resize(kMaxTitleLength);
  }
  if (trimmed.empty()) {
    return;
  }
  ScopedListPrefUpdate update(local_state, prefs::kCustomLists);
  for (base::Value& value : *update) {
    base::DictValue* dict = value.GetIfDict();
    const std::string* list_id = dict ? dict->FindString("id") : nullptr;
    if (list_id && *list_id == id) {
      const std::string* current = dict->FindString("title");
      if (!current || *current != trimmed) {
        dict->Set("title", trimmed);
      }
      return;
    }
  }
}

void RemoveCustomFilterList(PrefService* local_state, std::string_view id) {
  if (!base::StartsWith(id, kCustomPrefix)) {
    return;
  }
  ScopedListPrefUpdate(local_state, prefs::kCustomLists)
      ->EraseIf([id](const base::Value& value) {
        const base::DictValue* dict = value.GetIfDict();
        const std::string* list_id = dict ? dict->FindString("id") : nullptr;
        return list_id && *list_id == id;
      });
  ScopedListPrefUpdate(local_state, prefs::kDisabledLists)
      ->EraseIf([id](const base::Value& value) {
        return value.is_string() && value.GetString() == id;
      });
  ScopedDictPrefUpdate(local_state, prefs::kListState)->Remove(id);
}

FilterListState GetFilterListState(const PrefService* local_state,
                                   std::string_view id) {
  FilterListState state;
  const base::DictValue* dict =
      local_state->GetDict(prefs::kListState).FindDict(id);
  if (!dict) {
    return state;
  }
  auto string = [dict](std::string_view key) {
    const std::string* value = dict->FindString(key);
    return value ? *value : std::string();
  };
  auto time = [dict](std::string_view key) {
    const base::Value* value = dict->Find(key);
    return value ? base::ValueToTime(*value).value_or(base::Time())
                 : base::Time();
  };
  state.etag = string("etag");
  state.last_modified = string("last_modified");
  state.error = string("error");
  state.checked = time("checked");
  state.updated = time("updated");
  state.url_rules = dict->FindInt("url_rules").value_or(0);
  state.css_rules = dict->FindInt("css_rules").value_or(0);
  state.skipped = dict->FindInt("skipped").value_or(0);
  return state;
}

void SetFilterListState(PrefService* local_state,
                        std::string_view id,
                        const FilterListState& state) {
  if (!FindFilterList(local_state, id)) {
    return;
  }
  base::DictValue dict;
  dict.Set("etag", state.etag);
  dict.Set("last_modified", state.last_modified);
  dict.Set("error", state.error);
  dict.Set("checked", base::TimeToValue(state.checked));
  dict.Set("updated", base::TimeToValue(state.updated));
  dict.Set("url_rules", state.url_rules);
  dict.Set("css_rules", state.css_rules);
  dict.Set("skipped", state.skipped);
  ScopedDictPrefUpdate update(local_state, prefs::kListState);
  const base::DictValue* current = update->FindDict(id);
  if (!current || *current != dict) {
    update->Set(id, std::move(dict));
  }
}

base::FilePath GetAdBlockDir(const base::FilePath& user_data_dir) {
  return user_data_dir.Append(kRulesetConfig.top_level_directory);
}

base::FilePath GetListTextPath(const base::FilePath& user_data_dir,
                               std::string_view id) {
  return GetAdBlockDir(user_data_dir)
      .AppendASCII("Lists")
      .AppendASCII(std::string(id) + ".txt");
}

}  // namespace compass::adblock
