// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/space_export.h"

#include <string_view>

#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "url/gurl.h"

namespace compass {

namespace {

// What upstream's bookmark_html_writer writes, which every importer reads.
constexpr char kHeader[] =
    "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
    "<!-- This is an automatically generated file.\n"
    "     It will be read and overwritten.\n"
    "     DO NOT EDIT! -->\n"
    "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
    "<TITLE>Bookmarks</TITLE>\n"
    "<H1>Bookmarks</H1>\n"
    "<DL><p>\n";

void Indent(std::string& out, int depth) {
  out.append(static_cast<size_t>(depth) * 4, ' ');
}

void OpenFolder(std::string& out, int depth, std::u16string_view name) {
  Indent(out, depth);
  base::StrAppend(
      &out,
      {"<DT><H3>", base::EscapeForHTML(base::UTF16ToUTF8(name)), "</H3>\n"});
  Indent(out, depth);
  out.append("<DL><p>\n");
}

void CloseFolder(std::string& out, int depth) {
  Indent(out, depth);
  out.append("</DL><p>\n");
}

void WriteLink(std::string& out,
               int depth,
               const GURL& url,
               std::u16string_view title) {
  // Only what another browser can open: no chrome:// or empty tabs.
  if (!url.is_valid() || !(url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile() ||
                           url.SchemeIs("ftp"))) {
    return;
  }
  const std::string spec = url.spec();
  const std::string text = title.empty() ? spec : base::UTF16ToUTF8(title);
  Indent(out, depth);
  base::StrAppend(&out, {"<DT><A HREF=\"", base::EscapeForHTML(spec), "\">",
                         base::EscapeForHTML(text), "</A>\n"});
}

void WriteTab(std::string& out, int depth, const SidebarNode& tab) {
  // A pinned tab or favorite is its own page; an open tab, the one it shows.
  if (!tab.saved_url().is_empty()) {
    const std::u16string& title =
        !tab.custom_title().empty()  ? tab.custom_title()
        : !tab.saved_title().empty() ? tab.saved_title()
                                     : tab.title();
    WriteLink(out, depth, tab.saved_url(), title);
    return;
  }
  WriteLink(out, depth, tab.url(), tab.display_title());
}

void WriteChildren(std::string& out, int depth, const SidebarNode& parent) {
  for (const auto& child : parent.children()) {
    switch (child->type()) {
      case SidebarNode::Type::kTab:
        WriteTab(out, depth, *child);
        break;
      case SidebarNode::Type::kSplit:
        // A split's panes, side by side in Compass, are links in a row.
        WriteChildren(out, depth, *child);
        break;
      case SidebarNode::Type::kFolder:
        OpenFolder(out, depth, child->title());
        WriteChildren(out, depth + 1, *child);
        CloseFolder(out, depth);
        break;
      case SidebarNode::Type::kRoot:
        break;
    }
  }
}

}  // namespace

SpaceExport::SpaceExport() = default;
SpaceExport::SpaceExport(const SpaceExport&) = default;
SpaceExport& SpaceExport::operator=(const SpaceExport&) = default;
SpaceExport::~SpaceExport() = default;

std::string SpacesToBookmarksHtml(const SpaceExport& spaces) {
  std::string out = kHeader;
  for (const SpaceModel* favorites : spaces.favorites) {
    if (favorites && !favorites->pinned_root()->children().empty()) {
      OpenFolder(out, 1, u"Favorites");
      WriteChildren(out, 2, *favorites->pinned_root());
      CloseFolder(out, 1);
    }
  }
  for (const SpaceModel* space : spaces.spaces) {
    if (!space) {
      continue;
    }
    OpenFolder(out, 1, space->name());
    WriteChildren(out, 2, *space->pinned_root());
    if (!space->today_root()->children().empty()) {
      OpenFolder(out, 2, u"Today");
      WriteChildren(out, 3, *space->today_root());
      CloseFolder(out, 2);
    }
    CloseFolder(out, 1);
  }
  out.append("</DL><p>\n");
  return out;
}

}  // namespace compass
