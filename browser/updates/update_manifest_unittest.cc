// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/updates/update_manifest.h"

#include <string>

#include "base/containers/span.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::updates {
namespace {

// The release key's public half (config/args/release.gn) and a file that
// Sparkle 2.10's sign_update signed with the private half: the Windows
// updater checks the same signatures as Sparkle.
constexpr char kPublicKey[] = "FS3QT5Iu8sEmUjdndH5xmg4neztioz9CxvRmARgzbb0=";
constexpr char kSigned[] = "Compass update test\n";
constexpr char kSignature[] =
    "MoU/zkNiryvBGuj8d+NBEPybC/KmStwnNrQcgOkAxH596nP8ootQaAae6Yrhq05JhOxY4bGGA"
    "pCoOOlNptrbAA==";
constexpr char kSha256[] =
    "3744bb5784dc8ce83045a2a7c6144270bb6616e15c12423c24e5b88e7216dd72";

constexpr char kManifest[] = R"({
  "schema": 1,
  "channel": "beta",
  "win": {
    "version": "20261103.0215",
    "chromium": "156.0.8078.48",
    "commit": "abc1234",
    "date": "2026-11-03T01:15:00Z",
    "url": "https://updates.compassbrowser.org/win/CompassSetup-20261103.0215.exe",
    "size": 20,
    "sha256": "3744BB5784DC8CE83045A2A7C6144270BB6616E15C12423C24E5B88E7216DD72",
    "ed_signature": "sig"
  }
})";

ManifestEntry SignedEntry() {
  ManifestEntry entry;
  entry.version = "20261103.0215";
  entry.size = 20;
  entry.sha256 = kSha256;
  entry.ed_signature = kSignature;
  return entry;
}

base::span<const uint8_t> Bytes(std::string_view text) {
  return base::as_byte_span(text);
}

TEST(UpdateManifestTest, ChannelUrls) {
  EXPECT_EQ("https://updates.compassbrowser.org/beta.json",
            ChannelManifestUrl("beta").spec());
}

TEST(UpdateManifestTest, ParsesThePlatformsEntry) {
  std::optional<ManifestEntry> entry = ParseChannelManifest(kManifest, "win");
  ASSERT_TRUE(entry);
  EXPECT_EQ("20261103.0215", entry->version);
  EXPECT_EQ("156.0.8078.48", entry->chromium);
  EXPECT_EQ(20, entry->size);
  EXPECT_EQ(kSha256, entry->sha256);  // Lowercased.
  EXPECT_FALSE(ParseChannelManifest(kManifest, "mac"));
  EXPECT_FALSE(ParseChannelManifest("not json", "win"));
}

TEST(UpdateManifestTest, InstallersComeOnlyFromTheUpdateServer) {
  std::string json(kManifest);
  std::string other = json;
  other.replace(other.find("https://updates.compassbrowser.org"),
                strlen("https://updates.compassbrowser.org"),
                "https://example.com");
  EXPECT_FALSE(ParseChannelManifest(other, "win"));
  std::string http = json;
  http.replace(http.find("https://"), strlen("https://"), "http://");
  EXPECT_FALSE(ParseChannelManifest(http, "win"));
}

TEST(UpdateManifestTest, NewerVersions) {
  EXPECT_TRUE(IsNewerVersion("20261103.0215", "20261102.2359"));
  EXPECT_TRUE(IsNewerVersion("20261103.1000", "20261103.0215"));
  EXPECT_FALSE(IsNewerVersion("20261103.0215", "20261103.0215"));
  EXPECT_FALSE(IsNewerVersion("20261102.0000", "20261103.0215"));
  // A dev build has no Compass version: nothing is newer.
  EXPECT_FALSE(IsNewerVersion("20261103.0215", ""));
  EXPECT_FALSE(IsNewerVersion("bad", "20261103.0215"));
}

TEST(UpdateManifestTest, VerifiesSparkleSignatures) {
  EXPECT_TRUE(VerifyDownload(Bytes(kSigned), SignedEntry(), kPublicKey));
}

TEST(UpdateManifestTest, RejectsTamperedDownloads) {
  // Same size, one byte changed: the hash catches it.
  EXPECT_FALSE(VerifyDownload(Bytes("Compass update tesT\n"), SignedEntry(),
                              kPublicKey));
  // Wrong size.
  EXPECT_FALSE(VerifyDownload(Bytes("Compass"), SignedEntry(), kPublicKey));
  // A hash that matches but a signature that doesn't.
  ManifestEntry forged = SignedEntry();
  forged.ed_signature = std::string(kSignature).replace(0, 4, "AAAA");
  EXPECT_FALSE(VerifyDownload(Bytes(kSigned), forged, kPublicKey));
  // Another key.
  EXPECT_FALSE(VerifyDownload(Bytes(kSigned), SignedEntry(),
                              "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));
}

}  // namespace
}  // namespace compass::updates
