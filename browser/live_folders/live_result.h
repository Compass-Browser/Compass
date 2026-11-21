// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_LIVE_FOLDERS_LIVE_RESULT_H_
#define COMPASS_BROWSER_LIVE_FOLDERS_LIVE_RESULT_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/function_ref.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "url/gurl.h"
#include "compass/browser/live_folders/live_fetcher.h"
#include "compass/browser/sidebar/space_model.h"

namespace compass {

// A refresh's result as the browser takes it: the page normalized it with
// runtime/result.js, but the page is untrusted, so the rules that matter to
// the sidebar are checked again here (docs/design/live-folders.md,
// "Result").
struct LiveResult {
  LiveResult();
  LiveResult(LiveResult&&);
  LiveResult& operator=(LiveResult&&);
  ~LiveResult();

  std::vector<LiveItem> items;
  // A name for a folder created without one.
  std::u16string title;
  // When the plugin wants to be asked again; the caller clamps it.
  std::optional<base::TimeDelta> next_refresh;
};

inline constexpr size_t kMaxLiveItems = 50;

// Items that break a rule are dropped; JSON that isn't {items: [...]} is a
// bad_result error.
base::expected<LiveResult, LiveError> ParseLiveResult(std::string_view json);

// One row of a tab preview (docs/design/live-folders.md, "Tab previews").
struct LivePreviewItem {
  LivePreviewItem();
  LivePreviewItem(const LivePreviewItem&);
  LivePreviewItem& operator=(const LivePreviewItem&);
  LivePreviewItem(LivePreviewItem&&);
  LivePreviewItem& operator=(LivePreviewItem&&);
  ~LivePreviewItem();

  std::string id;
  std::u16string title;
  // Empty: a click shows the tab.
  GURL url;
  std::u16string subtitle;
  // Empty, or on a host the preview may load images from.
  GURL icon;
  std::optional<base::Time> time;
  std::u16string badge;
};

// A tab preview's result, checked again outside the untrusted page.
struct LivePreviewResult {
  LivePreviewResult();
  LivePreviewResult(const LivePreviewResult&);
  LivePreviewResult& operator=(const LivePreviewResult&);
  LivePreviewResult(LivePreviewResult&&);
  LivePreviewResult& operator=(LivePreviewResult&&);
  ~LivePreviewResult();

  // Nothing to show: the card keeps its thumbnail.
  bool empty() const { return items.empty() && footer.empty(); }

  std::vector<LivePreviewItem> items;
  // A header over the rows ("Coming up").
  std::u16string title;
  std::u16string footer;
  // The tab row's badge: how many (0 for none), or some without a number.
  int count = 0;
  bool some = false;
};

inline constexpr size_t kMaxLivePreviewItems = 10;

// Like ParseLiveResult() for previews: at most 10 items, `url` optional,
// a header and a footer, and icons only where |icon_allowed| says.
base::expected<LivePreviewResult, LiveError> ParseLivePreviewResult(
    std::string_view json,
    base::FunctionRef<bool(const GURL&)> icon_allowed);

}  // namespace compass

#endif  // COMPASS_BROWSER_LIVE_FOLDERS_LIVE_RESULT_H_
