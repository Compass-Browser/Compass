// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/dev_mode/dev_mode.h"

#include "base/values.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::dev_mode {
namespace {

class DevModeTest : public testing::Test {
 protected:
  // Starts from an empty list (as after taking the defaults off), so each
  // test sees only the sites it adds.
  DevModeTest() {
    RegisterCompassProfilePrefs(prefs_.registry());
    prefs_.SetList(prefs::kDevModeHosts, base::ListValue());
  }

  TestingPrefServiceSimple prefs_;
};

TEST(DevModeNormalizeTest, Normalizes) {
  EXPECT_EQ(NormalizeHost("localhost"), "localhost");
  EXPECT_EQ(NormalizeHost("  LocalHost:3000 "), "localhost");
  EXPECT_EQ(NormalizeHost("http://localhost:3000/app?x=1"), "localhost");
  EXPECT_EQ(NormalizeHost("https://www.Example.com/a"), "example.com");
  EXPECT_EQ(NormalizeHost("*.example.com"), "*.example.com");
  EXPECT_EQ(NormalizeHost("127.0.0.1:8080"), "127.0.0.1");
  EXPECT_EQ(NormalizeHost("bücher.example"), "xn--bcher-kva.example");
}

TEST(DevModeNormalizeTest, Refuses) {
  for (const char* input :
       {"", "  ", "*.", "*", "*.com", "ftp://example.com", "chrome://settings",
        "file:///etc/hosts", "*.127.0.0.1"}) {
    EXPECT_FALSE(NormalizeHost(input).has_value()) << input;
  }
}

// Local development servers are in development mode until taken off.
TEST(DevModeDefaultsTest, LocalServersByDefault) {
  TestingPrefServiceSimple prefs;
  RegisterCompassProfilePrefs(prefs.registry());
  EXPECT_EQ(Hosts(&prefs), (std::vector<std::string>{"localhost"}));
  EXPECT_TRUE(IsEnabledFor(&prefs, GURL("http://localhost:3000/")));
  EXPECT_TRUE(IsEnabledFor(&prefs, GURL("https://localhost:8443/app")));
  EXPECT_FALSE(IsEnabledFor(&prefs, GURL("http://127.0.0.1:8080/")));
  EXPECT_FALSE(IsEnabledFor(&prefs, GURL("https://example.com/")));

  // Turning it off on a local page takes that default off, and it stays off.
  SetEnabledFor(&prefs, GURL("http://localhost:5173/"), false);
  EXPECT_FALSE(IsEnabledFor(&prefs, GURL("http://localhost:3000/")));
  EXPECT_TRUE(Hosts(&prefs).empty());
}

TEST_F(DevModeTest, OffWithNoSites) {
  EXPECT_FALSE(IsEnabledFor(&prefs_, GURL("http://localhost:3000/")));
  EXPECT_TRUE(Hosts(&prefs_).empty());
}

TEST_F(DevModeTest, AddAndRemoveHosts) {
  EXPECT_EQ(AddHost(&prefs_, "localhost:3000"), "localhost");
  EXPECT_EQ(AddHost(&prefs_, "http://localhost"), "localhost");
  EXPECT_EQ(AddHost(&prefs_, "*.example.com"), "*.example.com");
  EXPECT_FALSE(AddHost(&prefs_, "chrome://settings").has_value());
  EXPECT_EQ(Hosts(&prefs_),
            (std::vector<std::string>{"localhost", "*.example.com"}));

  EXPECT_TRUE(IsEnabledFor(&prefs_, GURL("http://localhost:5173/")));
  EXPECT_TRUE(IsEnabledFor(&prefs_, GURL("https://app.example.com/x")));
  EXPECT_FALSE(IsEnabledFor(&prefs_, GURL("https://example.org/")));
  EXPECT_FALSE(IsEnabledFor(&prefs_, GURL("chrome://settings/")));

  RemoveHost(&prefs_, "localhost");
  EXPECT_FALSE(IsEnabledFor(&prefs_, GURL("http://localhost:5173/")));
  EXPECT_EQ(Hosts(&prefs_), (std::vector<std::string>{"*.example.com"}));
}

TEST_F(DevModeTest, SetEnabledForPage) {
  const GURL page("https://www.example.com:8443/docs");
  SetEnabledFor(&prefs_, page, true);
  EXPECT_EQ(Hosts(&prefs_), (std::vector<std::string>{"example.com"}));
  EXPECT_TRUE(IsEnabledFor(&prefs_, page));
  // On already: nothing more is added.
  SetEnabledFor(&prefs_, page, true);
  EXPECT_EQ(Hosts(&prefs_).size(), 1u);

  SetEnabledFor(&prefs_, page, false);
  EXPECT_FALSE(IsEnabledFor(&prefs_, page));
  EXPECT_TRUE(Hosts(&prefs_).empty());

  // Not for the browser's own pages.
  SetEnabledFor(&prefs_, GURL("chrome://settings/"), true);
  EXPECT_TRUE(Hosts(&prefs_).empty());
}

TEST_F(DevModeTest, TurningOffRemovesEveryMatchingPattern) {
  AddHost(&prefs_, "*.example.com");
  AddHost(&prefs_, "app.example.com");
  AddHost(&prefs_, "localhost");
  SetEnabledFor(&prefs_, GURL("https://app.example.com/"), false);
  EXPECT_FALSE(IsEnabledFor(&prefs_, GURL("https://app.example.com/")));
  EXPECT_EQ(Hosts(&prefs_), (std::vector<std::string>{"localhost"}));
}

}  // namespace
}  // namespace compass::dev_mode
