// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_plugin_manifest.h"

#include <optional>
#include <string>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

using ::testing::ElementsAre;

base::Value Json(std::string_view json) {
  std::optional<base::Value> value =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  CHECK(value) << json;
  return std::move(*value);
}

// The same cases as plugins/sdk/tests/sdk.test.mjs, "manifest: ...".

TEST(LivePluginManifestTest, ErrorsAreListedTogether) {
  auto manifest = ParseLivePluginManifest(Json(R"({
    "id": "Bad", "name": "", "version": "x", "api": 2,
    "hosts": ["ok.com", "https://no.com", "{config:missing}"],
    "cookies": ["other.com"],
    "config": [{"key": "k", "type": "color", "label": "K"}]})"));
  ASSERT_FALSE(manifest.has_value());
  EXPECT_EQ(manifest.error().size(), 8u);
}

TEST(LivePluginManifestTest, PlaceholdersConfigAndHostMatching) {
  auto manifest = ParseLivePluginManifest(Json(R"({
    "id": "a.b", "name": "N", "version": "1.0", "api": 1,
    "hosts": ["{config:url}", "*.cdn.com"],
    "config": [{"key": "url", "type": "url", "label": "URL"},
               {"key": "n", "type": "number", "label": "N", "default": 5,
                "max": 9},
               {"key": "on", "type": "bool", "label": "On"}],
    "refresh": {"default": 60, "min": 10}})"));
  ASSERT_TRUE(manifest.has_value()) << manifest.error()[0];
  // The floor applies.
  EXPECT_EQ(manifest->refresh_default, base::Seconds(300));
  EXPECT_EQ(manifest->refresh_min, base::Seconds(300));
  EXPECT_EQ(manifest->entry, "main.js");

  auto config = ValidateLiveConfig(
      *manifest,
      Json(R"({"url": "https://Feeds.Example.com/x", "n": "7", "extra": 1})")
          .GetDict());
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(*config, Json(R"({"url": "https://Feeds.Example.com/x", "n": 7,
                              "on": false})")
                         .GetDict());
  auto bad = ValidateLiveConfig(
      *manifest, Json(R"({"url": "ftp://x", "n": 99})").GetDict());
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().size(), 2u);

  LiveAllowedHosts allowed = ResolveLiveHosts(*manifest, *config, {});
  EXPECT_THAT(allowed.hosts, ElementsAre("feeds.example.com", "*.cdn.com"));
  EXPECT_TRUE(
      LiveUrlAllowed(GURL("http://feeds.example.com:8080/a"), allowed.hosts));
  EXPECT_TRUE(LiveUrlAllowed(GURL("https://cdn.com/"), allowed.hosts));
  EXPECT_FALSE(LiveUrlAllowed(GURL("https://evilcdn.com/"), allowed.hosts));
  EXPECT_FALSE(LiveUrlAllowed(GURL("https://feeds.example.com.evil.com/"),
                              allowed.hosts));
  EXPECT_FALSE(LiveUrlAllowed(GURL("file:///etc/passwd"), allowed.hosts));
  EXPECT_FALSE(
      LiveUrlAllowed(GURL("https://u:p@feeds.example.com/"), allowed.hosts));
}

TEST(LivePluginManifestTest, SecretPlaceholdersAndWebcal) {
  auto manifest = ParseLivePluginManifest(Json(R"({
    "id": "a.cal", "name": "Cal", "version": "1", "api": 1,
    "hosts": ["{secret:ics}"], "cookies": ["{secret:ics}"],
    "secrets": [{"key": "ics", "label": "Address"}]})"));
  ASSERT_TRUE(manifest.has_value()) << manifest.error()[0];
  LiveAllowedHosts allowed = ResolveLiveHosts(
      *manifest, base::DictValue(),
      {{"ics", "webcal://Calendar.Example.com/private/basic.ics"}});
  EXPECT_THAT(allowed.hosts, ElementsAre("calendar.example.com"));
  EXPECT_THAT(allowed.cookies, ElementsAre("calendar.example.com"));
  // Without the secret, nothing.
  EXPECT_TRUE(ResolveLiveHosts(*manifest, base::DictValue(), {}).hosts.empty());
}

TEST(LivePluginManifestTest, BuiltinsParse) {
  base::FilePath root;
  ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
  base::FilePath builtin =
      root.AppendASCII("compass").AppendASCII("plugins").AppendASCII("builtin");
  base::FileEnumerator plugins(builtin, false,
                               base::FileEnumerator::DIRECTORIES);
  int count = 0;
  for (base::FilePath dir = plugins.Next(); !dir.empty();
       dir = plugins.Next()) {
    std::string json;
    ASSERT_TRUE(base::ReadFileToString(dir.AppendASCII("manifest.json"), &json))
        << dir;
    auto manifest = ParseLivePluginManifest(Json(json));
    ASSERT_TRUE(manifest.has_value()) << dir << ": " << manifest.error()[0];
    EXPECT_TRUE(manifest->id.starts_with("compass.")) << dir;
    EXPECT_TRUE(base::PathExists(dir.AppendASCII(manifest->entry))) << dir;
    ++count;
  }
  EXPECT_EQ(count, 5);  // calendar, github, gmail, rss, web-list
}

// Tab previews: the same rules as plugins/runtime/manifest.js and the
// plugins/conformance/preview-* cases.

constexpr char kPreviewBase[] =
    R"("id": "test.preview", "name": "P", "version": "1", "api": 1,
       "folder": false, )";

base::expected<LivePluginManifest, LivePluginErrors> ParsePreviewManifest(
    std::string_view preview) {
  return ParseLivePluginManifest(Json(
      base::StrCat({"{", kPreviewBase, "\"preview\": ", preview, "}"})));
}

TEST(LivePluginManifestTest, PreviewParsesAndMatches) {
  auto manifest = ParsePreviewManifest(R"({
    "matches": ["https://github.com/*", "*://*.example.com/inbox*"],
    "select": {"rows": {"selector": "li", "attrs": ["class"], "text": true,
                        "fields": {"href": "a@href", "title": ".t", "id": "@data-id"},
                        "max": 20}},
    "session": {"related": ["avatars.githubusercontent.com"],
                "pageCookies": ["SID"]},
    "config": [{"key": "count", "type": "number", "label": "N", "default": 5}]
  })");
  // Matches span two sites, so no session.
  ASSERT_FALSE(manifest.has_value());
  EXPECT_THAT(manifest.error(),
              ElementsAre("preview.matches must all be on one site to use "
                          "preview.session"));

  manifest = ParsePreviewManifest(R"({
    "matches": ["https://github.com/*"],
    "select": {"rows": {"selector": "li", "attrs": ["class"], "text": true,
                        "fields": {"href": "a@href", "title": ".t", "id": "@data-id"},
                        "max": 20}},
    "session": {"related": ["avatars.githubusercontent.com"],
                "pageCookies": ["SID"]},
    "config": [{"key": "count", "type": "number", "label": "N", "default": 5}]
  })");
  ASSERT_TRUE(manifest.has_value()) << manifest.error()[0];
  EXPECT_FALSE(manifest->folder);
  ASSERT_TRUE(manifest->preview);
  const LivePluginManifest::Preview& preview = *manifest->preview;
  ASSERT_TRUE(preview.session);
  EXPECT_EQ(preview.session->site, "github.com");
  EXPECT_THAT(preview.session->related,
              ElementsAre("avatars.githubusercontent.com"));
  EXPECT_THAT(preview.session->page_cookies, ElementsAre("SID"));
  ASSERT_EQ(preview.select.size(), 1u);
  const LivePluginManifest::SelectGroup& rows = preview.select.at("rows");
  EXPECT_EQ(rows.max, 20);
  EXPECT_TRUE(rows.text);
  EXPECT_EQ(rows.fields.at("href"), "a@href");
  ASSERT_EQ(preview.config.size(), 1u);

  EXPECT_TRUE(LivePreviewMatches(preview, GURL("https://github.com/a/b")));
  EXPECT_TRUE(LivePreviewMatches(preview, GURL("https://github.com/")));
  EXPECT_FALSE(LivePreviewMatches(preview, GURL("http://github.com/")));
  EXPECT_FALSE(LivePreviewMatches(preview, GURL("https://gist.github.com/")));

  auto inbox = ParsePreviewManifest(
      R"({"matches": ["*://*.example.com/inbox*"]})");
  ASSERT_TRUE(inbox.has_value());
  EXPECT_TRUE(LivePreviewMatches(*inbox->preview,
                                 GURL("http://mail.example.com/inbox?x=1")));
  EXPECT_TRUE(
      LivePreviewMatches(*inbox->preview, GURL("https://example.com/inbox")));
  EXPECT_FALSE(
      LivePreviewMatches(*inbox->preview, GURL("https://example.com/sent")));
}

TEST(LivePluginManifestTest, PreviewRefusesPublicSuffixesAndOtherSites) {
  for (const char* bad : {
           R"({"matches": ["https://*.co.uk/*"]})",
           R"({"matches": ["https://github.io/*"]})",
           R"({"matches": ["https://*.com/*"]})",
           R"({"matches": ["github.com/*"]})",
           R"({"matches": []})",
           R"({"matches": ["https://a.com/*"], "session": {"related": ["*.github.io"]}})",
           R"({"matches": ["https://a.com/*"], "session": {"related": ["*.com"]}})",
           R"({"matches": ["https://a.com/*"], "session": {"related": ["x.a.com"]}})",
           R"({"matches": ["https://a.com/*"], "session": {"hosts": ["b.com"]}})",
           R"({"matches": ["https://a.com/*"], "session": {"pageCookies": ["a b"]}})",
           R"({"matches": ["https://a.com/*"], "select": {"g": {"selector": ""}}})",
           R"({"matches": ["https://a.com/*"], "select": {"g": {"selector": "a", "fields": {"x": "a@"}}}})",
           R"({"matches": ["https://a.com/*"], "select": {"g": {"selector": "a", "max": 500}}})",
       }) {
    EXPECT_FALSE(ParsePreviewManifest(bad).has_value()) << bad;
  }
  // Each github.io site is its own: a subdomain host pattern of it is fine.
  EXPECT_TRUE(ParsePreviewManifest(
                  R"({"matches": ["https://alice.github.io/*"],
                      "session": {"hosts": ["alice.github.io"]}})")
                  .has_value());
  // A plugin has to provide something.
  EXPECT_FALSE(ParseLivePluginManifest(Json(base::StrCat(
                                           {"{", kPreviewBase, "\"x\": 1}"})))
                   .has_value());
}

TEST(LivePluginManifestTest, SiteOf) {
  EXPECT_EQ(LiveSiteOf("calendar.google.com"), "google.com");
  EXPECT_EQ(LiveSiteOf("Calendar.Google.com."), "google.com");
  EXPECT_EQ(LiveSiteOf("alice.github.io"), "alice.github.io");
  EXPECT_EQ(LiveSiteOf("a.b.co.uk"), "b.co.uk");
  EXPECT_EQ(LiveSiteOf("github.io"), "");
  EXPECT_EQ(LiveSiteOf("127.0.0.1"), "");
}

}  // namespace
}  // namespace compass
