// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/renderer/adblock/cosmetic_css.h"

#include <algorithm>
#include <set>

#include "components/subresource_filter/core/common/flat/indexed_ruleset_generated.h"
#include "components/subresource_filter/core/common/style_rule_indexer.h"

namespace compass::adblock {

namespace {

namespace flat = subresource_filter::flat;
using NameMap = flatbuffers::Vector<flatbuffers::Offset<flat::NameToSelectors>>;

constexpr uint16_t kImplicit =
    subresource_filter::StyleRuleIndexer::kImplicitStyleRuleIndex;
constexpr std::string_view kHide = "{display:none!important}\n";

void ForEachIndex(const flat::NameToSelectors* entry, auto callback) {
  callback(entry->first_index());
  if (entry->more_indices()) {
    for (uint16_t index : *entry->more_indices()) {
      callback(index);
    }
  }
}

const flat::NameToSelectors* Find(const NameMap* map, std::string_view name) {
  if (!map) {
    return nullptr;
  }
  auto it = std::ranges::lower_bound(
      *map, name, {}, [](const flat::NameToSelectors* entry) {
        return entry->name()->string_view();
      });
  return it != map->end() && (*it)->name()->string_view() == name ? *it
                                                                   : nullptr;
}

void AppendRules(std::string& css, auto& selectors) {
  size_t in_rule = 0;
  for (std::string_view selector : selectors) {
    if (in_rule) {
      css += ',';
    }
    css += selector;
    if (++in_rule == kSelectorsPerRule) {
      css += kHide;
      in_rule = 0;
    }
  }
  if (in_rule) {
    css += kHide;
  }
}

}  // namespace

GenericSelectors::GenericSelectors() = default;
GenericSelectors::GenericSelectors(GenericSelectors&&) = default;
GenericSelectors& GenericSelectors::operator=(GenericSelectors&&) = default;
GenericSelectors::~GenericSelectors() = default;

GenericSelectors CollectGenericSelectors(const flat::StyleRuleIndex* index) {
  GenericSelectors result;
  if (!index) {
    return result;
  }
  const auto* selectors = index->selectors();
  std::set<uint16_t> seen;
  for (const NameMap* map : {index->class_map(), index->id_map()}) {
    if (!map) {
      continue;
    }
    for (const flat::NameToSelectors* entry : *map) {
      ForEachIndex(entry, [&](uint16_t rule) {
        if (rule == kImplicit) {
          result.implicit.push_back(entry->name()->string_view());
        } else if (selectors && rule < selectors->size() &&
                   seen.insert(rule).second) {
          result.indexed.emplace_back(rule,
                                      selectors->Get(rule)->string_view());
        }
      });
    }
  }
  return result;
}

std::vector<uint16_t> GetExcludedIndices(const flat::StyleRuleIndex* index,
                                         std::string_view host) {
  std::vector<uint16_t> excluded;
  if (!index || !index->exclusion_map()) {
    return excluded;
  }
  // "a.b.example" takes the exceptions for "a.b.example", "b.example" and
  // "example", as upstream's StyleRuleMatcher does.
  for (std::string_view domain = host; !domain.empty();) {
    if (const flat::NameToSelectors* entry =
            Find(index->exclusion_map(), domain)) {
      ForEachIndex(entry, [&](uint16_t rule) { excluded.push_back(rule); });
    }
    size_t dot = domain.find('.');
    if (dot == std::string_view::npos) {
      break;
    }
    domain.remove_prefix(dot + 1);
  }
  std::ranges::sort(excluded);
  return excluded;
}

std::string BuildHidingCss(const GenericSelectors& selectors,
                           base::span<const uint16_t> excluded) {
  std::vector<std::string_view> kept;
  kept.reserve(selectors.indexed.size() + selectors.implicit.size());
  for (const auto& [rule, selector] : selectors.indexed) {
    if (!std::ranges::binary_search(excluded, rule)) {
      kept.push_back(selector);
    }
  }
  kept.insert(kept.end(), selectors.implicit.begin(),
              selectors.implicit.end());
  return BuildHidingCss(kept);
}

std::string BuildHidingCss(base::span<const std::string_view> selectors) {
  std::string css;
  AppendRules(css, selectors);
  return css;
}

}  // namespace compass::adblock
