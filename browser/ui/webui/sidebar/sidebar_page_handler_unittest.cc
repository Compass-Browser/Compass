// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/sidebar/sidebar_page_handler.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
  void ActiveNodeChanged(const std::optional<std::string>& id) override {
    events_.push_back("active " + (id ? hosts_[*id] : "none"));
  }

  sidebar::mojom::TreePtr TakeTree() { return std::move(tree_); }
  std::vector<std::string> TakeEvents() { return std::move(events_); }

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

  // Delivers Mojo messages and runs Browser's coalesced UI updates.
  void Settle() { task_environment()->FastForwardBy(base::Seconds(1)); }

  TabStripModel* tab_strip_model() { return browser()->GetTabStripModel(); }
  SpaceModel& space_model() {
    return WindowFeatures::From(browser())->space_model();
  }

  FakePage page_;
  mojo::Remote<sidebar::mojom::PageHandler> handler_remote_;
  std::unique_ptr<SidebarPageHandler> handler_;
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
  EXPECT_THAT(page_.TakeEvents(),
              ElementsAre("removed a.test", "active b.test"));
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

  // The node is added before its navigation commits, so with an empty URL.
  // The commit then changes its URL, and then its title.
  EXPECT_THAT(page_.TakeEvents(),
              ElementsAre("added  at 0", "active ", "changed c.test",
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

}  // namespace
}  // namespace strata
