// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_COMMAND_BAR_COMMAND_BAR_PAGE_HANDLER_H_
#define COMPASS_BROWSER_UI_WEBUI_COMMAND_BAR_COMMAND_BAR_PAGE_HANDLER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "components/omnibox/browser/autocomplete_controller.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "compass/browser/ui/webui/command_bar/command_bar.mojom.h"

class BrowserWindowInterface;
class TopChromeWebUIController;

namespace compass {

class SidebarNode;
class SpaceModel;

// What a command result does. See docs/design/command-bar.md, "Results".
enum class CommandBarCommand {
  kNewSpace,
  kPinEntry,
  kUnpinEntry,
  kAddFavorite,
  kRemoveFavorite,
  kCopyUrl,
  kReload,
  kHistory,
  kDownloads,
  kExtensions,
  kSettings,
  // Opens chrome://compass-import (docs/design/arc-import.md).
  kImportFromArc,
  // Opens chrome://compass-settings (docs/design/shortcuts.md).
  kKeyboardShortcuts,
};

// Serves the command bar page its results and acts on the chosen one. Merges
// omnibox suggestions with the profile's sidebar entries, its spaces and a
// list of commands. See docs/design/command-bar.md.
class CommandBarPageHandler : public command_bar::mojom::PageHandler,
                              public AutocompleteController::Observer {
 public:
  // |controller| is the page's, to show and close its bubble; null in tests.
  CommandBarPageHandler(
      mojo::PendingReceiver<command_bar::mojom::PageHandler> receiver,
      mojo::PendingRemote<command_bar::mojom::Page> page,
      BrowserWindowInterface* browser,
      TopChromeWebUIController* controller);
  CommandBarPageHandler(const CommandBarPageHandler&) = delete;
  CommandBarPageHandler& operator=(const CommandBarPageHandler&) = delete;
  ~CommandBarPageHandler() override;

  // Tells the page the bar is about to show, in |mode|, starting from |text|.
  void Open(command_bar::mojom::Mode mode, const std::u16string& text);

  // command_bar::mojom::PageHandler:
  void ShowUI() override;
  void Query(const std::string& text,
             bool prevent_inline_autocomplete) override;
  void Accept(uint32_t id, const std::string& text, bool new_entry) override;
  void Close() override;

  // AutocompleteController::Observer:
  void OnResultChanged(AutocompleteController* controller,
                       bool default_match_changed) override;

 private:
  // One row of the list, and what choosing it does.
  struct Item {
    Item();
    Item(Item&&);
    Item& operator=(Item&&);
    ~Item();

    uint32_t id = 0;
    command_bar::mojom::ResultKind kind;
    std::u16string title;
    std::u16string subtitle;
    std::string action;
    GURL icon_url;
    // Omnibox suggestions: the match and its line in the controller's result.
    std::optional<AutocompleteMatch> match;
    size_t match_line = 0;
    // Entries and spaces. A favorite has no space.
    base::Uuid node_id;
    base::Uuid space_id;
    bool favorite = false;
    CommandBarCommand command = CommandBarCommand::kNewSpace;
  };

  // Rebuilds the list from the controller's result and the local results
  // for |text_|, and sends it.
  void Rebuild();
  Item MatchItem(const AutocompleteMatch& match, size_t line);
  void AddEntryItems(std::vector<Item>& items);
  void AddRecentTodayItems(std::vector<Item>& items);
  void AddSpaceItems(std::vector<Item>& items);
  void AddCommandItems(std::vector<Item>& items);
  Item EntryItem(const SidebarNode& node, const SpaceModel& space);
  // Gives |item| the ID it had earlier in this query, or a new one.
  void AssignId(Item& item, const std::string& key);

  // |line| is the match's line in the controller's result, for the omnibox
  // log; none for a match that isn't from it.
  void AcceptMatch(const AutocompleteMatch& match,
                   std::optional<size_t> line,
                   bool new_entry);
  // Reports the opening of |match| to the omnibox's listeners, as
  // OmniboxEditModel::OpenMatch() does.
  void LogMatch(const AutocompleteMatch& match,
                size_t line,
                WindowOpenDisposition disposition);
  void AcceptEntry(const Item& item);
  void AcceptSpace(const base::Uuid& space_id);
  void RunCommand(CommandBarCommand command);
  bool IsCommandAvailable(CommandBarCommand command) const;

  // The sidebar node of the window's active tab, or null.
  SidebarNode* GetActiveNode() const;
  // Whether |node| (or the split or folder holding it) is in Pinned.
  bool IsPinned(const SidebarNode& node) const;
  bool IsFavorite(const SidebarNode& node) const;

  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<TopChromeWebUIController> controller_;
  mojo::Receiver<command_bar::mojom::PageHandler> receiver_;
  mojo::Remote<command_bar::mojom::Page> page_;
  std::unique_ptr<AutocompleteController> autocomplete_;

  command_bar::mojom::Mode mode_ = command_bar::mojom::Mode::kNewEntry;
  std::u16string text_;
  // Whether the latest query followed a deletion.
  bool just_deleted_ = false;
  // When the user first changed the text since the bar opened, for the
  // omnibox log; null if they haven't.
  base::TimeTicks first_edit_time_;
  std::vector<Item> items_;
  // IDs given out in this query, by result.
  std::map<std::string, uint32_t> ids_;
  uint32_t next_id_ = 1;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_COMMAND_BAR_COMMAND_BAR_PAGE_HANDLER_H_
