// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/sidebar_import.h"

#include "base/strings/utf_string_conversions.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"

namespace compass::arc {

namespace {

// A tab's title before its page loads: Arc's saved title, else its host.
std::u16string TabTitle(const Item& item) {
  if (!item.saved_title.empty()) {
    return item.saved_title;
  }
  return base::UTF8ToUTF16(item.url.host());
}

class Builder {
 public:
  Builder(SpaceModel& model, std::optional<base::Time> last_active)
      : model_(model), last_active_(last_active) {}

  // Adds |items| at the end of |parent|.
  void AddAll(SidebarNode* parent, const std::vector<Item>& items) {
    for (const Item& item : items) {
      Add(parent, item);
    }
  }

 private:
  SidebarNode* AddTab(SidebarNode* parent, const Item& item) {
    SidebarNode* tab = model_->AddTab(parent, parent->children().size(),
                                      item.url, TabTitle(item));
    if (!tab) {
      return nullptr;
    }
    if (!item.title.empty()) {
      model_->SetCustomTitle(tab, item.title);
    }
    const base::Time last_active = last_active_.value_or(item.last_active);
    if (!last_active.is_null()) {
      model_->SetLastActive(tab, last_active);
    }
    return tab;
  }

  void Add(SidebarNode* parent, const Item& item) {
    switch (item.kind) {
      case Item::Kind::kTab:
        AddTab(parent, item);
        return;
      case Item::Kind::kFolder: {
        // Favorites hold tabs only: a folder's entries go in its place.
        if (model_->kind() == SpaceModel::Kind::kFavorites) {
          AddAll(parent, item.children);
          return;
        }
        SidebarNode* folder = model_->AddFolder(
            parent, parent->children().size(),
            item.title.empty() ? u"Folder" : item.title);
        if (folder) {
          AddAll(folder, item.children);
        }
        return;
      }
      case Item::Kind::kSplit: {
        if (model_->kind() == SpaceModel::Kind::kFavorites) {
          AddAll(parent, item.children);
          return;
        }
        // Its tabs, then as many of them as a split takes, side by side.
        SidebarNode* first = nullptr;
        for (const Item& child : item.children) {
          if (child.kind != Item::Kind::kTab) {
            Add(parent, child);
            continue;
          }
          SidebarNode* tab = AddTab(parent, child);
          if (!tab) {
            continue;
          }
          if (!first) {
            first = tab;
          } else if (model_->CanSplit(first, tab)) {
            first = model_->Split(first, tab);
          }
        }
        return;
      }
    }
  }

  const raw_ref<SpaceModel> model_;
  // Overrides the items' own last active times.
  const std::optional<base::Time> last_active_;
};

// A profile's untouched starting space.
bool IsUnused(const SpaceList& list) {
  return list.spaces().size() == 1 &&
         list.spaces()[0]->pinned_root()->children().empty() &&
         list.spaces()[0]->today_root()->children().empty();
}

}  // namespace

std::vector<SpaceModel*> AddArcSidebar(SpaceList& list,
                                       const Profile& profile,
                                       const std::vector<Space>& spaces,
                                       base::Time now) {
  std::vector<SpaceModel*> added;
  bool reuse_first = IsUnused(list);
  for (const Space& space : spaces) {
    SpaceModel* model = nullptr;
    if (reuse_first) {
      model = list.spaces()[0].get();
      model->SetName(space.name);
      reuse_first = false;
    } else {
      model = list.AddSpace(list.spaces().size(), space.name);
    }
    if (!model) {
      continue;
    }
    model->SetColor(space.color);
    model->SetGradient(space.gradient);
    model->SetIcon(space.icon);
    Builder(*model, std::nullopt).AddAll(model->pinned_root(), space.pinned);
    Builder(*model, now).AddAll(model->today_root(), space.today);
    added.push_back(model);
  }
  SpaceModel& favorites = list.favorites();
  Builder(favorites, std::nullopt)
      .AddAll(favorites.pinned_root(), profile.favorites);
  return added;
}

}  // namespace compass::arc
