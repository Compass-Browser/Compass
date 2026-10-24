// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/sidebar/sidebar_page_handler.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace strata {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

// Records what the browser pushes to the page, one line per call. Nodes are
// named by their URL's host, which the tests keep unique.
class FakePage : public sidebar::mojom::Page {
 public:
  mojo::PendingRemote<sidebar::mojom::Page> BindAndPassRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  // sidebar::mojom::Page:
  void SetSpaces(std::vector<sidebar::mojom::SpacePtr> spaces,
                 const std::string& active_id) override {
    spaces_.clear();
    for (size_t i = 0; i < spaces.size(); ++i) {
      const std::string& name = spaces[i]->name;
      spaces_ += (i ? "," : "") +
                 (spaces[i]->id == active_id ? "[" + name + "]" : name);
    }
  }
  void SetTree(sidebar::mojom::TreePtr tree) override {
    Learn(*tree->pinned);
    Learn(*tree->today);
    tree_ = std::move(tree);
  }
  void NodeAdded(const std::string& parent_id,
                 uint32_t index,
                 sidebar::mojom::NodePtr node) override {
    hosts_[node->id] = std::string(node->url.host());
    events_.push_back("added " + std::string(node->url.host()) + " at " +
                      std::to_string(index));
  }
  void NodeMoved(const std::string& id,
                 const std::string& new_parent_id,
                 uint32_t index) override {
    events_.push_back("moved " + hosts_[id] + " to " + std::to_string(index));
  }
  void NodeChanged(const std::string& id,
                   const std::string& title,
                   const GURL& url) override {
    hosts_[id] = std::string(url.host());
    events_.push_back("changed " + hosts_[id]);
  }
  void NodeRemoved(const std::string& id) override {
    events_.push_back("removed " + hosts_[id]);
  }
  void NodeLiveChanged(const std::string& id, bool live) override {
    events_.push_back(hosts_[id] + (live ? " live" : " unloaded"));
  }
  void ActiveNodeChanged(const std::optional<std::string>& id) override {
    events_.push_back("active " + (id ? hosts_[*id] : "none"));
  }

  sidebar::mojom::TreePtr TakeTree() { return std::move(tree_); }
  std::vector<std::string> TakeEvents() { return std::move(events_); }
  // Space names, comma separated, the window's in brackets.
  const std::string& spaces() const { return spaces_; }

 private:
  void Learn(const sidebar::mojom::Node& node) {
    hosts_[node.id] = std::string(node.url.host());
    for (const auto& child : node.children) {
      Learn(*child);
    }
  }

  mojo::Receiver<sidebar::mojom::Page> receiver_{this};
  sidebar::mojom::TreePtr tree_;
  std::map<std::string, std::string> hosts_;
  std::vector<std::string> events_;
  std::string spaces_;
};

std::vector<std::string> Hosts(const sidebar::mojom::Node& parent) {
  std::vector<std::string> hosts;
  for (const auto& child : parent.children) {
    hosts.push_back(std::string(child->url.host()));
  }
  return hosts;
}

class SidebarPageHandlerTest : public BrowserWithTestWindowTest {
 protected:
  // Mock time lets tests step past Browser's coalesced UI updates.
  SidebarPageHandlerTest()
      : BrowserWithTestWindowTest(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  void SetUp() override {
    BrowserWithTestWindowTest::SetUp();
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
    handler_ = std::make_unique<SidebarPageHandler>(
        handler_remote_.BindNewPipeAndPassReceiver(), page_.BindAndPassRemote(),
        browser());
    // Like the page, start with the tree, so later events can name nodes.
    GetTree();
  }

  void TearDown() override {
    handler_.reset();
    if (parking_) {
      parking_->GetTabStripModel()->CloseAllTabs();
      parking_.reset();
    }
    BrowserWithTestWindowTest::TearDown();
  }

  sidebar::mojom::TreePtr GetTree() {
    handler_remote_->RequestTree();
    Settle();
    return page_.TakeTree();
  }

  // The ID of Today's entry at |index|.
  std::string TodayId(size_t index) {
    return space_model()
        .today_root()
        ->children()[index]
        ->id()
        .AsLowercaseString();
  }

  std::string PinnedId() {
    return space_model().pinned_root()->id().AsLowercaseString();
  }
  std::string TodayRootId() {
    return space_model().today_root()->id().AsLowercaseString();
  }

  // Delivers Mojo messages and runs Browser's coalesced UI updates.
  void Settle() { task_environment()->FastForwardBy(base::Seconds(1)); }

  TabStripModel* tab_strip_model() { return browser()->GetTabStripModel(); }
  SpaceModel& space_model() {
    return WindowFeatures::From(browser())->space_model();
  }

  FakePage page_;
  mojo::Remote<sidebar::mojom::PageHandler> handler_remote_;
  std::unique_ptr<SidebarPageHandler> handler_;
  std::unique_ptr<BrowserWindowInterface> parking_;
};

TEST_F(SidebarPageHandlerTest, SendsTheTreeOnRequest) {
  sidebar::mojom::TreePtr tree = GetTree();
  ASSERT_TRUE(tree);

  EXPECT_EQ(tree->pinned->type, sidebar::mojom::NodeType::kRoot);
  EXPECT_THAT(tree->pinned->children, IsEmpty());
  EXPECT_THAT(Hosts(*tree->today), ElementsAre("a.test", "b.test"));
  EXPECT_EQ(tree->active_id, tree->today->children[0]->id);
}

TEST_F(SidebarPageHandlerTest, ActivatesNode) {
  handler_remote_->ActivateNode(TodayId(1));
  Settle();

  EXPECT_EQ(tab_strip_model()->active_index(), 1);
  EXPECT_THAT(page_.TakeEvents(), ElementsAre("active b.test"));
}

TEST_F(SidebarPageHandlerTest, ClosesNode) {
  handler_remote_->CloseNode(TodayId(0));
  Settle();

  EXPECT_EQ(tab_strip_model()->count(), 1);
  // The tab goes first, then its node.
  EXPECT_THAT(
      page_.TakeEvents(),
      ElementsAre("a.test unloaded", "removed a.test", "active b.test"));
}

TEST_F(SidebarPageHandlerTest, ClosesEveryPaneOfASplit) {
  space_model().Split(space_model().today_root()->children()[0].get(),
                      space_model().today_root()->children()[1].get());
  Settle();
  page_.TakeEvents();

  handler_remote_->CloseNode(TodayId(0));
  Settle();

  EXPECT_EQ(tab_strip_model()->count(), 0);
  EXPECT_THAT(space_model().today_root()->children(), IsEmpty());
}

TEST_F(SidebarPageHandlerTest, IgnoresUnknownNodeIds) {
  handler_remote_->ActivateNode("not-a-uuid");
  handler_remote_->CloseNode("00000000-0000-4000-8000-000000000000");
  Settle();

  EXPECT_EQ(tab_strip_model()->count(), 2);
  EXPECT_EQ(tab_strip_model()->active_index(), 0);
}

TEST_F(SidebarPageHandlerTest, SendsDiffsForNewTabs) {
  AddTab(browser(), GURL("https://c.test/"));
  Settle();

  // The node is added before its navigation commits, so with an empty URL,
  // and before its tab is registered. The commit then changes its URL, and
  // then its title.
  EXPECT_THAT(page_.TakeEvents(),
              ElementsAre("added  at 0", " live", "active ", "changed c.test",
                          "changed c.test"));
}

TEST_F(SidebarPageHandlerTest, SendsDiffsForModelMoves) {
  space_model().Move(space_model().today_root()->children()[1].get(),
                     space_model().today_root(), 0);
  Settle();

  EXPECT_THAT(page_.TakeEvents(), ElementsAre("moved b.test to 0"));
  // The bridge follows the model.
  EXPECT_EQ(tab_strip_model()->GetWebContentsAt(0)->GetLastCommittedURL(),
            GURL("https://b.test/"));
}

TEST_F(SidebarPageHandlerTest, MoveNodeToPinnedPinsTheTab) {
  handler_remote_->MoveNode(TodayId(1), PinnedId(), 0);
  Settle();

  ASSERT_EQ(space_model().pinned_root()->children().size(), 1u);
  EXPECT_EQ(space_model().pinned_root()->children()[0]->url(),
            GURL("https://b.test/"));
  EXPECT_EQ(tab_strip_model()->GetWebContentsAt(0)->GetLastCommittedURL(),
            GURL("https://b.test/"));
}

TEST_F(SidebarPageHandlerTest, MoveNodeCountsSlotsWithTheNodeInPlace) {
  // Slot 2 is after b, the last entry.
  handler_remote_->MoveNode(TodayId(0), TodayRootId(), 2);
  Settle();

  EXPECT_THAT(page_.TakeEvents(), ElementsAre("moved a.test to 1"));
}

TEST_F(SidebarPageHandlerTest, IgnoresInvalidMovesAndFolders) {
  handler_remote_->AddFolder(TodayRootId(), 0, "Not here");
  handler_remote_->MoveNode(TodayId(0), TodayRootId(), 7);
  handler_remote_->MoveNode(TodayId(0), TodayId(1), 0);
  Settle();

  EXPECT_THAT(page_.TakeEvents(), IsEmpty());
}

TEST_F(SidebarPageHandlerTest, AddsAndRenamesFolders) {
  handler_remote_->AddFolder(PinnedId(), 0, "Work");
  Settle();
  ASSERT_EQ(space_model().pinned_root()->children().size(), 1u);
  SidebarNode* folder = space_model().pinned_root()->children()[0].get();
  EXPECT_EQ(folder->type(), SidebarNode::Type::kFolder);

  handler_remote_->RenameFolder(folder->id().AsLowercaseString(), "Play");
  Settle();

  EXPECT_EQ(folder->title(), u"Play");
}

TEST_F(SidebarPageHandlerTest, ActivatingAnUnloadedPinnedNodeOpensIt) {
  handler_remote_->MoveNode(TodayId(0), PinnedId(), 0);
  Settle();
  const std::string pinned_id =
      space_model().pinned_root()->children()[0]->id().AsLowercaseString();
  handler_remote_->CloseNode(pinned_id);
  Settle();
  ASSERT_EQ(tab_strip_model()->count(), 1);
  page_.TakeEvents();

  handler_remote_->ActivateNode(pinned_id);
  Settle();

  EXPECT_EQ(tab_strip_model()->count(), 2);
  EXPECT_EQ(tab_strip_model()->active_index(), 0);
  EXPECT_THAT(page_.TakeEvents(), ElementsAre("a.test live", "active a.test"));
}

TEST_F(SidebarPageHandlerTest, DeleteNodeRemovesPinnedEntriesOnly) {
  handler_remote_->DeleteNode(TodayId(0));
  handler_remote_->MoveNode(TodayId(1), PinnedId(), 0);
  Settle();
  page_.TakeEvents();

  handler_remote_->DeleteNode(
      space_model().pinned_root()->children()[0]->id().AsLowercaseString());
  Settle();

  EXPECT_THAT(space_model().pinned_root()->children(), IsEmpty());
  EXPECT_EQ(tab_strip_model()->count(), 1);
}

TEST_F(SidebarPageHandlerTest, SwitchesSpacesAndSendsTheirTree) {
  EXPECT_EQ(page_.spaces(), "[Home]");

  handler_remote_->AddSpace();
  Settle();

  EXPECT_EQ(page_.spaces(), "Home,[Space 2]");
  sidebar::mojom::TreePtr tree = page_.TakeTree();
  ASSERT_TRUE(tree);
  EXPECT_TRUE(tree->pinned->children.empty());
  // A new tab, as the space had nothing to show.
  EXPECT_EQ(tree->today->children.size(), 1u);

  const std::string first_id = WindowFeatures::From(browser())
                                   ->service()
                                   .space_list()
                                   .spaces()[0]
                                   ->id()
                                   .AsLowercaseString();
  handler_remote_->SwitchSpace(first_id);
  Settle();

  EXPECT_EQ(page_.spaces(), "[Home],Space 2");
  tree = page_.TakeTree();
  ASSERT_TRUE(tree);
  EXPECT_THAT(Hosts(*tree->today), ElementsAre("a.test", "b.test"));
}

}  // namespace
}  // namespace strata
