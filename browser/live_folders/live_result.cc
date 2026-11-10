// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_result.h"

#include <set>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "url/gurl.h"

namespace compass {

namespace {

// runtime/result.js's LIMITS.
constexpr size_t kMaxIdLength = 512;
constexpr size_t kMaxTitleLength = 300;
constexpr size_t kMaxUrlLength = 2048;
constexpr size_t kMaxFolderTitleLength = 100;

}  // namespace

LiveResult::LiveResult() = default;
LiveResult::LiveResult(LiveResult&&) = default;
LiveResult& LiveResult::operator=(LiveResult&&) = default;
LiveResult::~LiveResult() = default;

base::expected<LiveResult, LiveError> ParseLiveResult(std::string_view json) {
  std::optional<base::Value> value =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  const base::DictValue* dict = value ? value->GetIfDict() : nullptr;
  const base::ListValue* items = dict ? dict->FindList("items") : nullptr;
  if (!items) {
    return base::unexpected(
        LiveError{"bad_result", "fetchItems must return {items: [...]}"});
  }
  LiveResult result;
  std::set<std::string, std::less<>> keys;
  for (const base::Value& item : *items) {
    if (result.items.size() == kMaxLiveItems) {
      break;
    }
    const base::DictValue* d = item.GetIfDict();
    const std::string* id = d ? d->FindString("id") : nullptr;
    const std::string* url = d ? d->FindString("url") : nullptr;
    const std::string* title = d ? d->FindString("title") : nullptr;
    if (!id || id->empty() || id->size() > kMaxIdLength || !url ||
        url->size() > kMaxUrlLength || !title || title->empty() ||
        keys.contains(*id)) {
      continue;
    }
    GURL gurl(*url);
    if (!gurl.is_valid() || !gurl.SchemeIsHTTPOrHTTPS()) {
      continue;
    }
    std::u16string title16 = base::UTF8ToUTF16(*title);
    if (title16.size() > kMaxTitleLength) {
      title16.resize(kMaxTitleLength);
    }
    keys.insert(*id);
    result.items.push_back({*id, std::move(gurl), std::move(title16)});
  }
  if (const std::string* title = dict->FindString("title")) {
    result.title = base::UTF8ToUTF16(*title);
    if (result.title.size() > kMaxFolderTitleLength) {
      result.title.resize(kMaxFolderTitleLength);
    }
  }
  if (std::optional<double> next = dict->FindDouble("nextRefresh");
      next && *next > 0) {
    result.next_refresh = base::Seconds(*next);
  }
  return result;
}

}  // namespace compass
