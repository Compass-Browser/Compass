// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/native_messaging/chrome_hosts_mac.h"

#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

constexpr char kOnePassword[] = "com.1password.1password.json";

class NativeMessagingChromeHostsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    chrome_ = temp_.GetPath().Append("Chrome");
    compass_ = temp_.GetPath().Append("Compass");
    ASSERT_TRUE(base::CreateDirectory(chrome_));
  }

  std::string Read(const base::FilePath& path) {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(path, &contents));
    return contents;
  }

  base::ScopedTempDir temp_;
  base::FilePath chrome_;
  base::FilePath compass_;
};

TEST_F(NativeMessagingChromeHostsTest, CopiesOnePasswordIntoNewFolder) {
  ASSERT_TRUE(base::WriteFile(chrome_.Append(kOnePassword), "{\"v\":1}"));
  CopyNativeMessagingHostsFromChrome(chrome_, compass_);
  EXPECT_EQ("{\"v\":1}", Read(compass_.Append(kOnePassword)));
}

TEST_F(NativeMessagingChromeHostsTest, UpdatesAChangedManifest) {
  ASSERT_TRUE(base::CreateDirectory(compass_));
  ASSERT_TRUE(base::WriteFile(compass_.Append(kOnePassword), "{\"v\":1}"));
  ASSERT_TRUE(base::WriteFile(chrome_.Append(kOnePassword), "{\"v\":2}"));
  CopyNativeMessagingHostsFromChrome(chrome_, compass_);
  EXPECT_EQ("{\"v\":2}", Read(compass_.Append(kOnePassword)));
}

TEST_F(NativeMessagingChromeHostsTest, KeepsCompassCopyWhenChromeHasNone) {
  ASSERT_TRUE(base::CreateDirectory(compass_));
  ASSERT_TRUE(base::WriteFile(compass_.Append(kOnePassword), "{\"v\":1}"));
  CopyNativeMessagingHostsFromChrome(chrome_, compass_);
  EXPECT_EQ("{\"v\":1}", Read(compass_.Append(kOnePassword)));
}

TEST_F(NativeMessagingChromeHostsTest, CopiesOnlyListedHosts) {
  ASSERT_TRUE(base::WriteFile(chrome_.Append("com.example.other.json"), "{}"));
  CopyNativeMessagingHostsFromChrome(chrome_, compass_);
  EXPECT_FALSE(base::PathExists(compass_.Append("com.example.other.json")));
}

}  // namespace
}  // namespace compass
