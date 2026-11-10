// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_folder_store.h"

#include <memory>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/uuid.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/os_crypt/async/common/test_encryptor.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

class LiveFolderStoreTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }

  base::FilePath Path() const {
    return dir_.GetPath().AppendASCII("Compass Live Folders.json");
  }

  std::unique_ptr<LiveFolderStore> Load() {
    auto store = std::make_unique<LiveFolderStore>(Path());
    base::test::TestFuture<void> loaded;
    store->Load(os_crypt_async::GetTestEncryptorForTesting(),
                loaded.GetCallback());
    EXPECT_TRUE(loaded.Wait());
    EXPECT_TRUE(store->loaded());
    return store;
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
};

TEST_F(LiveFolderStoreTest, KeepsSecretsEncryptedAndGrants) {
  const base::Uuid folder = base::Uuid::GenerateRandomV4();
  {
    std::unique_ptr<LiveFolderStore> store = Load();
    store->SetSecret("compass.github", "token", "ghp_secret");
    store->SetSecret("compass.calendar", "ics", "https://cal.test/private");
    store->SetCookiesGranted(folder, true);
    // Destroying writes.
  }
  task_environment_.RunUntilIdle();

  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(Path(), &contents));
  EXPECT_EQ(contents.find("ghp_secret"), std::string::npos);
  EXPECT_EQ(contents.find("cal.test"), std::string::npos);

  std::unique_ptr<LiveFolderStore> store = Load();
  EXPECT_EQ(store->GetSecrets("compass.github").at("token"), "ghp_secret");
  EXPECT_TRUE(store->HasSecret("compass.calendar", "ics"));
  EXPECT_FALSE(store->HasSecret("compass.rss", "token"));
  EXPECT_TRUE(store->CookiesGranted(folder));
  EXPECT_FALSE(store->CookiesGranted(base::Uuid::GenerateRandomV4()));

  // Empty removes.
  store->SetSecret("compass.github", "token", "");
  store->SetCookiesGranted(folder, false);
  EXPECT_FALSE(store->HasSecret("compass.github", "token"));
  EXPECT_FALSE(store->CookiesGranted(folder));
}

TEST_F(LiveFolderStoreTest, UnreadableFilesStartEmpty) {
  ASSERT_TRUE(base::WriteFile(Path(), "{not json"));
  std::unique_ptr<LiveFolderStore> store = Load();
  EXPECT_TRUE(store->GetSecrets("compass.github").empty());
}

}  // namespace
}  // namespace compass
