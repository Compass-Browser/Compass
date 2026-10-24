// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/sidebar/sidebar_page_handler.h"

#include <memory>
#include <string>
#include <vector>

#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace strata {
namespace {

using ::testing::ElementsAre;

// Records what the browser pushes to the page.
class FakePage : public sidebar::mojom::Page {
 public:
  mojo::PendingRemote<sidebar::mojom::Page> BindAndPassRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  // sidebar::mojom::Page:
  void TabsChanged(std::vector<sidebar::mojom::TabPtr> tabs) override {
    last_tabs_ = std::move(tabs);
    ++tabs_changed_count_;
  }

  const std::vector<sidebar::mojom::TabPtr>& last_tabs() const {
    return last_tabs_;
  }
  int tabs_changed_count() const { return tabs_changed_count_; }

 private:
  mojo::Receiver<sidebar::mojom::Page> receiver_{this};
  std::vector<sidebar::mojom::TabPtr> last_tabs_;
  int tabs_changed_count_ = 0;
};

// URLs of |tabs|, with the active one marked "*".
std::vector<std::string> Describe(
    const std::vector<sidebar::mojom::TabPtr>& tabs) {
  std::vector<std::string> result;
  for (const auto& tab : tabs) {
    result.push_back((tab->active ? "*" : "") + tab->url.spec());
  }
  return result;
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
    handler_ = std::make_unique<SidebarPageHandler>(
        handler_remote_.BindNewPipeAndPassReceiver(),
        page_.BindAndPassRemote(), browser());
  }

  void TearDown() override {
    handler_.reset();
    BrowserWithTestWindowTest::TearDown();
  }

  std::vector<std::string> GetTabs() {
    base::test::TestFuture<std::vector<sidebar::mojom::TabPtr>> future;
    handler_remote_->GetTabs(future.GetCallback());
    return Describe(future.Get());
  }

  TabStripModel* tab_strip_model() { return browser()->GetTabStripModel(); }

  // Delivers pending Mojo messages in both directions.
  void Flush() { base::RunLoop().RunUntilIdle(); }

  FakePage page_;
  mojo::Remote<sidebar::mojom::PageHandler> handler_remote_;
  std::unique_ptr<SidebarPageHandler> handler_;
};

TEST_F(SidebarPageHandlerTest, ListsTabsInStripOrder) {
  EXPECT_THAT(GetTabs(), ElementsAre("*https://a.test/", "https://b.test/"));
}

TEST_F(SidebarPageHandlerTest, ActivatesTab) {
  base::test::TestFuture<std::vector<sidebar::mojom::TabPtr>> future;
  handler_remote_->GetTabs(future.GetCallback());
  const int32_t second_tab_id = future.Get()[1]->id;

  handler_remote_->ActivateTab(second_tab_id);
  Flush();

  EXPECT_EQ(tab_strip_model()->active_index(), 1);
  EXPECT_THAT(Describe(page_.last_tabs()),
              ElementsAre("https://a.test/", "*https://b.test/"));
}

TEST_F(SidebarPageHandlerTest, ClosesTab) {
  base::test::TestFuture<std::vector<sidebar::mojom::TabPtr>> future;
  handler_remote_->GetTabs(future.GetCallback());
  const int32_t first_tab_id = future.Get()[0]->id;

  handler_remote_->CloseTab(first_tab_id);
  Flush();

  EXPECT_EQ(tab_strip_model()->count(), 1);
  EXPECT_THAT(GetTabs(), ElementsAre("*https://b.test/"));
}

TEST_F(SidebarPageHandlerTest, IgnoresUnknownTabIds) {
  handler_remote_->ActivateTab(-1);
  handler_remote_->CloseTab(-1);
  Flush();

  EXPECT_EQ(tab_strip_model()->count(), 2);
  EXPECT_EQ(tab_strip_model()->active_index(), 0);
}

TEST_F(SidebarPageHandlerTest, PushesChangesToThePage) {
  const int before = page_.tabs_changed_count();

  AddTab(browser(), GURL("https://c.test/"));
  // The tab is inserted before its navigation commits; the committed URL
  // arrives with Browser's coalesced UI update (200 ms).
  task_environment()->FastForwardBy(base::Seconds(1));

  EXPECT_GT(page_.tabs_changed_count(), before);
  EXPECT_THAT(Describe(page_.last_tabs()),
              ElementsAre("*https://c.test/", "https://a.test/",
                          "https://b.test/"));
}

}  // namespace
}  // namespace strata
