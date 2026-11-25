// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_APP_SIDEBAR_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_APP_SIDEBAR_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "base/values.h"
#include "compass/browser/browser_import/source.h"
#include "url/gurl.h"

// What the readers of Arc's successors share (Ardia, Cra, Crest, Yab:
// docs/design/browser-import.md): each keeps its sidebar in a file of its
// own, in JSON, with spaces, pinned tabs and folders much like Arc's.
namespace compass::browser_import {

// The JSON in the file at |path|, or nullopt if there's none or it isn't
// JSON. Blocking.
std::optional<base::Value> ReadJsonFile(const base::FilePath& path);
// The same for JSON already read.
std::optional<base::Value> ParseJson(std::string_view json);

// |dict|'s string at |key|, or empty.
std::string StringAt(const base::DictValue& dict, std::string_view key);

// A space color for a CSS-style hex color ("#2861B3", "#2861B3FF"), as
// arc::ArcSpaceColor() takes it: grey for an unsaturated one. nullopt if
// it isn't one.
std::optional<SpaceColor> HexSpaceColor(std::string_view hex);

// A space's icon: |emoji| if it's one, else a Compass symbol like the SF
// Symbol |symbol| (arc::ArcSymbolToCompass(), which also knows SF Symbols'
// base names: "briefcase.fill" is "briefcase"), else none.
SpaceIcon AppSpaceIcon(std::string_view emoji, std::string_view symbol);

// A time written as ISO 8601 ("2026-10-30T09:00:00Z"), or a null time.
base::Time TimeFromIso8601(std::string_view text);

// Whether a tab at |url| comes along: a page Compass can open again, not
// a blank or internal one.
bool IsImportableTabURL(const GURL& url);

// |entries| with one entry per URL (visits added up, the latest visit
// kept), the |max_entries| most recently visited.
std::vector<HistoryEntry> MergeHistory(std::vector<HistoryEntry> entries,
                                       size_t max_entries);

// A folder on the way to an entry, from the top down.
struct FolderStep {
  // The browser's ID for it, unique in the tree.
  std::string id;
  std::u16string name;
};

// Builds a tree of Pinned or Today from entries in order, each with the
// folders it's in: a folder sits where its first entry comes, and keeps
// its entries in order. Folders left empty are left out.
class FolderTreeBuilder {
 public:
  // Folder IDs: |id_prefix| + "folder:" + the browser's ID.
  explicit FolderTreeBuilder(std::string id_prefix);
  FolderTreeBuilder(const FolderTreeBuilder&) = delete;
  FolderTreeBuilder& operator=(const FolderTreeBuilder&) = delete;
  ~FolderTreeBuilder();

  void Add(const std::vector<FolderStep>& path, arc::Item item);
  std::vector<arc::Item> Build() &&;

 private:
  struct Node;

  std::string id_prefix_;
  std::unique_ptr<Node> root_;
};

// One of Crest's or Yab's profiles: the sign-ins some of its spaces share,
// a Chromium profile of their own.
struct AppProfile {
  AppProfile();
  AppProfile(const AppProfile&);
  AppProfile& operator=(const AppProfile&);
  ~AppProfile();

  // Its UUID, upper case.
  std::string key;
  // Its spaces' names, in the browser's order.
  std::vector<std::u16string> spaces;
};

// A name for a profile that has none of its own, from its spaces' names:
// "Work, Side", or "Work, Side, Home…" for more than three.
std::u16string AppProfileName(const std::vector<std::u16string>& spaces);

// Adds |bookmarks| as one more space, "<Browser>" (SpaceNameFor()), when
// there are any, or when |sidebar| has no space at all (so the profile
// still shows as one). As Zen's bookmarks come in.
void AddBookmarksSpace(const SourceProfile& profile,
                       std::vector<arc::Item> bookmarks,
                       ImportedSidebar& sidebar);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_APP_SIDEBAR_H_
