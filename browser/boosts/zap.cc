// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/boosts/zap.h"

#include <algorithm>

#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"

namespace compass::boosts {

namespace {

constexpr char kRuleEnd[] = " { display: none !important; }";

bool IsUsableSelector(std::string_view selector) {
  return !selector.empty() &&
         selector.find_first_of("{}") == std::string_view::npos &&
         selector.find("/*") == std::string_view::npos &&
         selector.find("*/") == std::string_view::npos &&
         selector.find('\n') == std::string_view::npos;
}

}  // namespace

bool IsZapBoost(const Boost& boost) {
  return base::StartsWith(boost.css, kZapMarker);
}

const Boost* FindZapBoost(const BoostStore& store, std::string_view match) {
  for (const Boost& boost : store.boosts()) {
    if (boost.match == match && IsZapBoost(boost)) {
      return &boost;
    }
  }
  return nullptr;
}

std::vector<std::string> ZappedSelectors(const Boost& boost) {
  std::vector<std::string> selectors;
  for (std::string_view line : base::SplitStringPiece(
           boost.css, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (line.ends_with(kRuleEnd)) {
      line.remove_suffix(std::string_view(kRuleEnd).size());
      selectors.emplace_back(line);
    }
  }
  return selectors;
}

Boost AddZappedSelectors(const Boost* existing,
                         std::string_view match,
                         const std::vector<std::string>& selectors) {
  Boost boost;
  std::vector<std::string> all;
  if (existing) {
    boost = *existing;
    all = ZappedSelectors(*existing);
  } else {
    boost.match = std::string(match);
    boost.name = base::StrCat({"Zapped on ", match});
    boost.author = "user";
  }
  for (const std::string& raw : selectors) {
    const std::string selector(base::TrimWhitespaceASCII(raw, base::TRIM_ALL));
    if (IsUsableSelector(selector) && !std::ranges::contains(all, selector)) {
      all.push_back(selector);
    }
  }
  std::string css = base::StrCat({kZapMarker, "\n"});
  for (const std::string& selector : all) {
    base::StrAppend(&css, {selector, kRuleEnd, "\n"});
  }
  boost.css = std::move(css);
  boost.enabled = true;
  return boost;
}

}  // namespace compass::boosts
