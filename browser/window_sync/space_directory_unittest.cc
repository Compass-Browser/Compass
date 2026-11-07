// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/window_sync/space_directory.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/testing_profile_manager.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

class RenamePlaceholderProfilesTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(profile_manager_.SetUp()); }

  ProfileAttributesEntry* AddProfile(const std::string& dir,
                                     const std::u16string& name,
                                     bool is_default_name) {
    Profile* profile = profile_manager_.CreateTestingProfile(dir);
    ProfileAttributesEntry* entry =
        storage().GetProfileAttributesWithPath(profile->GetPath());
    entry->SetLocalProfileName(name, is_default_name);
    return entry;
  }

  ProfileAttributesStorage& storage() {
    return *profile_manager_.profile_attributes_storage();
  }

  content::BrowserTaskEnvironment task_environment_;
  TestingProfileManager profile_manager_{TestingBrowserProcess::GetGlobal()};
};

TEST_F(RenamePlaceholderProfilesTest, DefaultPlaceholderNamesBecomeDefault) {
  ProfileAttributesEntry* chromium =
      AddProfile("a", u"Your Chromium", /*is_default_name=*/true);
  ProfileAttributesEntry* compass =
      AddProfile("b", u"Your Compass", /*is_default_name=*/true);
  // Taken from the chain (not a default name), still the placeholder: a
  // Windows device's "Your Chromium - Compass" desktop shortcut.
  ProfileAttributesEntry* synced =
      AddProfile("c", u"Your Chromium", /*is_default_name=*/false);
  ProfileAttributesEntry* work = AddProfile("d", u"Work", false);

  RenamePlaceholderProfiles(storage());

  EXPECT_EQ(chromium->GetLocalProfileName(), u"Default");
  EXPECT_TRUE(chromium->IsUsingDefaultName());
  // "Default" is taken: the next default name.
  EXPECT_NE(compass->GetLocalProfileName(), u"Your Compass");
  EXPECT_NE(compass->GetLocalProfileName(), u"Default");
  EXPECT_TRUE(compass->IsUsingDefaultName());
  EXPECT_FALSE(IsPlaceholderProfileName(synced->GetLocalProfileName()));
  EXPECT_TRUE(synced->IsUsingDefaultName());
  EXPECT_EQ(work->GetLocalProfileName(), u"Work");
}

}  // namespace
}  // namespace compass
