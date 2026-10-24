// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/command_bar/command_bar_page_handler.h"

#include <algorithm>
#include <utility>

#include "base/i18n/case_conversion.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "chrome/browser/autocomplete/chrome_autocomplete_provider_client.h"
#include "chrome/browser/autocomplete/chrome_autocomplete_scheme_classifier.h"
#include "chrome/browser/autocomplete/shortcuts_backend_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "components/omnibox/browser/autocomplete_classifier.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/autocomplete_result.h"
#include "components/omnibox/browser/shortcuts_backend.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/sidebar/sidebar_node.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/window_features.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace strata {

namespace {

using command_bar::mojom::Mode;
using command_bar::mojom::ResultKind;

// How many entries a query shows, and how many recent Today entries an empty
// query in new-entry mode shows.
constexpr size_t kMaxEntries = 5;
constexpr size_t kMaxRecentEntries = 8;

struct CommandInfo {
  CommandBarCommand command;
  const char16_t* name;
};

constexpr CommandInfo kCommands[] = {
    {CommandBarCommand::kNewSpace, u"New space"},
    {CommandBarCommand::kPinEntry, u"Pin tab"},
    {CommandBarCommand::kUnpinEntry, u"Unpin tab"},
    {CommandBarCommand::kCopyUrl, u"Copy URL"},
    {CommandBarCommand::kReload, u"Reload"},
    {CommandBarCommand::kHistory, u"History"},
    {CommandBarCommand::kDownloads, u"Downloads"},
    {CommandBarCommand::kExtensions, u"Extensions"},
    {CommandBarCommand::kSettings, u"Settings"},
};

metrics::OmniboxEventProto::PageClassification GetPageClassification(
    Mode mode) {
  return mode == Mode::kNewEntry
             ? metrics::OmniboxEventProto::
                   INSTANT_NTP_WITH_OMNIBOX_AS_STARTING_FOCUS
             : metrics::OmniboxEventProto::OTHER;
}

// The query's words, lowercased.
std::vector<std::u16string> GetWords(const std::u16string& text) {
  return base::SplitString(base::i18n::ToLower(text), u" ",
                           base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
}

// Whether every one of |words| is in |haystack| (lowercased).
bool ContainsAll(const std::u16string& haystack,
                 const std::vector<std::u16string>& words) {
  return std::ranges::all_of(words, [&](const std::u16string& word) {
    return haystack.find(word) != std::u16string::npos;
  });
}

// Calls |visit| for each tab node under |node|, in tree order.
template <typename Visit>
void ForEachTab(const SidebarNode& node, Visit&& visit) {
  for (const auto& child : node.children()) {
    if (child->type() == SidebarNode::Type::kTab) {
      visit(*child);
    } else {
      ForEachTab(*child, visit);
    }
  }
}

std::u16string GetEntryTitle(const SidebarNode& node) {
  return node.title().empty() ? base::UTF8ToUTF16(node.url().spec())
                              : node.title();
}

}  // namespace

CommandBarPageHandler::Item::Item() = default;
CommandBarPageHandler::Item::Item(Item&&) = default;
CommandBarPageHandler::Item& CommandBarPageHandler::Item::operator=(Item&&) =
    default;
CommandBarPageHandler::Item::~Item() = default;

CommandBarPageHandler::CommandBarPageHandler(
    mojo::PendingReceiver<command_bar::mojom::PageHandler> receiver,
    mojo::PendingRemote<command_bar::mojom::Page> page,
    BrowserWindowInterface* browser,
    TopChromeWebUIController* controller)
    : browser_(browser),
      controller_(controller),
      receiver_(this, std::move(receiver)),
      page_(std::move(page)) {
  Profile* profile = browser_->GetProfile();
  autocomplete_ = std::make_unique<AutocompleteController>(
      std::make_unique<ChromeAutocompleteProviderClient>(profile),
      AutocompleteControllerConfig{
          .provider_types = AutocompleteClassifier::DefaultOmniboxProviders()});
  autocomplete_->AddObserver(this);
}

CommandBarPageHandler::~CommandBarPageHandler() {
  autocomplete_->RemoveObserver(this);
}

void CommandBarPageHandler::Open(Mode mode, const std::u16string& text) {
  mode_ = mode;
  first_edit_time_ = base::TimeTicks();
  page_->Open(mode, base::UTF16ToUTF8(text));
}

void CommandBarPageHandler::ShowUI() {
  if (controller_ && controller_->embedder()) {
    controller_->embedder()->ShowUI();
  }
}

void CommandBarPageHandler::Query(const std::string& text,
                                  bool prevent_inline_autocomplete) {
  const std::u16string text16 = base::UTF8ToUTF16(text);
  if (text16 != text_) {
    ids_.clear();
  }
  text_ = text16;

  if (base::TrimWhitespace(text_, base::TRIM_ALL).empty()) {
    autocomplete_->Stop(AutocompleteStopReason::kClobbered);
    Rebuild();
    return;
  }
  AutocompleteInput input(text_, text_.size(), GetPageClassification(mode_),
                          ChromeAutocompleteSchemeClassifier(
                              browser_->GetProfile()));
  if (mode_ == Mode::kCurrentEntry) {
    if (tabs::TabInterface* tab = browser_->GetActiveTabInterface()) {
      input.set_current_url(tab->GetContents()->GetLastCommittedURL());
    }
  }
  input.set_prevent_inline_autocomplete(prevent_inline_autocomplete);
  // Results come back through OnResultChanged(), the first pass at once.
  autocomplete_->Start(input);
}

void CommandBarPageHandler::Accept(uint32_t id,
                                   const std::string& text,
                                   bool new_entry) {
  auto it = std::ranges::find(items_, id, &Item::id);
  if (it == items_.end()) {
    // The page is behind: act on the text as the omnibox would.
    const std::u16string text16 = base::UTF8ToUTF16(text);
    if (!base::TrimWhitespace(text16, base::TRIM_ALL).empty()) {
      AutocompleteMatch match;
      AutocompleteClassifierFactory::GetForProfile(browser_->GetProfile())
          ->Classify(text16, false, true, GetPageClassification(mode_),
                     &match, nullptr);
      if (match.destination_url.is_valid()) {
        AcceptMatch(match, 0, new_entry);
      }
    }
    Close();
    return;
  }

  // Acting can switch spaces, which can end up rebuilding |items_|.
  Item item = std::move(*it);
  switch (item.kind) {
    case ResultKind::kNavigate:
    case ResultKind::kSearch:
      AcceptMatch(*item.match, item.match_line, new_entry);
      break;
    case ResultKind::kEntry:
      AcceptEntry(item.node_id, item.space_id);
      break;
    case ResultKind::kSpace:
      AcceptSpace(item.space_id);
      break;
    case ResultKind::kCommand:
      RunCommand(item.command);
      break;
  }
  Close();
}

void CommandBarPageHandler::Close() {
  autocomplete_->Stop(AutocompleteStopReason::kClobbered);
  if (controller_ && controller_->embedder()) {
    controller_->embedder()->CloseUI();
  }
}

void CommandBarPageHandler::OnResultChanged(AutocompleteController* controller,
                                            bool default_match_changed) {
  Rebuild();
}

void CommandBarPageHandler::Rebuild() {
  std::vector<Item> items;
  std::u16string inline_autocompletion;
  const bool empty = base::TrimWhitespace(text_, base::TRIM_ALL).empty();

  if (empty) {
    if (mode_ == Mode::kNewEntry) {
      AddRecentTodayItems(items);
    }
    AddCommandItems(items);
  } else {
    const AutocompleteResult& result = autocomplete_->result();
    // The default match leads: it's what Enter takes.
    size_t first_other = 0;
    if (!result.empty() && result.match_at(0).allowed_to_be_default_match) {
      const AutocompleteMatch& match = result.match_at(0);
      items.push_back(MatchItem(match, 0));
      inline_autocompletion = match.inline_autocompletion;
      first_other = 1;
    }
    AddEntryItems(items);
    for (size_t i = first_other; i < result.size(); ++i) {
      items.push_back(MatchItem(result.match_at(i), i));
    }
    AddSpaceItems(items);
    AddCommandItems(items);
  }

  items_ = std::move(items);
  std::vector<command_bar::mojom::ResultPtr> results;
  for (const Item& item : items_) {
    auto result = command_bar::mojom::Result::New();
    result->id = item.id;
    result->kind = item.kind;
    result->title = base::UTF16ToUTF8(item.title);
    result->subtitle = base::UTF16ToUTF8(item.subtitle);
    result->action = item.action;
    result->icon_url = item.icon_url;
    results.push_back(std::move(result));
  }
  page_->SetResults(base::UTF16ToUTF8(text_), std::move(results),
                    base::UTF16ToUTF8(inline_autocompletion));
}

CommandBarPageHandler::Item CommandBarPageHandler::MatchItem(
    const AutocompleteMatch& match,
    size_t line) {
  Item item;
  const bool search = AutocompleteMatch::IsSearchType(match.type);
  item.kind = search ? ResultKind::kSearch : ResultKind::kNavigate;
  item.title = match.contents;
  item.subtitle = match.description;
  item.action = search ? "Search" : "Open";
  if (!search) {
    item.icon_url = match.destination_url;
  }
  item.match = match;
  item.match_line = line;
  AssignId(item, "m:" + match.destination_url.spec());
  return item;
}

void CommandBarPageHandler::AddEntryItems(std::vector<Item>& items) {
  WindowFeatures* features = WindowFeatures::From(browser_);
  if (!features) {
    return;
  }
  const std::vector<std::u16string> words = GetWords(text_);
  const SpaceModel& own_space = features->space_model();
  std::vector<const SpaceModel*> spaces = {&own_space};
  for (const auto& space : features->service().space_list().spaces()) {
    if (space.get() != &own_space) {
      spaces.push_back(space.get());
    }
  }

  size_t count = 0;
  for (const SpaceModel* space : spaces) {
    for (const SidebarNode* root : {space->pinned_root(), space->today_root()}) {
      ForEachTab(*root, [&](const SidebarNode& node) {
        if (count == kMaxEntries ||
            !ContainsAll(base::i18n::ToLower(node.title() + u" " +
                                             base::UTF8ToUTF16(
                                                 node.url().spec())),
                         words)) {
          return;
        }
        items.push_back(EntryItem(node, *space));
        ++count;
      });
    }
  }
}

void CommandBarPageHandler::AddRecentTodayItems(std::vector<Item>& items) {
  WindowFeatures* features = WindowFeatures::From(browser_);
  if (!features) {
    return;
  }
  const SpaceModel& space = features->space_model();
  std::vector<const SidebarNode*> nodes;
  ForEachTab(*space.today_root(),
             [&](const SidebarNode& node) { nodes.push_back(&node); });
  std::ranges::stable_sort(nodes, std::ranges::greater(),
                           &SidebarNode::last_active);
  nodes.resize(std::min(nodes.size(), kMaxRecentEntries));
  for (const SidebarNode* node : nodes) {
    items.push_back(EntryItem(*node, space));
  }
}

CommandBarPageHandler::Item CommandBarPageHandler::EntryItem(
    const SidebarNode& node,
    const SpaceModel& space) {
  WindowFeatures* features = WindowFeatures::From(browser_);
  const bool own_space = &features->space_model() == &space;
  const bool live = features->registry().GetTab(&node);

  Item item;
  item.kind = ResultKind::kEntry;
  item.title = GetEntryTitle(node);
  item.subtitle = base::UTF8ToUTF16(node.url().spec());
  if (!own_space) {
    item.subtitle = space.name() + u" · " + item.subtitle;
  }
  item.action = live ? "Switch to tab" : "Open";
  item.icon_url = node.url();
  item.node_id = node.id();
  item.space_id = space.id();
  AssignId(item, "e:" + node.id().AsLowercaseString());
  return item;
}

void CommandBarPageHandler::AddSpaceItems(std::vector<Item>& items) {
  WindowFeatures* features = WindowFeatures::From(browser_);
  if (!features) {
    return;
  }
  const std::vector<std::u16string> words = GetWords(text_);
  for (const auto& space : features->service().space_list().spaces()) {
    if (space.get() == &features->space_model() ||
        !ContainsAll(base::i18n::ToLower(space->name()), words)) {
      continue;
    }
    Item item;
    item.kind = ResultKind::kSpace;
    item.title = space->name();
    item.action = "Switch to space";
    item.space_id = space->id();
    AssignId(item, "s:" + space->id().AsLowercaseString());
    items.push_back(std::move(item));
  }
}

void CommandBarPageHandler::AddCommandItems(std::vector<Item>& items) {
  const std::vector<std::u16string> words = GetWords(text_);
  for (const CommandInfo& info : kCommands) {
    if (!IsCommandAvailable(info.command) ||
        !ContainsAll(base::i18n::ToLower(info.name), words)) {
      continue;
    }
    Item item;
    item.kind = ResultKind::kCommand;
    item.title = info.name;
    item.action = "Run";
    item.command = info.command;
    AssignId(item,
             "c:" + base::NumberToString(static_cast<int>(info.command)));
    items.push_back(std::move(item));
  }
}

void CommandBarPageHandler::AssignId(Item& item, const std::string& key) {
  auto [it, inserted] = ids_.try_emplace(key, next_id_);
  if (inserted) {
    ++next_id_;
  }
  item.id = it->second;
}

void CommandBarPageHandler::AcceptMatch(const AutocompleteMatch& match,
                                        size_t line,
                                        bool new_entry) {
  // Teach the shortcuts provider, as the omnibox does.
  if (scoped_refptr<ShortcutsBackend> shortcuts =
          ShortcutsBackendFactory::GetForProfile(browser_->GetProfile())) {
    shortcuts->AddOrUpdateShortcut(text_, match);
  }

  NavigateParams params(
      browser_, match.destination_url,
      ui::PageTransitionFromInt(match.transition |
                                ui::PAGE_TRANSITION_FROM_ADDRESS_BAR));
  params.disposition = new_entry || mode_ == Mode::kNewEntry ||
                               !browser_->GetActiveTabInterface()
                           ? WindowOpenDisposition::NEW_FOREGROUND_TAB
                           : WindowOpenDisposition::CURRENT_TAB;
  params.window_action = NavigateParams::WindowAction::kShowWindow;
  Navigate(&params);
}

void CommandBarPageHandler::AcceptEntry(const base::Uuid& node_id,
                                        const base::Uuid& space_id) {
  WindowFeatures* features = WindowFeatures::From(browser_);
  SpaceModel* space =
      features ? features->service().space_list().FindSpace(space_id)
               : nullptr;
  if (!space) {
    return;
  }
  if (space != &features->space_model()) {
    features->SwitchToSpace(*space);
  }
  SidebarNode* node = space->FindById(node_id);
  if (!node || node->type() != SidebarNode::Type::kTab) {
    return;
  }
  if (tabs::TabInterface* tab = features->tab_strip_bridge().OpenNode(node)) {
    TabStripModel* model = browser_->GetTabStripModel();
    model->ActivateTabAt(model->GetIndexOfTab(tab));
  }
}

void CommandBarPageHandler::AcceptSpace(const base::Uuid& space_id) {
  WindowFeatures* features = WindowFeatures::From(browser_);
  if (SpaceModel* space =
          features ? features->service().space_list().FindSpace(space_id)
                   : nullptr) {
    features->SwitchToSpace(*space);
  }
}

void CommandBarPageHandler::RunCommand(CommandBarCommand command) {
  if (!IsCommandAvailable(command)) {
    return;
  }
  WindowFeatures* features = WindowFeatures::From(browser_);
  switch (command) {
    case CommandBarCommand::kNewSpace: {
      SpaceList& list = features->service().space_list();
      SpaceModel* space = list.AddSpace(
          list.spaces().size(),
          u"Space " + base::NumberToString16(list.spaces().size() + 1));
      features->SwitchToSpace(*space);
      break;
    }
    case CommandBarCommand::kPinEntry: {
      SidebarNode* node = GetActiveNode();
      // A pane moves with its split.
      SidebarNode* entry =
          node->parent()->type() == SidebarNode::Type::kSplit ? node->parent()
                                                              : node;
      SpaceModel& space = features->space_model();
      if (space.CanMove(entry, space.pinned_root())) {
        space.Move(entry, space.pinned_root(),
                   space.pinned_root()->children().size());
      }
      break;
    }
    case CommandBarCommand::kUnpinEntry: {
      SidebarNode* node = GetActiveNode();
      SidebarNode* entry =
          node->parent()->type() == SidebarNode::Type::kSplit ? node->parent()
                                                              : node;
      SpaceModel& space = features->space_model();
      if (space.CanMove(entry, space.today_root())) {
        space.Move(entry, space.today_root(), 0);
      }
      break;
    }
    case CommandBarCommand::kCopyUrl:
      chrome::ExecuteCommand(browser_, IDC_COPY_URL);
      break;
    case CommandBarCommand::kReload:
      chrome::ExecuteCommand(browser_, IDC_RELOAD);
      break;
    case CommandBarCommand::kHistory:
      chrome::ExecuteCommand(browser_, IDC_SHOW_HISTORY);
      break;
    case CommandBarCommand::kDownloads:
      chrome::ExecuteCommand(browser_, IDC_SHOW_DOWNLOADS);
      break;
    case CommandBarCommand::kExtensions:
      chrome::ExecuteCommand(browser_, IDC_MANAGE_EXTENSIONS);
      break;
    case CommandBarCommand::kSettings:
      chrome::ExecuteCommand(browser_, IDC_OPTIONS);
      break;
  }
}

bool CommandBarPageHandler::IsCommandAvailable(
    CommandBarCommand command) const {
  WindowFeatures* features = WindowFeatures::From(browser_);
  switch (command) {
    case CommandBarCommand::kNewSpace:
      return features && !features->is_parking_window();
    case CommandBarCommand::kPinEntry: {
      const SidebarNode* node = GetActiveNode();
      return node && !IsPinned(*node);
    }
    case CommandBarCommand::kUnpinEntry: {
      const SidebarNode* node = GetActiveNode();
      return node && IsPinned(*node);
    }
    case CommandBarCommand::kCopyUrl:
    case CommandBarCommand::kReload:
      return browser_->GetActiveTabInterface();
    case CommandBarCommand::kHistory:
    case CommandBarCommand::kDownloads:
    case CommandBarCommand::kExtensions:
    case CommandBarCommand::kSettings:
      return true;
  }
}

SidebarNode* CommandBarPageHandler::GetActiveNode() const {
  WindowFeatures* features = WindowFeatures::From(browser_);
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  if (!features || features->is_parking_window() || !tab) {
    return nullptr;
  }
  return features->tab_strip_bridge().GetNode(tab);
}

bool CommandBarPageHandler::IsPinned(const SidebarNode& node) const {
  return WindowFeatures::From(browser_)->space_model().pinned_root()->Contains(
      &node);
}

}  // namespace strata
