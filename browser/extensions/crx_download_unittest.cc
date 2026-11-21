// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A .crx downloaded from a link installs with Developer mode on, as one
// dropped on chrome://extensions does (patch of download_crx_util.cc;
// docs/design/extensions.md, "Installing a .crx").

#include "chrome/browser/download/download_crx_util.h"

#include "chrome/common/pref_names.h"
#include "chrome/test/base/testing_profile.h"
#include "components/download/public/common/mock_download_item.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace {

using ::testing::ReturnRefOfCopy;

class CompassCrxDownloadTest : public testing::Test {
 protected:
  CompassCrxDownloadTest() {
    ON_CALL(item_, GetURL())
        .WillByDefault(ReturnRefOfCopy(GURL(
            "https://github.com/gorhill/uBlock/releases/download/1.76.7b0/"
            "uBlock0_1.76.7b0.chromium.crx")));
    ON_CALL(item_, GetReferrerUrl())
        .WillByDefault(ReturnRefOfCopy(
            GURL("https://github.com/gorhill/uBlock/releases")));
  }

  content::BrowserTaskEnvironment task_environment_;
  TestingProfile profile_;
  testing::NiceMock<download::MockDownloadItem> item_;
};

TEST_F(CompassCrxDownloadTest, RefusedWithoutDeveloperMode) {
  profile_.GetPrefs()->SetBoolean(prefs::kExtensionsUIDeveloperMode, false);
  EXPECT_FALSE(download_crx_util::OffStoreInstallAllowedByPrefs(&profile_,
                                                                 item_));
}

TEST_F(CompassCrxDownloadTest, AllowedWithDeveloperMode) {
  profile_.GetPrefs()->SetBoolean(prefs::kExtensionsUIDeveloperMode, true);
  EXPECT_TRUE(download_crx_util::OffStoreInstallAllowedByPrefs(&profile_,
                                                                item_));
}

}  // namespace
