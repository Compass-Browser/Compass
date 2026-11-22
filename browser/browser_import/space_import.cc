// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/space_import.h"

#include <optional>
#include <set>

#include "compass/browser/arc_import/sidebar_import.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"

namespace compass::browser_import {

namespace {

// The palette's colors (grey aside) no space of |list| has, in order; all
// of them again once each is taken.
class ColorPicker {
 public:
  explicit ColorPicker(const SpaceList& list) {
    for (const auto& space : list.spaces()) {
      used_.insert(space->color().tint().value_or(0));
    }
  }

  SpaceColor Next() {
    for (int pass = 0; pass < 2; ++pass) {
      for (const NamedSpaceColor& named : kSpaceColorPalette) {
        if (named.color.is_grey()) {
          continue;
        }
        const SkColor tint = *named.color.tint();
        if (!used_.contains(tint)) {
          used_.insert(tint);
          return named.color;
        }
      }
      // All taken: start over.
      used_.clear();
    }
    return SpaceColor::kBlue;
  }

 private:
  std::set<SkColor> used_;
};

void CollectUrls(const SidebarNode* node, std::set<GURL>& urls) {
  for (const auto& child : node->children()) {
    if (child->type() == SidebarNode::Type::kTab) {
      urls.insert(child->url());
    }
    CollectUrls(child.get(), urls);
  }
}

// A profile just made: one space, nothing in it.
bool IsUnused(const SpaceList& list) {
  return list.spaces().size() == 1 &&
         list.spaces()[0]->pinned_root()->children().empty() &&
         list.spaces()[0]->today_root()->children().empty();
}

constexpr char kNewProfilePrefix[] = "profile/";

void NamespaceItems(std::vector<arc::Item>& items) {
  for (arc::Item& item : items) {
    if (!item.id.empty()) {
      item.id = kNewProfilePrefix + item.id;
    }
    NamespaceItems(item.children);
  }
}

}  // namespace

AddedSidebar::AddedSidebar() = default;
AddedSidebar::AddedSidebar(AddedSidebar&&) = default;
AddedSidebar& AddedSidebar::operator=(AddedSidebar&&) = default;
AddedSidebar::~AddedSidebar() = default;

bool IsImported(const SpaceList& list, const arc::Space& space) {
  const base::Uuid id = arc::ArcDerivedId(space.id);
  return id.is_valid() && list.FindSpace(id);
}

AddedSidebar AddImportedSidebar(SpaceList& list,
                                const ImportedSidebar& sidebar,
                                base::Time now,
                                bool into_new_profile) {
  AddedSidebar added;
  bool reuse_first = into_new_profile && IsUnused(list);
  ColorPicker colors(list);
  for (const arc::Space& space : sidebar.spaces) {
    if (IsImported(list, space)) {
      continue;
    }
    SpaceModel* model = nullptr;
    if (reuse_first) {
      model = list.spaces()[0].get();
      model->SetName(space.name);
      reuse_first = false;
    } else {
      const base::Uuid derived = arc::ArcDerivedId(space.id);
      model =
          list.AddSpace(list.spaces().size(), space.name,
                        derived.is_valid() ? std::optional<base::Uuid>(derived)
                                           : std::nullopt);
    }
    if (!model) {
      continue;
    }
    if (space.color.is_grey() && space.gradient.empty()) {
      model->SetColor(colors.Next());
    } else {
      model->SetColor(space.color);
      model->SetGradient(space.gradient);
    }
    model->SetIcon(space.icon);
    arc::AddItems(list, *model, model->pinned_root(), space.pinned,
                  std::nullopt);
    arc::AddItems(list, *model, model->today_root(), space.today, now);
    added.pinned += CountTabs(space.pinned);
    added.today += CountTabs(space.today);
    added.spaces.push_back(model);
  }

  SpaceModel& favorites = list.favorites();
  std::set<GURL> have;
  CollectUrls(favorites.pinned_root(), have);
  std::vector<arc::Item> missing;
  for (const arc::Item& item : sidebar.favorites) {
    if (item.kind == arc::Item::Kind::kTab && !have.contains(item.url)) {
      have.insert(item.url);
      missing.push_back(item);
    }
  }
  arc::AddItems(list, favorites, favorites.pinned_root(), missing,
                std::nullopt);
  added.favorites = static_cast<int>(missing.size());
  return added;
}

ImportedSidebar ForNewProfile(ImportedSidebar sidebar) {
  for (arc::Space& space : sidebar.spaces) {
    space.id = kNewProfilePrefix + space.id;
    NamespaceItems(space.pinned);
    NamespaceItems(space.today);
  }
  NamespaceItems(sidebar.favorites);
  return sidebar;
}

}  // namespace compass::browser_import
