// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_result.h"

#include <algorithm>
#include <cmath>
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
constexpr size_t kMaxSubtitleLength = 300;
constexpr size_t kMaxBadgeLength = 12;
// PREVIEW_LIMITS.
constexpr size_t kMaxFooterLength = 120;

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

LivePreviewItem::LivePreviewItem() = default;
LivePreviewItem::LivePreviewItem(const LivePreviewItem&) = default;
LivePreviewItem& LivePreviewItem::operator=(const LivePreviewItem&) = default;
LivePreviewItem::LivePreviewItem(LivePreviewItem&&) = default;
LivePreviewItem& LivePreviewItem::operator=(LivePreviewItem&&) = default;
LivePreviewItem::~LivePreviewItem() = default;

LivePreviewResult::LivePreviewResult() = default;
LivePreviewResult::LivePreviewResult(const LivePreviewResult&) = default;
LivePreviewResult& LivePreviewResult::operator=(const LivePreviewResult&) =
    default;
LivePreviewResult::LivePreviewResult(LivePreviewResult&&) = default;
LivePreviewResult& LivePreviewResult::operator=(LivePreviewResult&&) = default;
LivePreviewResult::~LivePreviewResult() = default;

base::expected<LivePreviewResult, LiveError> ParseLivePreviewResult(
    std::string_view json,
    base::FunctionRef<bool(const GURL&)> icon_allowed) {
  std::optional<base::Value> value =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  const base::DictValue* dict = value ? value->GetIfDict() : nullptr;
  const base::ListValue* items = dict ? dict->FindList("items") : nullptr;
  if (!items) {
    return base::unexpected(
        LiveError{"bad_result", "previewTab must return {items: [...]}"});
  }
  auto clip = [](const std::string& s, size_t max) {
    std::u16string out = base::UTF8ToUTF16(s);
    if (out.size() > max) {
      out.resize(max);
    }
    return out;
  };
  LivePreviewResult result;
  std::set<std::string, std::less<>> keys;
  for (const base::Value& item : *items) {
    if (result.items.size() == kMaxLivePreviewItems) {
      break;
    }
    const base::DictValue* d = item.GetIfDict();
    const std::string* id = d ? d->FindString("id") : nullptr;
    const std::string* title = d ? d->FindString("title") : nullptr;
    if (!id || id->empty() || id->size() > kMaxIdLength || !title ||
        title->empty() || keys.contains(*id)) {
      continue;
    }
    LivePreviewItem out;
    if (const std::string* url = d->FindString("url")) {
      GURL gurl(*url);
      if (url->size() > kMaxUrlLength || !gurl.is_valid() ||
          !gurl.SchemeIsHTTPOrHTTPS()) {
        continue;
      }
      out.url = std::move(gurl);
    }
    out.id = *id;
    out.title = clip(*title, kMaxTitleLength);
    if (const std::string* subtitle = d->FindString("subtitle")) {
      out.subtitle = clip(*subtitle, kMaxSubtitleLength);
    }
    if (const std::string* icon = d->FindString("icon")) {
      // Loading an image is a request: only to hosts the preview may reach.
      GURL gurl(*icon);
      if (icon->size() <= kMaxUrlLength && gurl.is_valid() &&
          gurl.SchemeIsHTTPOrHTTPS() && icon_allowed(gurl)) {
        out.icon = std::move(gurl);
      }
    }
    if (std::optional<double> time = d->FindDouble("time")) {
      out.time = base::Time::FromMillisecondsSinceUnixEpoch(*time);
    }
    if (const std::string* badge = d->FindString("badge")) {
      out.badge = clip(*badge, kMaxBadgeLength);
    }
    keys.insert(*id);
    result.items.push_back(std::move(out));
  }
  if (const std::string* title = dict->FindString("title")) {
    result.title = clip(*title, kMaxFolderTitleLength);
  }
  if (const std::string* footer = dict->FindString("footer")) {
    result.footer = clip(*footer, kMaxFooterLength);
  }
  if (const base::Value* count = dict->Find("count")) {
    if (count->is_bool()) {
      result.some = count->GetBool();
    } else if (std::optional<double> n = count->GetIfDouble();
               n && *n >= 0 && std::isfinite(*n)) {
      result.count = static_cast<int>(std::min(*n, 99999.0));
    }
  }
  return result;
}

}  // namespace compass
