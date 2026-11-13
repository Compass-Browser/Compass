// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/command_bar/command_bar_page_handler.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/search_test_utils.h"
#include "components/omnibox/browser/omnibox_event_global_tracker.h"
#include "components/omnibox/browser/omnibox_log.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/window_features.h"
#include "compass/test/compass_browser_window_test.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

using command_bar::mojom::Mode;
using command_bar::mojom::ResultKind;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;

// Records what the browser pushes to the page.
class FakePage : public command_bar::mojom::Page {
 public:
  mojo::PendingRemote<command_bar::mojom::Page> BindAndPassRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  // command_bar::mojom::Page:
  void Open(Mode mode, const std::string& text) override { opened_ = text; }
  void SetResults(const std::string& text,
                  std::vector<command_bar::mojom::ResultPtr> results,
                  const std::string& inline_autocompletion) override {
    text_ = text;
    results_ = std::move(results);
  }

  const std::string& opened() const { return opened_; }
  const std::string& text() const { return text_; }
  const std::vector<command_bar::mojom::ResultPtr>& results() const {
    return results_;
  }

  // The results other than omnibox suggestions and the Claude row (which
  // every query has; see AsksClaude), as "kind title". Entries are named by
  // their URL's host, which the tests keep unique.
  std::vector<std::string> LocalResults() const {
    std::vector<std::string> names;
    for (const auto& result : results_) {
      if (IsClaudeRow(*result)) {
        continue;
      }
      switch (result->kind) {
        case ResultKind::kNavigate:
        case ResultKind::kSearch:
          break;
        case ResultKind::kEntry:
          names.push_back("entry " + std::string(result->icon_url.host()));
          break;
        case ResultKind::kSpace:
          names.push_back("space " + result->title);
          break;
        case ResultKind::kCommand:
          names.push_back("command " + result->title);
          break;
        case ResultKind::kOtherDeviceTab:
          names.push_back(
              "other " + std::string(result->icon_url.host()) + " " +
              result->subtitle.substr(0, result->subtitle.find(" · ")));
          break;
      }
    }
    return names;
  }

  static bool IsClaudeRow(const command_bar::mojom::Result& result) {
    return result.kind == ResultKind::kCommand &&
           (result.title == "Ask Claude" || result.title == "Chat with Claude");
  }

  // The ID of the first result titled |title| (or, for entries, whose host
  // is |title|), or 0.
  uint32_t IdOf(const std::string& title) const {
    for (const auto& result : results_) {
      if (result->title == title || (result->kind == ResultKind::kEntry &&
                                     result->icon_url.host() == title)) {
        return result->id;
      }
    }
    return 0;
  }

 private:
  mojo::Receiver<command_bar::mojom::Page> receiver_{this};
  std::string opened_;
  std::string text_;
  std::vector<command_bar::mojom::ResultPtr> results_;
};

class CommandBarPageHandlerTest : public CompassBrowserWindowTest {
 protected:
  // Mock time lets tests step past Browser's coalesced UI updates.
  CommandBarPageHandlerTest()
      : CompassBrowserWindowTest(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  TestingProfile::TestingFactories GetTestingFactories() override {
    TestingProfile::TestingFactories factories =
        CompassBrowserWindowTest::GetTestingFactories();
    factories.emplace_back(
        TemplateURLServiceFactory::GetInstance(),
        base::BindRepeating(&TemplateURLServiceFactory::BuildInstanceFor));
    factories.emplace_back(
        AutocompleteClassifierFactory::GetInstance(),
        base::BindRepeating(&AutocompleteClassifierFactory::BuildInstanceFor));
    return factories;
  }

  void SetUp() override {
    CompassBrowserWindowTest::SetUp();
    search_test_utils::WaitForTemplateURLServiceToLoad(
        TemplateURLServiceFactory::GetForProfile(profile()));
    // Test windows can't be created the production way.
    WindowFeatures::From(browser())
        ->service()
        .SetParkingWindowFactoryForTesting(
            base::BindLambdaForTesting([this](Profile* profile) {
              parking_ =
                  CreateBrowser(profile, BrowserWindowInterface::TYPE_POPUP,
                                /*hosted_app=*/false);
              return parking_.get();
            }));
    // AddTab() inserts at the front and activates.
    AddTab(browser(), GURL("https://b.test/"));
    AddTab(browser(), GURL("https://a.test/"));
    Settle();
    handler_ = std::make_unique<CommandBarPageHandler>(
        handler_remote_.BindNewPipeAndPassReceiver(), page_.BindAndPassRemote(),
        browser(), /*controller=*/nullptr);
    // No SESSIONS type in tests: the fake devices' tabs.
    handler_->SetOtherDeviceTabsGetterForTesting(
        base::BindLambdaForTesting([this]() { return other_devices_; }));
  }

  void TearDown() override {
    handler_.reset();
    if (parking_) {
      parking_->GetTabStripModel()->CloseAllTabs();
      parking_.reset();
    }
    CompassBrowserWindowTest::TearDown();
  }

  // Opens the bar in |mode| and queries |text|, as the page does.
  void OpenAndQuery(Mode mode, const std::string& text) {
    handler_->Open(mode, std::u16string());
    handler_remote_->Query(text, /*prevent_inline_autocomplete=*/false);
    Settle();
  }

  // Delivers Mojo messages, runs Browser's coalesced UI updates and lets
  // the omnibox providers finish.
  void Settle() { task_environment()->FastForwardBy(base::Seconds(1)); }

  TabStripModel* tab_strip_model() { return browser()->GetTabStripModel(); }
  SpaceModel& space_model() {
    return WindowFeatures::From(browser())->space_model();
  }
  SidebarNode* TodayEntry(size_t index) {
    return space_model().today_root()->children()[index].get();
  }
  SpaceModel& favorites() {
    return WindowFeatures::From(browser())->service().space_list().favorites();
  }

  std::vector<OtherDevice> other_devices_;
  FakePage page_;
  mojo::Remote<command_bar::mojom::PageHandler> handler_remote_;
  std::unique_ptr<CommandBarPageHandler> handler_;
  std::unique_ptr<BrowserWindowInterface> parking_;
};

TEST_F(CommandBarPageHandlerTest, EmptyNewEntryQueryShowsRecentEntries) {
  // b was seen last, a never.
  space_model().SetLastActive(TodayEntry(0), base::Time());
  space_model().SetLastActive(TodayEntry(1), base::Time::Now());

  OpenAndQuery(Mode::kNewEntry, "");

  EXPECT_EQ(page_.text(), "");
  EXPECT_THAT(page_.LocalResults(),
              ElementsAre("entry b.test", "entry a.test", "command New space",
                          "command New space on this device",
                          "command Pin tab", "command Add to Favorites",
                          "command Copy URL",
                          "command Reload", "command History",
                          "command Downloads", "command Extensions",
                          "command Settings", "command Import from Arc",
                          "command Keyboard shortcuts"));
}

TEST_F(CommandBarPageHandlerTest, EmptyCurrentEntryQueryShowsCommandsOnly) {
  OpenAndQuery(Mode::kCurrentEntry, "");

  EXPECT_THAT(page_.LocalResults(),
              Not(Contains(testing::StartsWith("entry"))));
  EXPECT_THAT(page_.LocalResults(), Contains("command Reload"));
}

TEST_F(CommandBarPageHandlerTest, OffersUnpinForPinnedEntries) {
  space_model().Move(TodayEntry(0), space_model().pinned_root(), 0);
  Settle();

  OpenAndQuery(Mode::kNewEntry, "pin");

  EXPECT_THAT(page_.LocalResults(), ElementsAre("command Unpin tab"));
}

TEST_F(CommandBarPageHandlerTest, MatchesEntriesSpacesAndCommands) {
  SpaceList& list = WindowFeatures::From(browser())->service().space_list();
  list.AddSpace(1, u"Reading");

  OpenAndQuery(Mode::kNewEntry, "re");

  // Entries come before spaces, spaces before commands.
  EXPECT_THAT(page_.LocalResults(),
              ElementsAre("space Reading", "command Reload"));

  handler_remote_->Query("a.test", false);
  Settle();

  EXPECT_EQ(page_.text(), "a.test");
  EXPECT_THAT(page_.LocalResults(), ElementsAre("entry a.test"));
  // The omnibox's default match leads.
  ASSERT_FALSE(page_.results().empty());
  EXPECT_EQ(page_.results()[0]->kind, ResultKind::kNavigate);
}

TEST_F(CommandBarPageHandlerTest, NamesTheSpaceOfOtherSpacesEntries) {
  SpaceList& list = WindowFeatures::From(browser())->service().space_list();
  SpaceModel* work = list.AddSpace(1, u"Work");
  work->AddTab(work->today_root(), 0, GURL("https://c.test/"), u"Cee");

  OpenAndQuery(Mode::kNewEntry, "cee");

  ASSERT_THAT(page_.LocalResults(), ElementsAre("entry c.test"));
  const auto entry =
      std::ranges::find(page_.results(), ResultKind::kEntry,
                        [](const auto& result) { return result->kind; });
  EXPECT_EQ((*entry)->subtitle, "Work · https://c.test/");
  EXPECT_EQ((*entry)->action, "Open");
}

TEST_F(CommandBarPageHandlerTest, ListsFavoritesFirstAndOpensThemInPlace) {
  // b is the second Today entry.
  SidebarNode* b = TodayEntry(1);
  ASSERT_EQ(b->url(), GURL("https://b.test/"));
  MoveEntry(profile(), b, favorites(), favorites().pinned_root(), 0);
  Settle();

  OpenAndQuery(Mode::kNewEntry, "test");

  EXPECT_THAT(page_.LocalResults(),
              ElementsAre("entry b.test", "entry a.test"));
  const auto entry =
      std::ranges::find(page_.results(), ResultKind::kEntry,
                        [](const auto& result) { return result->kind; });
  EXPECT_EQ((*entry)->subtitle, "Favorites · https://b.test/");

  // From another space, the favorite opens in it: no switch back.
  SpaceList& list = WindowFeatures::From(browser())->service().space_list();
  SpaceModel* work = list.AddSpace(1, u"Work");
  WindowFeatures::From(browser())->SwitchToSpace(*work);
  Settle();
  OpenAndQuery(Mode::kNewEntry, "b.test");
  // Not the omnibox's suggestion titled the same.
  const auto favorite =
      std::ranges::find(page_.results(), ResultKind::kEntry,
                        [](const auto& result) { return result->kind; });
  ASSERT_NE(favorite, page_.results().end());

  handler_remote_->Accept((*favorite)->id, "b.test", false);
  Settle();

  EXPECT_EQ(&space_model(), work);
  EXPECT_EQ(tab_strip_model()->GetActiveWebContents()->GetVisibleURL(),
            GURL("https://b.test/"));
}

TEST_F(CommandBarPageHandlerTest, AddsAndRemovesTheActiveEntryFromFavorites) {
  OpenAndQuery(Mode::kNewEntry, "favorites");
  ASSERT_THAT(page_.LocalResults(), ElementsAre("command Add to Favorites"));

  handler_remote_->Accept(page_.IdOf("Add to Favorites"), "favorites", false);
  Settle();

  ASSERT_EQ(favorites().pinned_root()->children().size(), 1u);
  EXPECT_EQ(favorites().pinned_root()->children()[0]->url(),
            GURL("https://a.test/"));
  // Its tab stays.
  EXPECT_EQ(tab_strip_model()->count(), 2);

  // A favorite isn't pinned or unpinned.
  OpenAndQuery(Mode::kNewEntry, "pin");
  EXPECT_THAT(page_.LocalResults(), IsEmpty());

  OpenAndQuery(Mode::kNewEntry, "favorites");
  ASSERT_THAT(page_.LocalResults(),
              ElementsAre("command Remove from Favorites"));

  handler_remote_->Accept(page_.IdOf("Remove from Favorites"), "favorites",
                          false);
  Settle();

  EXPECT_TRUE(favorites().pinned_root()->children().empty());
  EXPECT_EQ(TodayEntry(0)->url(), GURL("https://a.test/"));
  EXPECT_EQ(tab_strip_model()->count(), 2);
}

// A window swapping to another profile's space (SwitchWindowToSpace()) keeps
// showing its tabs, bridgeless, until it closes; its space can go first. The
// page can still call in then, and nothing it asks for may crash.
TEST_F(CommandBarPageHandlerTest, StaysQuietInAWindowThatSwappedAway) {
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceList& list = features->service().space_list();
  list.AddSpace(list.spaces().size(), u"Other");
  features->BeginSwap();
  list.RemoveSpace(space_model());
  ASSERT_FALSE(features->has_tab_strip_bridge());
  ASSERT_TRUE(browser()->GetActiveTabInterface());

  OpenAndQuery(Mode::kNewEntry, "");
  EXPECT_THAT(page_.LocalResults(),
              Not(Contains(testing::StartsWith("entry"))));
  EXPECT_THAT(page_.LocalResults(), Not(Contains("command New space")));
  EXPECT_THAT(page_.LocalResults(),
              Not(Contains("command New space on this device")));
  EXPECT_THAT(page_.LocalResults(), Not(Contains("command Pin tab")));
  EXPECT_THAT(page_.LocalResults(),
              Not(Contains("command Add to Favorites")));
  EXPECT_THAT(page_.LocalResults(), Contains("command Reload"));
}

TEST_F(CommandBarPageHandlerTest, KeepsIdsWithinAQuery) {
  OpenAndQuery(Mode::kNewEntry, "re");
  const uint32_t reload = page_.IdOf("Reload");
  ASSERT_NE(reload, 0u);

  // Results come in again as providers finish: same IDs.
  handler_remote_->Query("re", false);
  Settle();
  EXPECT_EQ(page_.IdOf("Reload"), reload);

  // A new query starts over.
  handler_remote_->Query("rel", false);
  Settle();
  EXPECT_NE(page_.IdOf("Reload"), 0u);
  EXPECT_NE(page_.IdOf("Reload"), reload);
}

TEST_F(CommandBarPageHandlerTest, AcceptingAnEntrySwitchesToItsTab) {
  OpenAndQuery(Mode::kNewEntry, "");
  ASSERT_EQ(tab_strip_model()->active_index(), 0);

  handler_remote_->Accept(page_.IdOf("b.test"), "", false);
  Settle();

  EXPECT_EQ(tab_strip_model()->active_index(), 1);
  EXPECT_EQ(tab_strip_model()->count(), 2);
}

TEST_F(CommandBarPageHandlerTest, AcceptingASpaceSwitchesToIt) {
  SpaceList& list = WindowFeatures::From(browser())->service().space_list();
  SpaceModel* work = list.AddSpace(1, u"Work");
  OpenAndQuery(Mode::kNewEntry, "work");

  handler_remote_->Accept(page_.IdOf("Work"), "work", false);
  Settle();

  EXPECT_EQ(&space_model(), work);
}

// A space's row shows its icon (docs/design/spaces.md, "Space icons").
TEST_F(CommandBarPageHandlerTest, SpacesCarryTheirIcons) {
  SpaceList& list = WindowFeatures::From(browser())->service().space_list();
  list.AddSpace(1, u"Work")->SetIcon(*SpaceIcon::Symbol("work"));
  list.AddSpace(2, u"Wonder")->SetIcon(*SpaceIcon::Emoji("🚀"));
  list.AddSpace(3, u"Wool");
  OpenAndQuery(Mode::kNewEntry, "wo");

  std::vector<std::string> icons;
  for (const auto& result : page_.results()) {
    if (result->kind == ResultKind::kSpace) {
      icons.push_back(result->title + " " + result->space_symbol + "|" +
                      result->space_emoji);
    }
  }
  EXPECT_THAT(icons, ElementsAre("Work work|", "Wonder |🚀", "Wool |"));
}

TEST_F(CommandBarPageHandlerTest, PinCommandPinsTheActiveEntry) {
  OpenAndQuery(Mode::kNewEntry, "pin");

  handler_remote_->Accept(page_.IdOf("Pin tab"), "pin", false);
  Settle();

  ASSERT_EQ(space_model().pinned_root()->children().size(), 1u);
  EXPECT_EQ(space_model().pinned_root()->children()[0]->url(),
            GURL("https://a.test/"));
}

TEST_F(CommandBarPageHandlerTest, NewEntryModeOpensSuggestionsInANewTab) {
  OpenAndQuery(Mode::kNewEntry, "c.test");
  ASSERT_FALSE(page_.results().empty());

  handler_remote_->Accept(page_.results()[0]->id, "c.test", false);
  Settle();

  EXPECT_EQ(tab_strip_model()->count(), 3);
  EXPECT_EQ(tab_strip_model()->GetActiveWebContents()->GetVisibleURL(),
            GURL("http://c.test/"));
}

TEST_F(CommandBarPageHandlerTest, CurrentEntryModeLoadsInTheActiveTab) {
  OpenAndQuery(Mode::kCurrentEntry, "c.test");
  ASSERT_FALSE(page_.results().empty());

  handler_remote_->Accept(page_.results()[0]->id, "c.test", false);
  Settle();

  EXPECT_EQ(tab_strip_model()->count(), 2);
  EXPECT_EQ(tab_strip_model()->GetActiveWebContents()->GetVisibleURL(),
            GURL("http://c.test/"));
}

TEST_F(CommandBarPageHandlerTest, NewEntryFlagOpensANewTabInCurrentMode) {
  OpenAndQuery(Mode::kCurrentEntry, "c.test");
  ASSERT_FALSE(page_.results().empty());

  handler_remote_->Accept(page_.results()[0]->id, "c.test", true);
  Settle();

  EXPECT_EQ(tab_strip_model()->count(), 3);
}

TEST_F(CommandBarPageHandlerTest, ReportsTheOmniboxLog) {
  std::vector<std::string> logs;
  base::CallbackListSubscription subscription =
      OmniboxEventGlobalTracker::GetInstance()->RegisterCallback(
          base::BindLambdaForTesting([&](OmniboxLog* log) {
            logs.push_back(base::UTF16ToUTF8(log->text) + " line " +
                           base::NumberToString(log->selection.line) + " " +
                           log->final_destination_url.spec());
            EXPECT_EQ(log->disposition,
                      WindowOpenDisposition::NEW_FOREGROUND_TAB);
            EXPECT_GE(log->elapsed_time_since_user_first_modified_omnibox,
                      base::TimeDelta());
          }));
  OpenAndQuery(Mode::kNewEntry, "c.test");
  ASSERT_FALSE(page_.results().empty());

  handler_remote_->Accept(page_.results()[0]->id, "c.test", false);
  Settle();

  EXPECT_THAT(logs, ElementsAre("c.test line 0 http://c.test/"));

  // Entries, commands and stale IDs aren't omnibox suggestions.
  OpenAndQuery(Mode::kNewEntry, "reload");
  handler_remote_->Accept(page_.IdOf("Reload"), "reload", false);
  handler_remote_->Accept(/*id=*/9999, "d.test", false);
  Settle();
  EXPECT_EQ(logs.size(), 1u);
}

// docs/design/sync.md, "UI".
TEST_F(CommandBarPageHandlerTest, TabsOnOtherDevices) {
  other_devices_ = {
      {u"MacBook Air",
       {{u"Recipes", GURL("https://food.test/")},
        {u"Train times", GURL("https://rail.test/")}}},
      {u"Studio", {{u"Food news", GURL("https://news.test/food")}}},
  };

  // By title or URL, grouped by device (the most recent device first).
  OpenAndQuery(Mode::kNewEntry, "food");
  EXPECT_THAT(page_.LocalResults(),
              ElementsAre("other food.test On MacBook Air",
                          "other news.test On Studio"));

  // By device name: all its tabs.
  OpenAndQuery(Mode::kNewEntry, "macbook");
  EXPECT_THAT(page_.LocalResults(),
              ElementsAre("other food.test On MacBook Air",
                          "other rail.test On MacBook Air"));

  // Not in the empty state.
  OpenAndQuery(Mode::kNewEntry, "");
  for (const auto& result : page_.results()) {
    EXPECT_NE(result->kind, ResultKind::kOtherDeviceTab);
  }

  // Enter opens it as a new Today entry, even from current-entry mode.
  OpenAndQuery(Mode::kCurrentEntry, "train");
  const uint32_t id = page_.IdOf("Train times");
  ASSERT_NE(id, 0u);
  handler_remote_->Accept(id, "train", false);
  Settle();
  EXPECT_EQ(tab_strip_model()->count(), 3);
  EXPECT_EQ(tab_strip_model()->GetActiveWebContents()->GetVisibleURL(),
            GURL("https://rail.test/"));
  EXPECT_EQ(space_model().today_root()->children().size(), 3u);
}

TEST(OtherDeviceTabsTest, MatchesEveryWordUpToTheLimit) {
  const std::vector<OtherDevice> devices = {
      {u"Air",
       {{u"A b", GURL("https://one.test/")},
        {u"A c", GURL("https://two.test/")}}},
      {u"Studio", {{u"A b", GURL("https://three.test/")}}},
  };
  auto matched = [&](const std::u16string& query, size_t limit) {
    std::vector<std::string> out;
    for (const OtherDeviceTabMatch& match :
         MatchOtherDeviceTabs(devices, query, limit)) {
      out.emplace_back(devices[match.device].tabs[match.tab].url.host());
    }
    return out;
  };
  EXPECT_THAT(matched(u"a b", 10), ElementsAre("one.test", "three.test"));
  EXPECT_THAT(matched(u"A", 2), ElementsAre("one.test", "two.test"));
  EXPECT_THAT(matched(u"studio", 10), ElementsAre("three.test"));
  EXPECT_THAT(matched(u"  ", 10), IsEmpty());
  EXPECT_THAT(matched(u"zebra", 10), IsEmpty());
}

TEST_F(CommandBarPageHandlerTest, AcceptingAStaleIdUsesTheText) {
  OpenAndQuery(Mode::kNewEntry, "");

  handler_remote_->Accept(/*id=*/9999, "d.test", false);
  Settle();

  EXPECT_EQ(tab_strip_model()->count(), 3);
  EXPECT_EQ(tab_strip_model()->GetActiveWebContents()->GetVisibleURL(),
            GURL("http://d.test/"));

  // Nothing to act on.
  handler_remote_->Accept(/*id=*/9999, "  ", false);
  Settle();
  EXPECT_EQ(tab_strip_model()->count(), 3);
}

// docs/design/claude.md, "Chat panel".
TEST_F(CommandBarPageHandlerTest, AsksClaude) {
  // No text: the chat, last.
  OpenAndQuery(Mode::kNewEntry, "");
  ASSERT_FALSE(page_.results().empty());
  EXPECT_EQ(page_.results().back()->title, "Chat with Claude");
  EXPECT_EQ(page_.results().back()->subtitle, "");

  // Text naming it: the chat too.
  OpenAndQuery(Mode::kNewEntry, "claude");
  EXPECT_EQ(page_.results().back()->title, "Chat with Claude");

  // Any other text: the question, after everything else.
  OpenAndQuery(Mode::kCurrentEntry, "  what is this page about? ");
  ASSERT_FALSE(page_.results().empty());
  EXPECT_EQ(page_.results().back()->title, "Ask Claude");
  EXPECT_EQ(page_.results().back()->subtitle, "what is this page about?");
  EXPECT_EQ(page_.results().back()->action, "Ask");

  // Accepting it opens no tab (the test window has no panel to ask).
  handler_remote_->Accept(page_.IdOf("Ask Claude"), "what is this page about?",
                          false);
  Settle();
  EXPECT_EQ(tab_strip_model()->count(), 2);
}

TEST_F(CommandBarPageHandlerTest, NoClaudeInIncognito) {
  std::unique_ptr<BrowserWindowInterface> incognito = CreateBrowser(
      profile()->GetPrimaryOTRProfile(/*create_if_needed=*/true),
      BrowserWindowInterface::TYPE_NORMAL, /*hosted_app=*/false);
  mojo::Remote<command_bar::mojom::PageHandler> remote;
  FakePage page;
  auto handler = std::make_unique<CommandBarPageHandler>(
      remote.BindNewPipeAndPassReceiver(), page.BindAndPassRemote(),
      incognito.get(), /*controller=*/nullptr);
  handler->Open(Mode::kCurrentEntry, std::u16string());
  remote->Query("what is this?", false);
  Settle();
  for (const auto& result : page.results()) {
    EXPECT_FALSE(FakePage::IsClaudeRow(*result)) << result->title;
  }
  handler.reset();
  incognito->GetTabStripModel()->CloseAllTabs();
}

}  // namespace
}  // namespace compass
