// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/sidebar_import.h"

#include <array>
#include <string>

#include "base/containers/span.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "crypto/sha2.h"

namespace compass::arc {

namespace {

// A tab's title before its page loads: Arc's saved title, else its host.
std::u16string TabTitle(const Item& item) {
  if (!item.saved_title.empty()) {
    return item.saved_title;
  }
  return base::UTF8ToUTF16(item.url.host());
}

// Whether no space, entry or favorite of |list| has |id|.
bool IsFree(SpaceList& list, const base::Uuid& id) {
  if (list.FindSpace(id) || list.favorites().FindById(id)) {
    return false;
  }
  for (const auto& space : list.spaces()) {
    if (space->FindById(id)) {
      return false;
    }
  }
  return true;
}

class Builder {
 public:
  Builder(SpaceList& list,
          SpaceModel& model,
          std::optional<base::Time> last_active)
      : list_(list), model_(model), last_active_(last_active) {}

  // Adds |items| at the end of |parent|.
  void AddAll(SidebarNode* parent, const std::vector<Item>& items) {
    for (const Item& item : items) {
      Add(parent, item);
    }
  }

 private:
  // Arc's item's derived ID, or none (a fresh one) if it's taken.
  base::Uuid IdFor(const Item& item) {
    const base::Uuid id = ArcDerivedId(item.id);
    return id.is_valid() && IsFree(*list_, id) ? id : base::Uuid();
  }

  // Model::Split() of two tabs, the split getting |item|'s derived ID.
  SidebarNode* SplitAs(const Item& item,
                       SidebarNode* first,
                       SidebarNode* tab) {
    const base::Uuid id = IdFor(item);
    if (!id.is_valid()) {
      return model_->Split(first, tab);
    }
    SidebarNode* parent = first->parent();
    SidebarNode* split =
        model_->AddSplit(parent, parent->GetIndexOf(first), id);
    if (!split) {
      return model_->Split(first, tab);
    }
    model_->Move(first, split, 0);
    model_->Move(tab, split, 1);
    return split;
  }

  SidebarNode* AddTab(SidebarNode* parent, const Item& item) {
    SidebarNode* tab = model_->AddTab(parent, parent->children().size(),
                                      item.url, TabTitle(item), IdFor(item));
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
            item.title.empty() ? u"Folder" : item.title, IdFor(item));
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
            first = first->type() == SidebarNode::Type::kSplit
                        ? model_->Split(first, tab)
                        : SplitAs(item, first, tab);
          }
        }
        return;
      }
    }
  }

  const raw_ref<SpaceList> list_;
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
      // The derived ID, unless a space has it (a second import).
      std::optional<base::Uuid> id;
      if (const base::Uuid derived = ArcDerivedId(space.id);
          derived.is_valid() && IsFree(list, derived)) {
        id = derived;
      }
      model = list.AddSpace(list.spaces().size(), space.name, id);
    }
    if (!model) {
      continue;
    }
    model->SetColor(space.color);
    Builder(list, *model, std::nullopt)
        .AddAll(model->pinned_root(), space.pinned);
    Builder(list, *model, now).AddAll(model->today_root(), space.today);
    added.push_back(model);
  }
  SpaceModel& favorites = list.favorites();
  Builder(list, favorites, std::nullopt)
      .AddAll(favorites.pinned_root(), profile.favorites);
  return added;
}

base::Uuid ArcDerivedId(std::string_view arc_id) {
  if (arc_id.empty()) {
    return base::Uuid();
  }
  const std::string name = base::StrCat({"compass-arc-import:", arc_id});
  std::array<uint8_t, crypto::kSHA256Length> hash =
      crypto::SHA256Hash(base::as_byte_span(name));
  hash[6] = (hash[6] & 0x0f) | 0x80;  // Version 8.
  hash[8] = (hash[8] & 0x3f) | 0x80;  // Variant 10.
  const std::string hex =
      base::HexEncodeLower(base::span(hash).first<16>());
  return base::Uuid::ParseLowercase(base::StrCat(
      {std::string_view(hex).substr(0, 8), "-",
       std::string_view(hex).substr(8, 4), "-",
       std::string_view(hex).substr(12, 4), "-",
       std::string_view(hex).substr(16, 4), "-",
       std::string_view(hex).substr(20, 12)}));
}

}  // namespace compass::arc
