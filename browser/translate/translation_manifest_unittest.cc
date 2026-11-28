// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/translate/translation_manifest.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace compass::translate {
namespace {

std::string Hash(char c) {
  return std::string(64, c);
}

constexpr char kManifest[] = R"({
  "attachments": "https://cdn.example/",
  "directions": {
    "de-en": {"version": "3.0", "architecture": "base", "files": [
      {"type": "model", "location": "a/model.zst", "size": 5,
       "hash": "%s", "decompressed_size": 10, "decompressed_hash": "%s"},
      {"type": "vocab", "location": "a/vocab.zst", "size": 5,
       "hash": "%s", "decompressed_size": 10, "decompressed_hash": "%s"}
    ]}
  }
})";

std::string ManifestJson(
    const std::string& location = "a/model.zst",
    const std::string& attachments = "https://cdn.example/") {
  std::string json = kManifest;
  for (size_t at; (at = json.find("%s")) != std::string::npos;) {
    json.replace(at, 2, Hash('a'));
  }
  json.replace(json.find("a/model.zst"), 11, location);
  json.replace(json.find("https://cdn.example/"), 20, attachments);
  return json;
}

TEST(TranslationManifestTest, BuiltInManifestParses) {
  const translate::Manifest& manifest = BuiltInManifest();
  EXPECT_EQ(manifest.attachments,
            "https://assets.compassbrowser.org/translation/");
  // Every pack upstream knows has both directions.
  for (const char* language :
       {"ar", "bg", "bn", "cs", "da", "de", "el",     "es", "fi", "fr",
        "he", "hi", "hr", "hu", "id", "it", "ja",     "kn", "ko", "lt",
        "mr", "nb", "nl", "pl", "pt", "ro", "ru",     "sk", "sl", "sv",
        "ta", "te", "th", "tr", "uk", "vi", "zh-Hans"}) {
    EXPECT_TRUE(manifest.Find(language, "en")) << language;
    EXPECT_TRUE(manifest.Find("en", language)) << language;
  }
  EXPECT_TRUE(manifest.Find("en", "zh-Hant"));
}

TEST(TranslationManifestTest, Parses) {
  std::optional<translate::Manifest> manifest = ParseManifest(ManifestJson());
  ASSERT_TRUE(manifest);
  const ModelDirection* direction = manifest->Find("de", "en");
  ASSERT_TRUE(direction);
  EXPECT_EQ(direction->version, "3.0");
  ASSERT_EQ(direction->files.size(), 2u);
  EXPECT_EQ(direction->files[0].type, "model");
  EXPECT_EQ(direction->files[0].size, 5);
  EXPECT_FALSE(manifest->Find("en", "de"));
}

TEST(TranslationManifestTest, RefusesWhatItShouldnt) {
  EXPECT_FALSE(ParseManifest("not json"));
  EXPECT_FALSE(
      ParseManifest(ManifestJson("a/model.zst", "http://cdn.example/")));
  EXPECT_FALSE(ParseManifest(ManifestJson("../escape.zst")));
  EXPECT_FALSE(ParseManifest(ManifestJson("/absolute.zst")));
  std::string bad_hash = ManifestJson();
  bad_hash.replace(bad_hash.find(Hash('a')), 64, "xyz");
  EXPECT_FALSE(ParseManifest(bad_hash));
}

TEST(TranslationManifestTest, LanguageCodes) {
  EXPECT_EQ(ToModelLanguage("iw"), "he");
  EXPECT_EQ(ToModelLanguage("no"), "nb");
  EXPECT_EQ(ToModelLanguage("zh"), "zh-Hans");
  EXPECT_EQ(ToModelLanguage("zh-Hant"), "zh-Hant");
  EXPECT_EQ(ToModelLanguage("de"), "de");
}

}  // namespace
}  // namespace compass::translate
