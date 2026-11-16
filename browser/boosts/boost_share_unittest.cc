// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/boosts/boost_share.h"

#include <optional>
#include <string>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::boosts {
namespace {

// common/boosts/share_vectors.json: the vectors every Compass client tests,
// an array. Those with an "error" must fail; the others decode.
std::vector<base::DictValue> Vectors(bool failing) {
  base::FilePath root;
  EXPECT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
  std::string contents;
  EXPECT_TRUE(base::ReadFileToString(
      root.AppendASCII("compass/common/boosts/share_vectors.json"),
      &contents));
  std::optional<base::Value> vectors =
      base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
  EXPECT_TRUE(vectors && vectors->is_list());
  std::vector<base::DictValue> found;
  if (vectors && vectors->is_list()) {
    for (const base::Value& item : vectors->GetList()) {
      if (item.is_dict() &&
          (item.GetDict().FindString("error") != nullptr) == failing) {
        found.push_back(item.GetDict().Clone());
      }
    }
  }
  return found;
}

std::string Str(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

ShareError::Kind KindNamed(std::string_view name) {
  if (name == "newer") {
    return ShareError::Kind::kNewer;
  }
  if (name == "too_big") {
    return ShareError::Kind::kTooBig;
  }
  if (name == "refused") {
    return ShareError::Kind::kRefused;
  }
  return ShareError::Kind::kInvalid;
}

Boost MakeBoost() {
  Boost boost;
  boost.id = "0f8fad5b-d9cb-469f-a165-70867728950e";
  boost.name = "Quieter “GitHub”";
  boost.match = "*.github.com/settings";
  boost.css = "header { display: none; }\n";
  boost.js = "console.log('\\u00e8 \"quoted\"');\n";
  boost.js_world = kMainWorld;
  boost.enabled = false;
  boost.author = kAuthorClaude;
  return boost;
}

TEST(BoostShareTest, VectorsDecode) {
  const std::vector<base::DictValue> valid = Vectors(/*failing=*/false);
  ASSERT_FALSE(valid.empty());
  for (const base::DictValue& vector : valid) {
    SCOPED_TRACE(Str(vector, "name"));
    base::expected<Boost, ShareError> boost =
        DecodeShareFragment(Str(vector, "fragment"));
    ASSERT_TRUE(boost.has_value()) << boost.error().message;
    const base::DictValue* expected = vector.FindDict("expected");
    ASSERT_TRUE(expected);
    EXPECT_EQ(boost->name, Str(*expected, "name"));
    EXPECT_EQ(boost->match, Str(*expected, "match"));
    EXPECT_EQ(boost->css, Str(*expected, "css"));
    EXPECT_EQ(boost->js, Str(*expected, "js"));
    EXPECT_EQ(boost->js_world, Str(*expected, "js_world"));
    // A new Boost: no id, on, and someone else's.
    EXPECT_TRUE(boost->id.empty());
    EXPECT_TRUE(boost->enabled);
    EXPECT_EQ(boost->author, kAuthorShared);

    // The file is the same JSON, uncompressed.
    base::expected<Boost, ShareError> from_json =
        ParseSharedJson(Str(vector, "json"));
    ASSERT_TRUE(from_json.has_value());
    EXPECT_EQ(*from_json, *boost);

    // As a link, the fragment comes back out of the URL.
    EXPECT_EQ(ShareFragmentFromUrl(
                  GURL(base::StrCat({kShareLinkPrefix,
                                     Str(vector, "fragment")}))),
              Str(vector, "fragment"));
  }
}

TEST(BoostShareTest, VectorsRefused) {
  const std::vector<base::DictValue> invalid = Vectors(/*failing=*/true);
  ASSERT_FALSE(invalid.empty());
  for (const base::DictValue& vector : invalid) {
    SCOPED_TRACE(Str(vector, "name"));
    base::expected<Boost, ShareError> boost =
        DecodeShareFragment(Str(vector, "fragment"));
    ASSERT_FALSE(boost.has_value());
    EXPECT_EQ(boost.error().kind, KindNamed(Str(vector, "error")))
        << boost.error().message;
    EXPECT_FALSE(boost.error().message.empty());
  }
}

TEST(BoostShareTest, RoundTrip) {
  const Boost boost = MakeBoost();
  const std::string link = ShareLink(boost);
  ASSERT_TRUE(link.starts_with(kShareLinkPrefix));
  std::optional<std::string> fragment = ShareFragmentFromUrl(GURL(link));
  ASSERT_TRUE(fragment);
  EXPECT_EQ(*fragment, EncodeShareFragment(boost));
  // base64url without padding.
  EXPECT_EQ(fragment->find_first_of("+/="), std::string::npos);

  base::expected<Boost, ShareError> back = DecodeShareFragment(*fragment);
  ASSERT_TRUE(back.has_value()) << back.error().message;
  EXPECT_EQ(back->name, boost.name);
  EXPECT_EQ(back->match, boost.match);
  EXPECT_EQ(back->css, boost.css);
  EXPECT_EQ(back->js, boost.js);
  EXPECT_EQ(back->js_world, kMainWorld);
  EXPECT_TRUE(back->id.empty());
  EXPECT_TRUE(back->enabled);
  EXPECT_EQ(back->author, kAuthorShared);

  // The file reads back the same.
  base::expected<Boost, ShareError> from_file =
      ParseSharedJson(ToSharedJson(boost, /*pretty=*/true));
  ASSERT_TRUE(from_file.has_value());
  EXPECT_EQ(*from_file, *back);
}

TEST(BoostShareTest, JsonHasOnlyTheSharedKeys) {
  Boost boost = MakeBoost();
  std::optional<base::DictValue> json = base::JSONReader::ReadDict(
      ToSharedJson(boost, /*pretty=*/false), base::JSON_PARSE_RFC);
  ASSERT_TRUE(json);
  EXPECT_EQ(json->FindInt("v"), 1);
  EXPECT_EQ(Str(*json, "name"), boost.name);
  EXPECT_EQ(Str(*json, "match"), boost.match);
  EXPECT_EQ(Str(*json, "css"), boost.css);
  EXPECT_EQ(Str(*json, "js"), boost.js);
  EXPECT_EQ(Str(*json, "js_world"), "main");
  EXPECT_EQ(json->size(), 6u);

  // CSS only: no js or js_world.
  boost.js.clear();
  json = base::JSONReader::ReadDict(ToSharedJson(boost, /*pretty=*/true),
                                    base::JSON_PARSE_RFC);
  ASSERT_TRUE(json);
  EXPECT_FALSE(json->Find("js"));
  EXPECT_FALSE(json->Find("js_world"));
  EXPECT_EQ(json->size(), 4u);
}

TEST(BoostShareTest, Refuses) {
  // Keys of the wrong type, or no match.
  for (const char* json :
       {R"({"v": 1, "match": "a.test", "css": 5})",
        R"({"v": 1, "name": ["x"], "match": "a.test", "css": "a{}"})",
        R"({"v": 1, "name": "x", "css": "a{}"})",
        R"({"v": "1", "match": "a.test", "css": "a{}"})",
        R"({"v": 1.5, "match": "a.test", "css": "a{}"})", R"([1])",
        R"("a")"}) {
    base::expected<Boost, ShareError> boost = ParseSharedJson(json);
    ASSERT_FALSE(boost.has_value()) << json;
    EXPECT_EQ(boost.error().kind, ShareError::Kind::kInvalid) << json;
  }
  base::expected<Boost, ShareError> newer =
      ParseSharedJson(R"({"v": 2, "match": "a.test", "css": "a{}"})");
  ASSERT_FALSE(newer.has_value());
  EXPECT_EQ(newer.error().kind, ShareError::Kind::kNewer);
  EXPECT_NE(newer.error().message.find("made by a newer Compass"),
            std::string::npos);
  base::expected<Boost, ShareError> world = ParseSharedJson(
      R"({"v": 1, "match": "a.test", "js": "1", "js_world": "elsewhere"})");
  ASSERT_FALSE(world.has_value());
  EXPECT_EQ(world.error().kind, ShareError::Kind::kRefused);
  // A file over the limit isn't parsed at all.
  EXPECT_EQ(ParseSharedJson(std::string(kMaxSharedBoostBytes + 1, ' '))
                .error()
                .kind,
            ShareError::Kind::kTooBig);
  // Empty, or not base64url.
  EXPECT_FALSE(DecodeShareFragment("").has_value());
  EXPECT_FALSE(DecodeShareFragment("a b").has_value());
}

// Just past the limit is refused; well under it isn't, though the JSON is
// mostly padding.
TEST(BoostShareTest, DecompressionLimit) {
  Boost boost;
  boost.name = "Big";
  boost.match = "a.test";
  boost.css = std::string(200 * 1024, 'a');
  base::expected<Boost, ShareError> fine =
      DecodeShareFragment(EncodeShareFragment(boost));
  ASSERT_TRUE(fine.has_value()) << fine.error().message;
  EXPECT_EQ(fine->css.size(), 200u * 1024);

  // Over 256 KB of code: the rules for saving refuse it.
  boost.css = std::string(kMaxBoostBytes + 1, 'a');
  base::expected<Boost, ShareError> too_much_code =
      DecodeShareFragment(EncodeShareFragment(boost));
  ASSERT_FALSE(too_much_code.has_value());
  EXPECT_EQ(too_much_code.error().kind, ShareError::Kind::kRefused);

  // Over 512 KB decompressed: stopped while inflating.
  boost.css = std::string(kMaxSharedBoostBytes, 'a');
  base::expected<Boost, ShareError> bomb =
      DecodeShareFragment(EncodeShareFragment(boost));
  ASSERT_FALSE(bomb.has_value());
  EXPECT_EQ(bomb.error().kind, ShareError::Kind::kTooBig);
}

TEST(BoostShareTest, LinksAndPreview) {
  for (const char* url :
       {"https://compassbrowser.org/boost#abc",
        "https://compassbrowser.org/boost/#abc",
        "https://compassbrowser.org/boost.html#abc",
        "https://compassbrowser.org/boost?x=1#abc",
        "https://www.compassbrowser.org/boost#abc"}) {
    EXPECT_EQ(ShareFragmentFromUrl(GURL(url)), "abc") << url;
  }
  for (const char* url :
       {"https://compassbrowser.org/boost", "https://compassbrowser.org/boost#",
        "http://compassbrowser.org/boost#abc",
        "https://shop.compassbrowser.org/boost#abc",
        "https://compassbrowser.org/boosts#abc",
        "https://compassbrowser.org/#abc", "https://example.com/boost#abc"}) {
    EXPECT_EQ(ShareFragmentFromUrl(GURL(url)), std::nullopt) << url;
  }
  EXPECT_EQ(ImportPreviewUrl("aB-_9").spec(),
            "chrome://settings/boosts?import=aB-_9");
  EXPECT_EQ(ImportPreviewUrl("a&b=c").spec(),
            "chrome://settings/boosts?import=");
}

TEST(BoostShareTest, FileName) {
  Boost boost;
  boost.name = "Quieter GitHub";
  EXPECT_EQ(ShareFileName(boost), "Quieter GitHub.compassboost");
  boost.name = "a/b: c?";
  EXPECT_EQ(ShareFileName(boost), "a-b- c.compassboost");
  boost.name = "...";
  EXPECT_EQ(ShareFileName(boost), "Boost.compassboost");
}

}  // namespace
}  // namespace compass::boosts
