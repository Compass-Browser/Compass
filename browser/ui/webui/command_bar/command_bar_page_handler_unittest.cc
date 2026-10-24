// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/command_bar/command_bar_page_handler.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "chrome/test/base/search_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace strata {
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
  void Open(Mode mode, const std::string& text) override {
    opened_ = text;
  }
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

  // The results other than omnibox suggestions, as "kind title". Entries are
  // named by their URL's host, which the tests keep unique.
  std::vector<std::string> LocalResults() const {
    std::vector<std::string> names;
    for (const auto& result : results_) {
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
      }
    }
    return names;
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

class CommandBarPageHandlerTest : public BrowserWithTestWindowTest {
 protected:
  // Mock time lets tests step past Browser's coalesced UI updates.
  CommandBarPageHandlerTest()
      : BrowserWithTestWindowTest(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  TestingProfile::TestingFactories GetTestingFactories() override {
    TestingProfile::TestingFactories factories =
        BrowserWithTestWindowTest::GetTestingFactories();
    factories.emplace_back(
        TemplateURLServiceFactory::GetInstance(),
        base::BindRepeating(&TemplateURLServiceFactory::BuildInstanceFor));
    factories.emplace_back(
        AutocompleteClassifierFactory::GetInstance(),
        base::BindRepeating(&AutocompleteClassifierFactory::BuildInstanceFor));
    return factories;
  }

  void SetUp() override {
    BrowserWithTestWindowTest::SetUp();
    search_test_utils::WaitForTemplateURLServiceToLoad(
        TemplateURLServiceFactory::GetForProfile(profile()));
    // Test windows can't be created the production way.
    WindowFeatures::From(browser())->service().SetParkingWindowFactoryForTesting(
        base::BindLambdaForTesting([this](Profile* profile) {
          parking_ = CreateBrowser(profile, BrowserWindowInterface::TYPE_POPUP,
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
  }

  void TearDown() override {
    handler_.reset();
    if (parking_) {
      parking_->GetTabStripModel()->CloseAllTabs();
      parking_.reset();
    }
    BrowserWithTestWindowTest::TearDown();
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
                          "command Pin tab", "command Copy URL",
                          "command Reload", "command History",
                          "command Downloads", "command Extensions",
                          "command Settings"));
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
  const auto entry = std::ranges::find(
      page_.results(), ResultKind::kEntry,
      [](const auto& result) { return result->kind; });
  EXPECT_EQ((*entry)->subtitle, "Work · https://c.test/");
  EXPECT_EQ((*entry)->action, "Open");
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

}  // namespace
}  // namespace strata
