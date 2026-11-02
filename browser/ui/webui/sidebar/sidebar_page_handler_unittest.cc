// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sidebar/sidebar_page_handler.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/recently_audible_helper.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/testing_profile_manager.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/test/test_clipboard.h"
#include "ui/base/models/menu_model.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/menus/simple_menu_model.h"
#include "url/gurl.h"

namespace compass {
namespace {

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;

using Command = SidebarPageHandler::MenuCommand;

// Records what the browser pushes to the page, one line per call. Nodes are
// named by their URL's host, which the tests keep unique.
class FakePage : public sidebar::mojom::Page {
 public:
  mojo::PendingRemote<sidebar::mojom::Page> BindAndPassRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  // sidebar::mojom::Page:
  void SetSpaces(std::vector<sidebar::mojom::SpacePtr> spaces,
                 const std::string& active_id,
                 std::vector<sidebar::mojom::ProfileOptionPtr> profiles)
      override {
    profiles_ = std::move(profiles);
    spaces_.clear();
    for (size_t i = 0; i < spaces.size(); ++i) {
      if (spaces[i]->id == active_id) {
        archive_after_hours_ = spaces[i]->archive_after_hours;
      }
      // Another profile's are starred.
      const std::string name =
          spaces[i]->name + (spaces[i]->own_profile ? "" : "*");
      spaces_ += (i ? "," : "") +
                 (spaces[i]->id == active_id ? "[" + name + "]" : name);
    }
  }
  void SetTree(sidebar::mojom::TreePtr tree) override {
    Learn(*tree->favorites);
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
                   const std::string& custom_title,
                   const GURL& url,
                   bool collapsed) override {
    hosts_[id] = std::string(url.host());
    // Folders have no host: their title, and whether they're collapsed.
    events_.push_back("changed " +
                      (url.is_empty() ? title + (collapsed ? " collapsed" : "")
                                      : hosts_[id]));
  }
  void NodeRemoved(const std::string& id) override {
    events_.push_back("removed " + hosts_[id]);
  }
  void NodeLiveChanged(const std::string& id, bool live) override {
    events_.push_back(hosts_[id] + (live ? " live" : " unloaded"));
  }
  void NodeAudioChanged(const std::string& id,
                        sidebar::mojom::AudioState audio) override {
    switch (audio) {
      case sidebar::mojom::AudioState::kNone:
        events_.push_back(hosts_[id] + " silent");
        break;
      case sidebar::mojom::AudioState::kAudible:
        events_.push_back(hosts_[id] + " audible");
        break;
      case sidebar::mojom::AudioState::kMuted:
        events_.push_back(hosts_[id] + " muted");
        break;
    }
  }
  void ActiveNodeChanged(const std::optional<std::string>& id) override {
    events_.push_back("active " + (id ? hosts_[*id] : "none"));
  }
  void SetArchive(
      std::vector<sidebar::mojom::ArchivedEntryPtr> entries) override {
    archive_ = std::move(entries);
  }
  void StartRename(const std::string& id) override {
    events_.push_back("rename " + id);
  }
  // No tracker without a page: see now_playing_browsertest.cc.
  void SetNowPlaying(
      std::vector<sidebar::mojom::NowPlayingPtr> items) override {}

  sidebar::mojom::TreePtr TakeTree() { return std::move(tree_); }
  std::vector<std::string> TakeEvents() { return std::move(events_); }
  // Space names, comma separated, the window's in brackets, other profiles'
  // starred.
  const std::string& spaces() const { return spaces_; }
  const std::vector<sidebar::mojom::ArchivedEntryPtr>& archive() const {
    return archive_;
  }
  uint32_t archive_after_hours() const { return archive_after_hours_; }
  const std::vector<sidebar::mojom::ProfileOptionPtr>& profiles() const {
    return profiles_;
  }

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
  uint32_t archive_after_hours_ = 0;
  std::vector<sidebar::mojom::ProfileOptionPtr> profiles_;
  std::vector<sidebar::mojom::ArchivedEntryPtr> archive_;
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

  SpaceModel& favorites() {
    return WindowFeatures::From(browser())->service().space_list().favorites();
  }
  std::string FavoritesId() {
    return favorites().pinned_root()->id().AsLowercaseString();
  }

  std::string PinnedId() {
    return space_model().pinned_root()->id().AsLowercaseString();
  }
  std::string TodayRootId() {
    return space_model().today_root()->id().AsLowercaseString();
  }

  // The commands of node |id|'s context menu, separators left out.
  std::vector<int> MenuCommands(const std::string& id) {
    std::vector<int> commands;
    std::unique_ptr<ui::SimpleMenuModel> menu = handler_->CreateContextMenu(id);
    for (size_t i = 0; menu && i < menu->GetItemCount(); ++i) {
      if (menu->GetTypeAt(i) != ui::MenuModel::TYPE_SEPARATOR) {
        commands.push_back(menu->GetCommandIdAt(i));
      }
    }
    return commands;
  }

  // Chooses |command| in node |id|'s context menu.
  void Choose(const std::string& id, int command) {
    std::unique_ptr<ui::SimpleMenuModel> menu = handler_->CreateContextMenu(id);
    ASSERT_TRUE(menu);
    std::optional<size_t> index = menu->GetIndexOfCommandId(command);
    ASSERT_TRUE(index) << "No command " << command;
    menu->ActivatedAt(*index);
    Settle();
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
  // Not in a tab, nor past the end.
  handler_remote_->AddFolder(TodayId(0), 0, "Not here");
  handler_remote_->AddFolder(TodayRootId(), 9, "Nor here");
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

TEST_F(SidebarPageHandlerTest, CollapsesFolders) {
  SidebarNode* folder =
      space_model().AddFolder(space_model().pinned_root(), 0, u"Work");
  const std::string id = folder->id().AsLowercaseString();
  Settle();
  page_.TakeEvents();

  handler_remote_->SetFolderCollapsed(id, true);
  Settle();
  EXPECT_TRUE(folder->collapsed());
  EXPECT_THAT(page_.TakeEvents(), ElementsAre("changed Work collapsed"));

  // The tree carries it, for a page that loads later.
  handler_remote_->RequestTree();
  Settle();
  sidebar::mojom::TreePtr tree = page_.TakeTree();
  ASSERT_TRUE(tree);
  ASSERT_EQ(tree->pinned->children.size(), 1u);
  EXPECT_TRUE(tree->pinned->children[0]->collapsed);

  // Again is a no-op; only folders collapse.
  handler_remote_->SetFolderCollapsed(id, true);
  handler_remote_->SetFolderCollapsed(TodayId(0), true);
  Settle();
  EXPECT_THAT(page_.TakeEvents(), IsEmpty());

  handler_remote_->SetFolderCollapsed(id, false);
  Settle();
  EXPECT_FALSE(folder->collapsed());
  EXPECT_THAT(page_.TakeEvents(), ElementsAre("changed Work"));
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

TEST_F(SidebarPageHandlerTest, FoldersInToday) {
  handler_remote_->AddFolder(TodayRootId(), 0, "Trip");
  Settle();
  ASSERT_EQ(space_model().today_root()->children().size(), 3u);
  SidebarNode* folder = space_model().today_root()->children()[0].get();
  ASSERT_EQ(folder->type(), SidebarNode::Type::kFolder);
  const std::string id = folder->id().AsLowercaseString();

  // Tabs go in, live, and keep the strip in the tree's order.
  handler_remote_->MoveNode(TodayId(2), id, 0);
  Settle();
  ASSERT_EQ(folder->children().size(), 1u);
  EXPECT_EQ(folder->children()[0]->url(), GURL("https://b.test/"));
  EXPECT_EQ(tab_strip_model()->count(), 2);
  EXPECT_EQ(tab_strip_model()->GetWebContentsAt(0)->GetVisibleURL(),
            GURL("https://b.test/"));

  // It can be pinned and unpinned.
  EXPECT_THAT(MenuCommands(id), Contains(Command::kPin));
  Choose(id, Command::kPin);
  EXPECT_EQ(space_model().pinned_root()->children()[0].get(), folder);
  Choose(id, Command::kUnpin);
  EXPECT_EQ(space_model().today_root()->children()[0].get(), folder);

  // Deleting it closes its tabs.
  handler_remote_->DeleteNode(id);
  Settle();
  EXPECT_EQ(space_model().today_root()->children().size(), 1u);
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

TEST_F(SidebarPageHandlerTest, SendsAnotherSpacesTreeAndPreviewsIt) {
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");
  const std::string work_id = work->id().AsLowercaseString();
  handler_remote_->MoveNodeToSpace(TodayId(1), work_id);
  Settle();

  // For a swipe to show before it switches.
  sidebar::mojom::TreePtr tree;
  handler_remote_->GetSpaceTree(
      work_id, base::BindLambdaForTesting(
                   [&](sidebar::mojom::TreePtr t) { tree = std::move(t); }));
  Settle();
  ASSERT_TRUE(tree);
  EXPECT_THAT(Hosts(*tree->today), ElementsAre("b.test"));
  // The entry the switch would show: b, whose tab waits in the parking
  // window.
  EXPECT_EQ(tree->active_id, tree->today->children[0]->id);
  bool answered = false;
  handler_remote_->GetSpaceTree(
      "not-a-space", base::BindLambdaForTesting([&](sidebar::mojom::TreePtr t) {
        answered = true;
        EXPECT_FALSE(t);
      }));
  Settle();
  EXPECT_TRUE(answered);

  // The tint follows the swipe.
  handler_remote_->PreviewSpace(work_id, 0.4);
  Settle();
  EXPECT_EQ(features->preview_space(), work);
  EXPECT_DOUBLE_EQ(features->preview_progress(), 0.4);
  // Not the window's own space.
  handler_remote_->PreviewSpace(space_model().id().AsLowercaseString(), 0.5);
  Settle();
  EXPECT_FALSE(features->preview_space());
  // Switching ends it.
  handler_remote_->PreviewSpace(work_id, 0.9);
  handler_remote_->SwitchSpace(work_id);
  Settle();
  EXPECT_EQ(&features->space_model(), work);
  EXPECT_FALSE(features->preview_space());
  EXPECT_EQ(features->preview_progress(), 0);
}

TEST_F(SidebarPageHandlerTest, RenamesAndColorsSpaces) {
  SpaceModel& home = space_model();
  const std::string id = home.id().AsLowercaseString();

  handler_remote_->RenameSpace(id, "  Work ");
  handler_remote_->RenameSpace(id, " ");
  handler_remote_->SetSpaceColor(id,
                                 sidebar::mojom::SpaceColor::New(34, 197, 94));
  Settle();

  EXPECT_EQ(page_.spaces(), "[Work]");
  EXPECT_EQ(home.name(), u"Work");
  EXPECT_EQ(home.color(), SpaceColor::kGreen);

  // Any color, and back to grey.
  handler_remote_->SetSpaceColor(id,
                                 sidebar::mojom::SpaceColor::New(1, 2, 3));
  Settle();
  EXPECT_EQ(home.color(), SpaceColor::FromRGB(1, 2, 3));
  handler_remote_->SetSpaceColor(id, nullptr);
  Settle();
  EXPECT_EQ(home.color(), SpaceColor::kGrey);
}

TEST_F(SidebarPageHandlerTest, SetsSpaceGradients) {
  SpaceModel& home = space_model();
  const std::string id = home.id().AsLowercaseString();
  home.SetColor(SpaceColor::kBlue);

  std::vector<sidebar::mojom::SpaceColorPtr> gradient;
  gradient.push_back(sidebar::mojom::SpaceColor::New(236, 72, 153));
  gradient.push_back(sidebar::mojom::SpaceColor::New(1, 2, 3));
  handler_remote_->SetSpaceGradient(id, std::move(gradient));
  Settle();
  EXPECT_EQ(home.gradient(), (std::vector<SpaceColor>{
                                 SpaceColor::kPink, SpaceColor::FromRGB(1, 2, 3)}));

  handler_remote_->SetSpaceGradient(id, {});
  Settle();
  EXPECT_TRUE(home.gradient().empty());
}

TEST_F(SidebarPageHandlerTest, SetsSpaceIntensityAndGrain) {
  SpaceModel& home = space_model();
  const std::string id = home.id().AsLowercaseString();

  handler_remote_->SetSpaceIntensity(id, 0.6);
  handler_remote_->SetSpaceGrain(id, 5);
  Settle();

  EXPECT_EQ(home.intensity(), 0.6);
  EXPECT_EQ(home.grain(), 1);

  handler_remote_->SetSpaceAppearance(id,
                                      sidebar::mojom::SpaceAppearance::kDark);
  Settle();
  EXPECT_EQ(home.appearance(), SpaceAppearance::kDark);
}

TEST_F(SidebarPageHandlerTest, SetsSpaceIcons) {
  SpaceModel& home = space_model();
  const std::string id = home.id().AsLowercaseString();

  handler_remote_->SetSpaceIcon(id,
                                sidebar::mojom::SpaceIcon::NewSymbol("home"));
  Settle();
  EXPECT_EQ(home.icon(), *SpaceIcon::Symbol("home"));

  // Invalid ones are ignored.
  handler_remote_->SetSpaceIcon(id, sidebar::mojom::SpaceIcon::NewEmoji(" "));
  handler_remote_->SetSpaceIcon(id,
                                sidebar::mojom::SpaceIcon::NewSymbol("../x"));
  Settle();
  EXPECT_EQ(home.icon(), *SpaceIcon::Symbol("home"));

  handler_remote_->SetSpaceIcon(id, sidebar::mojom::SpaceIcon::NewEmoji("🚀"));
  Settle();
  EXPECT_EQ(home.icon(), *SpaceIcon::Emoji("🚀"));

  handler_remote_->SetSpaceIcon(id, nullptr);
  Settle();
  EXPECT_TRUE(home.icon().is_none());
}

TEST_F(SidebarPageHandlerTest, MovesAnEntryAndItsTabToAnotherSpace) {
  SpaceList& list = WindowFeatures::From(browser())->service().space_list();
  SpaceModel& home = space_model();
  SpaceModel* work = list.AddSpace(1, u"Work");
  tabs::TabInterface* a = tab_strip_model()->GetActiveTab();
  page_.TakeEvents();

  // The window's active tab.
  handler_remote_->MoveNodeToSpace(TodayId(0), work->id().AsLowercaseString());
  Settle();

  // The window shows b; a waits in the parking window, still a's.
  EXPECT_THAT(page_.TakeEvents(), ElementsAre("active b.test", "removed a.test"));
  ASSERT_EQ(home.today_root()->children().size(), 1u);
  ASSERT_EQ(work->today_root()->children().size(), 1u);
  SidebarNode* moved = work->today_root()->children()[0].get();
  EXPECT_EQ(moved->url(), GURL("https://a.test/"));
  EXPECT_EQ(WindowFeatures::From(browser())->registry().GetTab(moved), a);
  ASSERT_TRUE(parking_);
  EXPECT_EQ(a->GetBrowserWindowInterface(), parking_.get());

  // It comes back with the space.
  handler_remote_->SwitchSpace(work->id().AsLowercaseString());
  Settle();
  EXPECT_EQ(tab_strip_model()->GetActiveTab(), a);
}

TEST_F(SidebarPageHandlerTest, DeletesSpacesButNotTheLastOne) {
  SpaceList& list = WindowFeatures::From(browser())->service().space_list();
  const std::string home_id = space_model().id().AsLowercaseString();
  handler_remote_->AddSpace();
  Settle();
  ASSERT_EQ(page_.spaces(), "Home,[Space 2]");
  ASSERT_TRUE(parking_);
  EXPECT_EQ(parking_->GetTabStripModel()->count(), 2);

  // Another space: its parked tabs close.
  handler_remote_->DeleteSpace(home_id);
  Settle();
  EXPECT_EQ(page_.spaces(), "[Space 2]");
  EXPECT_EQ(list.spaces().size(), 1u);
  EXPECT_TRUE(!parking_ || parking_->GetTabStripModel()->empty());

  // The last one stays.
  handler_remote_->DeleteSpace(space_model().id().AsLowercaseString());
  Settle();
  EXPECT_EQ(page_.spaces(), "[Space 2]");
}

TEST_F(SidebarPageHandlerTest, DeletingTheWindowsSpaceSwitchesAway) {
  const std::string home_id = space_model().id().AsLowercaseString();
  handler_remote_->AddSpace();
  Settle();
  page_.TakeTree();

  handler_remote_->DeleteSpace(space_model().id().AsLowercaseString());
  Settle();

  EXPECT_EQ(page_.spaces(), "[Home]");
  EXPECT_EQ(space_model().id().AsLowercaseString(), home_id);
  sidebar::mojom::TreePtr tree = page_.TakeTree();
  ASSERT_TRUE(tree);
  EXPECT_THAT(Hosts(*tree->today), ElementsAre("a.test", "b.test"));
}

TEST_F(SidebarPageHandlerTest, ArchivesAndRestores) {
  const std::string id = space_model().id().AsLowercaseString();
  EXPECT_EQ(page_.archive_after_hours(), 12u);
  handler_remote_->SetArchiveAfter(id, 24);
  // More than a year is ignored.
  handler_remote_->SetArchiveAfter(id, 366 * 24);
  Settle();
  EXPECT_EQ(page_.archive_after_hours(), 24u);
  EXPECT_EQ(space_model().archive_after(), base::Hours(24));

  // b.test isn't shown.
  space_model().Archive(space_model().today_root()->children()[1].get(),
                        base::Time::Now());
  Settle();
  ASSERT_EQ(page_.archive().size(), 1u);
  EXPECT_EQ(page_.archive()[0]->tabs[0]->url, GURL("https://b.test/"));
  EXPECT_EQ(tab_strip_model()->count(), 1);

  handler_remote_->RestoreArchived(page_.archive()[0]->id);
  Settle();
  EXPECT_THAT(page_.archive(), IsEmpty());
  ASSERT_EQ(space_model().today_root()->children().size(), 2u);
  EXPECT_EQ(tab_strip_model()->count(), 2);
  EXPECT_EQ(tab_strip_model()->GetActiveWebContents()->GetVisibleURL(),
            GURL("https://b.test/"));

  space_model().Archive(space_model().today_root()->children()[1].get(),
                        base::Time::Now());
  Settle();
  ASSERT_EQ(page_.archive().size(), 1u);
  handler_remote_->ClearArchive();
  Settle();
  EXPECT_THAT(page_.archive(), IsEmpty());
}

TEST_F(SidebarPageHandlerTest, SendsTheFavoritesInTheTree) {
  favorites().AddTab(favorites().pinned_root(), 0, GURL("https://f.test/"),
                     u"F");
  sidebar::mojom::TreePtr tree = GetTree();
  EXPECT_EQ(tree->favorites->id, FavoritesId());
  EXPECT_THAT(Hosts(*tree->favorites), ElementsAre("f.test"));
  EXPECT_FALSE(tree->favorites->children[0]->live);
}

TEST_F(SidebarPageHandlerTest, MovesEntriesIntoWithinAndOutOfFavorites) {
  const std::string a = TodayId(0);
  const std::string b = TodayId(1);
  page_.TakeEvents();

  handler_remote_->MoveNode(b, FavoritesId(), 0);
  Settle();
  EXPECT_THAT(page_.TakeEvents(),
              ElementsAre("removed b.test", "added b.test at 0"));
  // Its tab moved to the front of the strip.
  EXPECT_EQ(tab_strip_model()->GetWebContentsAt(0)->GetLastCommittedURL(),
            GURL("https://b.test/"));

  handler_remote_->MoveNode(a, FavoritesId(), 1);
  Settle();
  // Slots count with the node in place, as elsewhere: after a is 2.
  handler_remote_->MoveNode(b, FavoritesId(), 2);
  Settle();
  EXPECT_THAT(Hosts(*GetTree()->favorites), ElementsAre("a.test", "b.test"));

  handler_remote_->MoveNode(a, PinnedId(), 0);
  Settle();
  EXPECT_THAT(Hosts(*GetTree()->favorites), ElementsAre("b.test"));
  ASSERT_EQ(space_model().pinned_root()->children().size(), 1u);
  EXPECT_EQ(space_model().pinned_root()->children()[0]->id().AsLowercaseString(),
            a);
  // Its tab stayed: this window shows the space it went to.
  EXPECT_EQ(tab_strip_model()->count(), 2);
}

TEST_F(SidebarPageHandlerTest, FavoritesTakeOnlyTabs) {
  handler_remote_->AddFolder(PinnedId(), 0, "Folder");
  Settle();
  const std::string folder =
      space_model().pinned_root()->children()[0]->id().AsLowercaseString();

  handler_remote_->MoveNode(folder, FavoritesId(), 0);
  handler_remote_->AddFolder(FavoritesId(), 0, "Not here");
  Settle();

  EXPECT_TRUE(favorites().pinned_root()->children().empty());
}

TEST_F(SidebarPageHandlerTest, ClosingAnUnloadedFavoriteRemovesIt) {
  const std::string b = TodayId(1);
  handler_remote_->MoveNode(b, FavoritesId(), 0);
  Settle();

  // The first close unloads it, the second removes it.
  handler_remote_->CloseNode(b);
  Settle();
  ASSERT_EQ(favorites().pinned_root()->children().size(), 1u);
  EXPECT_EQ(tab_strip_model()->count(), 1);

  handler_remote_->CloseNode(b);
  Settle();
  EXPECT_TRUE(favorites().pinned_root()->children().empty());
}

TEST_F(SidebarPageHandlerTest, TodayEntryMenu) {
  EXPECT_THAT(MenuCommands(TodayId(0)),
              ElementsAre(Command::kRename, Command::kCopyLink,
                          Command::kDuplicate, Command::kReload, Command::kMute,
                          Command::kPin, Command::kAddToFavorites,
                          Command::kCloseTab));
  EXPECT_THAT(MenuCommands(TodayRootId()), IsEmpty());
  EXPECT_THAT(MenuCommands("not-a-uuid"), IsEmpty());

  Choose(TodayId(0), Command::kCloseTab);
  EXPECT_EQ(tab_strip_model()->count(), 1);
  EXPECT_EQ(space_model().today_root()->children().size(), 1u);
}

TEST_F(SidebarPageHandlerTest, PinsAndUnpinsFromTheMenu) {
  const std::string b = TodayId(1);
  Choose(b, Command::kPin);
  ASSERT_EQ(space_model().pinned_root()->children().size(), 1u);
  EXPECT_EQ(space_model().pinned_root()->children()[0]->id().AsLowercaseString(),
            b);
  // a is the window's active entry: b can show next to it.
  EXPECT_THAT(MenuCommands(b),
              ElementsAre(Command::kOpenInSplit, Command::kRename,
                          Command::kCopyLink, Command::kDuplicate,
                          Command::kReload, Command::kMute, Command::kUnpin,
                          Command::kAddToFavorites, Command::kCloseTab,
                          Command::kRemove));

  // Closing a pinned tab leaves the entry; Remove takes it.
  Choose(b, Command::kCloseTab);
  EXPECT_EQ(tab_strip_model()->count(), 1);
  EXPECT_THAT(MenuCommands(b), Not(Contains(Command::kCloseTab)));
  EXPECT_THAT(MenuCommands(b), Not(Contains(Command::kReload)));
  EXPECT_THAT(MenuCommands(b), Not(Contains(Command::kMute)));

  // Unpinned entries go to the top of Today.
  Choose(b, Command::kUnpin);
  EXPECT_THAT(space_model().pinned_root()->children(), IsEmpty());
  EXPECT_EQ(TodayId(0), b);
  // Unloaded in Today, it can only be removed.
  EXPECT_THAT(MenuCommands(b), Contains(Command::kCloseTab));
  Choose(b, Command::kCloseTab);
  EXPECT_EQ(space_model().today_root()->children().size(), 1u);
}

TEST_F(SidebarPageHandlerTest, RemovesPinnedEntriesFromTheMenu) {
  const std::string a = TodayId(0);
  Choose(a, Command::kPin);
  Choose(a, Command::kRemove);

  EXPECT_THAT(space_model().pinned_root()->children(), IsEmpty());
  EXPECT_EQ(tab_strip_model()->count(), 1);
}

TEST_F(SidebarPageHandlerTest, AddsToAndRemovesFromFavoritesFromTheMenu) {
  const std::string b = TodayId(1);
  Choose(b, Command::kAddToFavorites);
  ASSERT_EQ(favorites().pinned_root()->children().size(), 1u);
  // Favorites aren't pinned or moved between spaces.
  EXPECT_THAT(MenuCommands(b),
              ElementsAre(Command::kCopyLink, Command::kDuplicate,
                          Command::kReload, Command::kMute, Command::kCloseTab,
                          Command::kRemove));

  Choose(b, Command::kRemove);
  EXPECT_THAT(favorites().pinned_root()->children(), IsEmpty());
  EXPECT_EQ(tab_strip_model()->count(), 1);
}

TEST_F(SidebarPageHandlerTest, MovesToAnotherSpaceFromTheMenu) {
  EXPECT_THAT(MenuCommands(TodayId(0)), Not(Contains(Command::kMoveToSpace)));
  SpaceList& list = WindowFeatures::From(browser())->service().space_list();
  SpaceModel* work = list.AddSpace(list.spaces().size(), u"Work");
  const std::string a = TodayId(0);

  std::unique_ptr<ui::SimpleMenuModel> menu = handler_->CreateContextMenu(a);
  std::optional<size_t> index = menu->GetIndexOfCommandId(Command::kMoveToSpace);
  ASSERT_TRUE(index);
  ui::MenuModel* spaces = menu->GetSubmenuModelAt(*index);
  // Only the other spaces.
  ASSERT_EQ(spaces->GetItemCount(), 1u);
  EXPECT_EQ(spaces->GetLabelAt(0), u"Work");
  spaces->ActivatedAt(0);
  Settle();

  EXPECT_EQ(space_model().today_root()->children().size(), 1u);
  ASSERT_EQ(work->today_root()->children().size(), 1u);
  EXPECT_EQ(work->today_root()->children()[0]->url(), GURL("https://a.test/"));
}

TEST_F(SidebarPageHandlerTest, DuplicatesEntriesFromTheMenu) {
  // A live tab is copied next to it.
  Choose(TodayId(0), Command::kDuplicate);
  EXPECT_EQ(tab_strip_model()->count(), 3);
  EXPECT_THAT(Hosts(*GetTree()->today),
              ElementsAre("a.test", "a.test", "b.test"));

  // An unloaded one gets a new entry, opened.
  const std::string b = TodayId(2);
  Choose(b, Command::kPin);
  Choose(b, Command::kCloseTab);
  ASSERT_EQ(tab_strip_model()->count(), 2);
  Choose(b, Command::kDuplicate);
  EXPECT_EQ(tab_strip_model()->count(), 3);
  EXPECT_THAT(Hosts(*GetTree()->today),
              ElementsAre("b.test", "a.test", "a.test"));
  EXPECT_EQ(tab_strip_model()->GetActiveWebContents()->GetVisibleURL(),
            GURL("https://b.test/"));
}

TEST_F(SidebarPageHandlerTest, CopiesLinksFromTheMenu) {
  ui::TestClipboard::CreateForCurrentThread();
  Choose(TodayId(1), Command::kCopyLink);

  std::u16string text;
  ui::Clipboard::GetForCurrentThread()->ReadText(
      ui::ClipboardBuffer::kCopyPaste, std::nullopt,
      base::BindLambdaForTesting(
          [&](std::u16string result) { text = std::move(result); }));
  EXPECT_EQ(text, u"https://b.test/");
  ui::Clipboard::DestroyClipboardForCurrentThread();
}

TEST_F(SidebarPageHandlerTest, MutesSilentTabsFromTheMenu) {
  const std::string a = TodayId(0);
  content::WebContents* contents = tab_strip_model()->GetWebContentsAt(0);
  page_.TakeEvents();

  Choose(a, Command::kMute);
  EXPECT_TRUE(contents->IsAudioMuted());
  // The entry is muted too (saved, so it reopens muted).
  EXPECT_THAT(page_.TakeEvents(),
              ElementsAre("a.test muted", "changed a.test"));
  EXPECT_EQ(GetTree()->today->children[0]->audio,
            sidebar::mojom::AudioState::kMuted);
  EXPECT_THAT(MenuCommands(a), AllOf(Contains(Command::kUnmute),
                                     Not(Contains(Command::kMute))));

  Choose(a, Command::kUnmute);
  EXPECT_FALSE(contents->IsAudioMuted());
  EXPECT_THAT(page_.TakeEvents(),
              ElementsAre("a.test silent", "changed a.test"));
}

// An entry stays muted while its tab is closed (docs/design/sidebar.md,
// "Sound").
TEST_F(SidebarPageHandlerTest, AMutedEntryReopensMuted) {
  handler_remote_->MoveNode(TodayId(0), PinnedId(), 0);
  Settle();
  SidebarNode* node = space_model().pinned_root()->children()[0].get();
  const std::string id = node->id().AsLowercaseString();
  EXPECT_FALSE(node->muted());

  Choose(id, Command::kMute);
  EXPECT_TRUE(node->muted());

  handler_remote_->CloseNode(id);
  Settle();
  ASSERT_EQ(tab_strip_model()->count(), 1);
  EXPECT_TRUE(node->muted());

  handler_remote_->ActivateNode(id);
  Settle();
  ASSERT_EQ(tab_strip_model()->count(), 2);
  content::WebContents* contents = tab_strip_model()->GetWebContentsAt(0);
  EXPECT_TRUE(contents->IsAudioMuted());
  EXPECT_EQ(GetTree()->pinned->children[0]->audio,
            sidebar::mojom::AudioState::kMuted);

  // Unmuting the tab unmutes the entry.
  Choose(id, Command::kUnmute);
  EXPECT_FALSE(contents->IsAudioMuted());
  EXPECT_FALSE(node->muted());
}

TEST_F(SidebarPageHandlerTest, ShowsAndMutesPlayingTabs) {
  const std::string a = TodayId(0);
  content::WebContents* contents = tab_strip_model()->GetWebContentsAt(0);
  RecentlyAudibleHelper* audible =
      RecentlyAudibleHelper::FromWebContents(contents);
  ASSERT_TRUE(audible);
  page_.TakeEvents();

  audible->SetCurrentlyAudibleForTesting();
  Settle();
  EXPECT_THAT(page_.TakeEvents(), ElementsAre("a.test audible"));
  EXPECT_EQ(GetTree()->today->children[0]->audio,
            sidebar::mojom::AudioState::kAudible);

  // Its speaker mutes it, and unmutes it.
  handler_remote_->SetNodeMuted(a, true);
  Settle();
  EXPECT_TRUE(contents->IsAudioMuted());
  EXPECT_THAT(page_.TakeEvents(),
              ElementsAre("a.test muted", "changed a.test"));
  handler_remote_->SetNodeMuted(a, false);
  Settle();
  EXPECT_FALSE(contents->IsAudioMuted());
  EXPECT_THAT(page_.TakeEvents(),
              ElementsAre("a.test audible", "changed a.test"));

  // Silent again, a moment after the sound stops.
  audible->SetNotRecentlyAudibleForTesting();
  Settle();
  EXPECT_THAT(page_.TakeEvents(), ElementsAre("a.test silent"));

  // Only tabs.
  handler_remote_->SetNodeMuted(TodayRootId(), true);
  handler_remote_->SetNodeMuted("not-a-uuid", true);
  Settle();
  EXPECT_THAT(page_.TakeEvents(), IsEmpty());
}

TEST_F(SidebarPageHandlerTest, FolderMenu) {
  handler_remote_->AddFolder(PinnedId(), 0, "Work");
  Settle();
  SidebarNode* folder = space_model().pinned_root()->children()[0].get();
  const std::string id = folder->id().AsLowercaseString();
  EXPECT_THAT(MenuCommands(id),
              ElementsAre(Command::kNewFolder, Command::kRename,
                          Command::kUnpin, Command::kRemove));
  page_.TakeEvents();

  Choose(id, Command::kRename);
  EXPECT_THAT(page_.TakeEvents(), ElementsAre("rename " + id));

  Choose(id, Command::kNewFolder);
  ASSERT_EQ(folder->children().size(), 1u);
  EXPECT_EQ(folder->children()[0]->type(), SidebarNode::Type::kFolder);

  Choose(id, Command::kRemove);
  EXPECT_THAT(space_model().pinned_root()->children(), IsEmpty());
}

TEST_F(SidebarPageHandlerTest, RenamesTabs) {
  const std::string a = TodayId(0);
  SidebarNode* node = space_model().today_root()->children()[0].get();
  page_.TakeEvents();

  Choose(a, Command::kRename);
  EXPECT_THAT(page_.TakeEvents(), ElementsAre("rename " + a));

  handler_remote_->RenameTab(a, "  Inbox ");
  Settle();
  EXPECT_EQ(node->custom_title(), u"Inbox");
  EXPECT_EQ(GetTree()->today->children[0]->custom_title, "Inbox");
  // The page's title goes on changing underneath.
  space_model().SetTitle(node, u"Mail (3)");
  EXPECT_EQ(node->display_title(), u"Inbox");

  // A blank name goes back to the page's title.
  handler_remote_->RenameTab(a, " ");
  Settle();
  EXPECT_TRUE(node->custom_title().empty());
  EXPECT_EQ(node->display_title(), u"Mail (3)");

  // Only tabs.
  handler_remote_->AddFolder(PinnedId(), 0, "Work");
  Settle();
  SidebarNode* folder = space_model().pinned_root()->children()[0].get();
  handler_remote_->RenameTab(folder->id().AsLowercaseString(), "Job");
  Settle();
  EXPECT_TRUE(folder->custom_title().empty());
  EXPECT_EQ(folder->title(), u"Work");
}

TEST_F(SidebarPageHandlerTest, SplitsAndSeparatesFromTheMenu) {
  // a is active; b opens next to it.
  const std::string b = TodayId(1);
  EXPECT_THAT(MenuCommands(TodayId(0)), Not(Contains(Command::kOpenInSplit)));
  Choose(b, Command::kOpenInSplit);
  ASSERT_EQ(space_model().today_root()->children().size(), 1u);
  SidebarNode* split = space_model().today_root()->children()[0].get();
  ASSERT_EQ(split->type(), SidebarNode::Type::kSplit);
  tabs::TabInterface* a = tab_strip_model()->GetTabAtIndex(0);
  ASSERT_TRUE(a->GetSplit());
  EXPECT_EQ(a->GetSplit(), tab_strip_model()->GetTabAtIndex(1)->GetSplit());

  // A pane's menu, or the split's, separates them.
  EXPECT_THAT(MenuCommands(b), Contains(Command::kSeparate));
  Choose(split->id().AsLowercaseString(), Command::kSeparate);
  EXPECT_EQ(space_model().today_root()->children().size(), 2u);
  EXPECT_FALSE(a->GetSplit());
  EXPECT_EQ(tab_strip_model()->count(), 2);
}

TEST_F(SidebarPageHandlerTest, DroppingATabOnAnotherSplitsThem) {
  const std::string a = TodayId(0);
  const std::string b = TodayId(1);

  // b dropped on a's left half: b, then a.
  handler_remote_->SplitNode(b, a, /*first=*/true);
  Settle();

  ASSERT_EQ(space_model().today_root()->children().size(), 1u);
  const SidebarNode* split = space_model().today_root()->children()[0].get();
  ASSERT_EQ(split->type(), SidebarNode::Type::kSplit);
  EXPECT_EQ(split->children()[0]->id().AsLowercaseString(), b);
  EXPECT_EQ(split->children()[1]->id().AsLowercaseString(), a);
  EXPECT_TRUE(tab_strip_model()->GetTabAtIndex(0)->GetSplit());

  // Not onto itself, nor a third pane.
  handler_remote_->SplitNode(a, a, /*first=*/false);
  Settle();
  EXPECT_EQ(split->children().size(), 2u);
}

TEST_F(SidebarPageHandlerTest, ChoosingForAGoneNodeDoesNothing) {
  const std::string a = TodayId(0);
  std::unique_ptr<ui::SimpleMenuModel> menu = handler_->CreateContextMenu(a);
  handler_remote_->CloseNode(a);
  Settle();
  ASSERT_EQ(tab_strip_model()->count(), 1);

  menu->ActivatedAt(*menu->GetIndexOfCommandId(Command::kDuplicate));
  Settle();
  EXPECT_EQ(tab_strip_model()->count(), 1);

  // Nor once the handler is gone.
  menu = handler_->CreateContextMenu(TodayId(0));
  handler_.reset();
  menu->ActivatedAt(*menu->GetIndexOfCommandId(Command::kCloseTab));
  Settle();
  EXPECT_EQ(tab_strip_model()->count(), 1);
}

// Slice 3 of docs/design/spaces.md.
TEST_F(SidebarPageHandlerTest, ListsAndSwapsToAnotherProfilesSpace) {
  TestingProfile* other = profile_manager()->CreateTestingProfile("other");
  // Listed as if it had a window.
  SpaceDirectory::Get()->List(other);
  SpaceList& other_list = SpaceService::Get(other)->space_list();
  SpaceModel* work = other_list.AddSpace(1, u"Work");
  work->AddTab(work->pinned_root(), 0, GURL("https://w.test/"), u"W");
  Settle();

  // One list, in the order the spaces came.
  EXPECT_EQ(page_.spaces(), "[Home],Home*,Work*");
  // Every profile can take a space.
  EXPECT_EQ(page_.profiles().size(), 2u);

  // Its tree, for a swipe, with its own entries.
  base::test::TestFuture<sidebar::mojom::TreePtr> tree;
  handler_remote_->GetSpaceTree(work->id().AsLowercaseString(),
                                tree.GetCallback());
  ASSERT_TRUE(tree.Get());
  EXPECT_THAT(Hosts(*tree.Get()->pinned), ElementsAre("w.test"));

  // Entries can't go there.
  handler_remote_->MoveNodeToSpace(TodayId(0), work->id().AsLowercaseString());
  Settle();
  EXPECT_THAT(work->today_root()->children(), IsEmpty());
  EXPECT_EQ(space_model().today_root()->children().size(), 2u);

  std::unique_ptr<BrowserWindowInterface> swapped;
  SetSwapWindowFactoryForTesting(base::BindLambdaForTesting(
      [&](Profile* profile, const gfx::Rect& bounds,
            ui::mojom::WindowShowState show_state) {
        swapped = CreateBrowser(profile, BrowserWindowInterface::TYPE_NORMAL,
                                /*hosted_app=*/false);
        return swapped.get();
      }));
  handler_remote_->SwitchSpace(work->id().AsLowercaseString());
  Settle();
  SetSwapWindowFactoryForTesting({});

  ASSERT_TRUE(swapped);
  EXPECT_EQ(&WindowFeatures::From(swapped.get())->space_model(), work);
  EXPECT_EQ(swapped->GetTabStripModel()->count(), 1);
  EXPECT_TRUE(tab_strip_model()->empty());
  handler_.reset();
  swapped->GetTabStripModel()->CloseAllTabs();
}

TEST_F(SidebarPageHandlerTest, ReordersSpacesAcrossProfiles) {
  TestingProfile* other = profile_manager()->CreateTestingProfile("other");
  SpaceDirectory::Get()->List(other);
  SpaceModel* work =
      SpaceService::Get(other)->space_list().AddSpace(1, u"Work");
  Settle();
  ASSERT_EQ(page_.spaces(), "[Home],Home*,Work*");

  // Another profile's space to the front, the window's own to the end.
  handler_remote_->MoveSpace(work->id().AsLowercaseString(), 0);
  handler_remote_->MoveSpace(space_model().id().AsLowercaseString(), 3);
  // Unknown spaces are ignored.
  handler_remote_->MoveSpace("nope", 0);
  Settle();

  EXPECT_EQ(page_.spaces(), "Work*,Home*,[Home]");
}

TEST_F(SidebarPageHandlerTest, AssignsASpaceToAnotherProfile) {
  TestingProfile* other = profile_manager()->CreateTestingProfile("other");
  SpaceModel* work = WindowFeatures::From(browser())
                         ->service()
                         .space_list()
                         .AddSpace(1, u"Work");
  work->AddTab(work->pinned_root(), 0, GURL("https://w.test/"), u"W");
  Settle();
  ASSERT_EQ(page_.spaces(), "[Home],Work");

  handler_remote_->SetSpaceProfile(work->id().AsLowercaseString(),
                                   other->GetBaseName().AsUTF8Unsafe());
  Settle();

  // In its place, now the other profile's; that profile's own empty Home
  // stays out.
  EXPECT_EQ(page_.spaces(), "[Home],Work*");
  SpaceList& other_list = SpaceService::Get(other)->space_list();
  ASSERT_EQ(other_list.spaces().size(), 1u);
  EXPECT_THAT(other_list.spaces()[0]->pinned_root()->children(),
              testing::SizeIs(1));
}

}  // namespace
}  // namespace compass
