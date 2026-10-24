// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/sidebar_node.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"

namespace strata {

SidebarNode::SidebarNode(Type type, std::u16string title, GURL url)
    : id_(base::Uuid::GenerateRandomV4()),
      type_(type),
      title_(std::move(title)),
      url_(std::move(url)) {}

SidebarNode::~SidebarNode() = default;

size_t SidebarNode::GetIndexOf(const SidebarNode* child) const {
  auto it = std::ranges::find_if(
      children_, [child](const auto& node) { return node.get() == child; });
  CHECK(it != children_.end());
  return static_cast<size_t>(std::distance(children_.begin(), it));
}

bool SidebarNode::Contains(const SidebarNode* node) const {
  for (const SidebarNode* n = node; n; n = n->parent()) {
    if (n == this) {
      return true;
    }
  }
  return false;
}

SidebarNode* SidebarNode::Add(std::unique_ptr<SidebarNode> child,
                              size_t index) {
  CHECK_LE(index, children_.size());
  child->parent_ = this;
  auto it = children_.insert(
      children_.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));
  return it->get();
}

std::unique_ptr<SidebarNode> SidebarNode::Remove(const SidebarNode* child) {
  auto it = children_.begin() +
            static_cast<std::ptrdiff_t>(GetIndexOf(child));
  std::unique_ptr<SidebarNode> removed = std::move(*it);
  children_.erase(it);
  removed->parent_ = nullptr;
  return removed;
}

}  // namespace strata
