// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <optional>
#include <string>

#include "base/test/scoped_command_line.h"
#include "base/values.h"
#include "compass/components/sync/engine_components_factory.h"
#include "compass/components/sync/pairing_network_transport.h"
#include "compass/components/sync/pref_names.h"
#include "compass/components/sync/sync_credentials.h"
#include "compass/components/sync/sync_hooks.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "components/sync/base/command_line_switches.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

class CompassSyncEngineTest : public testing::Test {
 protected:
  CompassSyncEngineTest() {
    prefs_.registry()->RegisterStringPref(prefs::kServerUrl, std::string());
    prefs_.registry()->RegisterStringPref(prefs::kSeed, std::string());
  }
  ~CompassSyncEngineTest() override { SyncCredentials::Forget(&prefs_); }

  TestingPrefServiceSimple prefs_;
};

TEST_F(CompassSyncEngineTest, ConnectionUrl) {
  const GURL url = CompassEngineComponentsFactory::MakeConnectionURL(
      GURL("http://192.168.1.20:8295/v2"), "guid");
  EXPECT_EQ(url.GetPath(), "/v2/command/");
  EXPECT_EQ(url.host(), "192.168.1.20");
  EXPECT_NE(url.GetQuery().find("client_id=guid"), std::string::npos);
  EXPECT_EQ(CompassEngineComponentsFactory::MakeConnectionURL(
                GURL("https://sync.example.com/v2/"), "guid")
                .GetPath(),
            "/v2/command/");
}

TEST_F(CompassSyncEngineTest, PairUrl) {
  EXPECT_EQ(NetworkPairingTransport::MakePairUrl(
                GURL("http://192.168.1.20:8295/v2"), "")
                .spec(),
            "http://192.168.1.20:8295/v2/compass/pair");
  EXPECT_EQ(NetworkPairingTransport::MakePairUrl(
                GURL("https://sync.example.com/v2/"), "7/msg")
                .spec(),
            "https://sync.example.com/v2/compass/pair/7/msg");
}

// A 410's body says why the slot closed (docs/design/sync.md, "Slice 4
// notes"); anything else is kUnknown.
TEST_F(CompassSyncEngineTest, PairClosedReason) {
  auto reply = [](const char* reason) {
    base::DictValue dict;
    dict.Set("reason", reason);
    return std::optional<base::DictValue>(std::move(dict));
  };
  EXPECT_EQ(NetworkPairingTransport::ClosedReasonOf(410, reply("mismatch")),
            PairingClosedReason::kMismatch);
  EXPECT_EQ(NetworkPairingTransport::ClosedReasonOf(410, reply("cancel")),
            PairingClosedReason::kCancel);
  EXPECT_EQ(NetworkPairingTransport::ClosedReasonOf(410, reply("deny")),
            PairingClosedReason::kDeny);
  EXPECT_EQ(NetworkPairingTransport::ClosedReasonOf(410, reply("expired")),
            PairingClosedReason::kExpired);
  EXPECT_EQ(NetworkPairingTransport::ClosedReasonOf(410, reply("bored")),
            PairingClosedReason::kUnknown);
  EXPECT_EQ(NetworkPairingTransport::ClosedReasonOf(404, reply("cancel")),
            PairingClosedReason::kUnknown);
  EXPECT_EQ(NetworkPairingTransport::ClosedReasonOf(410, std::nullopt),
            PairingClosedReason::kUnknown);
  EXPECT_EQ(PairingClosedReasonName(PairingClosedReason::kDeny), "deny");
  EXPECT_EQ(PairingClosedReasonName(PairingClosedReason::kUnknown), "");
}

TEST_F(CompassSyncEngineTest, ServerUrlFromPref) {
  const GURL google("https://clients4.google.com/chrome-sync");
  EXPECT_FALSE(GetServerUrl(&prefs_, google).is_valid());
  EXPECT_FALSE(IsChainEnabled(&prefs_, google));

  prefs_.SetString(prefs::kServerUrl, "http://192.168.1.20:8295/v2");
  EXPECT_EQ(GetServerUrl(&prefs_, google),
            GURL("http://192.168.1.20:8295/v2"));
  // No seed yet.
  EXPECT_FALSE(IsChainEnabled(&prefs_, google));

  prefs_.SetString(prefs::kSeed, "c2VlZA==");
  EXPECT_TRUE(IsChainEnabled(&prefs_, google));

  prefs_.SetString(prefs::kServerUrl, "not a url");
  EXPECT_FALSE(IsChainEnabled(&prefs_, google));
  prefs_.SetString(prefs::kServerUrl, "file:///tmp/sync");
  EXPECT_FALSE(IsChainEnabled(&prefs_, google));
}

TEST_F(CompassSyncEngineTest, SyncUrlSwitchWins) {
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitchASCII(
      syncer::kSyncServiceURL, "http://localhost:8295/v2");
  prefs_.SetString(prefs::kServerUrl, "http://192.168.1.20:8295/v2");
  // SyncServiceImpl passes --sync-url as the upstream URL.
  EXPECT_EQ(GetServerUrl(&prefs_, GURL("http://localhost:8295/v2")),
            GURL("http://localhost:8295/v2"));
}

TEST_F(CompassSyncEngineTest, UnregisteredPrefsMeanNoChain) {
  TestingPrefServiceSimple bare;
  EXPECT_FALSE(IsChainEnabled(&bare, GURL("https://example.com/")));
}

TEST_F(CompassSyncEngineTest, Credentials) {
  scoped_refptr<SyncCredentials> credentials =
      SyncCredentials::ForPrefs(&prefs_);
  EXPECT_EQ(credentials, SyncCredentials::ForPrefs(&prefs_));
  EXPECT_FALSE(credentials->HasKey());
  EXPECT_FALSE(credentials->MakeToken(base::Time::Now()));

  Seed seed{};
  const AuthKey key = DeriveAuthKey(seed);
  credentials->SetKey(key);
  const base::Time now = base::Time::FromMillisecondsSinceUnixEpoch(1000);
  EXPECT_EQ(credentials->MakeToken(now), MakeAccessToken(key, now));

  credentials->Clear();
  EXPECT_FALSE(credentials->MakeToken(now));

  SyncCredentials::Forget(&prefs_);
  EXPECT_NE(credentials, SyncCredentials::ForPrefs(&prefs_));
}

}  // namespace
}  // namespace compass::sync
