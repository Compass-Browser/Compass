// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sidebar/update_notice.h"

#include <optional>

#include "base/functional/bind.h"
#include "base/version.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/upgrade_detector/build_state.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "compass/browser/updates/update_hooks.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

// The sidebar's "update ready" card: docs/design/sidebar.md, "Update ready".

namespace compass {
namespace {

class UpdateNoticeBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpdate(const char* version) {
    g_browser_process->GetBuildState()->SetUpdate(
        BuildState::UpdateType::kNormalUpdate, base::Version(version),
        std::nullopt);
  }

  PrefService& local_state() { return *g_browser_process->local_state(); }
};

IN_PROC_BROWSER_TEST_F(UpdateNoticeBrowserTest, ShowsOnceAnUpdateIsReady) {
  int changes = 0;
  UpdateNotice notice(base::BindRepeating([](int* n) { ++*n; }, &changes));
  EXPECT_FALSE(notice.ShouldShow());

  SetUpdate("999.0.0.1");
  EXPECT_TRUE(notice.ShouldShow());
  EXPECT_GE(changes, 1);
}

IN_PROC_BROWSER_TEST_F(UpdateNoticeBrowserTest, FollowsTheAboutPageSwitch) {
  int changes = 0;
  UpdateNotice notice(base::BindRepeating([](int* n) { ++*n; }, &changes));
  SetUpdate("999.0.0.1");
  ASSERT_TRUE(notice.ShouldShow());

  changes = 0;
  local_state().SetBoolean(updates::kUpdateSidebarNoticePref, false);
  EXPECT_FALSE(notice.ShouldShow());
  EXPECT_EQ(1, changes);

  local_state().SetBoolean(updates::kUpdateSidebarNoticePref, true);
  EXPECT_TRUE(notice.ShouldShow());
}

IN_PROC_BROWSER_TEST_F(UpdateNoticeBrowserTest,
                       DismissingHidesEverywhereUntilANewerUpdate) {
  int changes = 0;
  UpdateNotice notice(base::BindRepeating([](int* n) { ++*n; }, &changes));
  // Another window's.
  int other_changes = 0;
  UpdateNotice other(
      base::BindRepeating([](int* n) { ++*n; }, &other_changes));
  SetUpdate("999.0.0.1");
  ASSERT_TRUE(notice.ShouldShow());

  other_changes = 0;
  notice.Dismiss();
  EXPECT_FALSE(notice.ShouldShow());
  EXPECT_FALSE(other.ShouldShow());
  EXPECT_EQ(1, other_changes);

  SetUpdate("999.0.0.2");
  EXPECT_TRUE(notice.ShouldShow());
  EXPECT_TRUE(other.ShouldShow());
}

}  // namespace
}  // namespace compass
