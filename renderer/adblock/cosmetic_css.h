// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_RENDERER_ADBLOCK_COSMETIC_CSS_H_
#define COMPASS_RENDERER_ADBLOCK_COSMETIC_CSS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"

namespace subresource_filter::flat {
struct StyleRuleIndex;
}  // namespace subresource_filter::flat

// The ad blocker's element hiding CSS (docs/design/ad-blocker.md "Element
// hiding"), from the style rules upstream indexes with the network rules.
namespace compass::adblock {

// How many selectors share a CSS rule: one the page's CSS parser doesn't
// take drops only its group.
inline constexpr size_t kSelectorsPerRule = 20;

// The selectors of the rules for every site (the style index's class and
// id maps), each once, as indices into the index's selectors or, for rules
// whose selector is the class or id itself, the name.
struct GenericSelectors {
  GenericSelectors();
  GenericSelectors(GenericSelectors&&);
  GenericSelectors& operator=(GenericSelectors&&);
  ~GenericSelectors();

  // Selectors with an index, which exceptions can name.
  std::vector<std::pair<uint16_t, std::string_view>> indexed;
  // Selectors that are the class or id itself: no exception names them
  // (upstream's builder files an exception's selector first, so it gets an
  // index).
  std::vector<std::string_view> implicit;
};

GenericSelectors CollectGenericSelectors(
    const subresource_filter::flat::StyleRuleIndex* index);

// The indices of the selectors `host` (and its parent domains) has
// exceptions for ("example.com#@#.ad").
std::vector<uint16_t> GetExcludedIndices(
    const subresource_filter::flat::StyleRuleIndex* index,
    std::string_view host);

// A stylesheet hiding what `selectors` match, kSelectorsPerRule a rule,
// leaving out `excluded` (sorted).
std::string BuildHidingCss(const GenericSelectors& selectors,
                           base::span<const uint16_t> excluded);
std::string BuildHidingCss(base::span<const std::string_view> selectors);

}  // namespace compass::adblock

#endif  // COMPASS_RENDERER_ADBLOCK_COSMETIC_CSS_H_
