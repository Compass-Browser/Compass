// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/sidebar_node.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"

namespace compass {

LiveSource::LiveSource() = default;

LiveSource::LiveSource(std::string plugin_id,
                       base::DictValue config,
                       GURL plugin_url,
                       std::string plugin_sha256)
    : plugin_id(std::move(plugin_id)),
      config(std::move(config)),
      plugin_url(std::move(plugin_url)),
      plugin_sha256(std::move(plugin_sha256)) {}

LiveSource::LiveSource(const LiveSource& other)
    : plugin_id(other.plugin_id),
      config(other.config.Clone()),
      plugin_url(other.plugin_url),
      plugin_sha256(other.plugin_sha256) {}

LiveSource& LiveSource::operator=(const LiveSource& other) {
  if (this != &other) {
    plugin_id = other.plugin_id;
    config = other.config.Clone();
    plugin_url = other.plugin_url;
    plugin_sha256 = other.plugin_sha256;
  }
  return *this;
}

LiveSource::LiveSource(LiveSource&&) = default;
LiveSource& LiveSource::operator=(LiveSource&&) = default;
LiveSource::~LiveSource() = default;

SidebarNode::SidebarNode(Type type,
                         std::u16string title,
                         GURL url,
                         const base::Uuid& id)
    : id_(id.is_valid() ? id : base::Uuid::GenerateRandomV4()),
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

}  // namespace compass
