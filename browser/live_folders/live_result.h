// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_LIVE_FOLDERS_LIVE_RESULT_H_
#define COMPASS_BROWSER_LIVE_FOLDERS_LIVE_RESULT_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "base/types/expected.h"
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

}  // namespace compass

#endif  // COMPASS_BROWSER_LIVE_FOLDERS_LIVE_RESULT_H_
