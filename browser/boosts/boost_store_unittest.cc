// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/boosts/boost_store.h"

#include <memory>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/uuid.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::boosts {
namespace {

BoostMatch Parse(std::string_view match) {
  base::expected<BoostMatch, std::string> parsed = BoostMatch::Parse(match);
  EXPECT_TRUE(parsed.has_value()) << match;
  return parsed.value_or(BoostMatch());
}

TEST(BoostMatchTest, Parses) {
  EXPECT_EQ(Parse("example.com").ToString(), "example.com");
  EXPECT_FALSE(Parse("example.com").subdomains);
  EXPECT_EQ(Parse("  Example.COM ").ToString(), "example.com");
  // "www." is the site itself, which covers it.
  EXPECT_EQ(Parse("www.example.com").ToString(), "example.com");
  EXPECT_EQ(Parse("*.example.com").ToString(), "*.example.com");
  EXPECT_TRUE(Parse("*.example.com").subdomains);
  EXPECT_EQ(Parse("*.www.example.com").ToString(), "*.www.example.com");
  EXPECT_EQ(Parse("github.com/settings").ToString(), "github.com/settings");
  EXPECT_EQ(Parse("github.com/settings").path, "/settings");
  // Pasted URLs, trailing slashes and stars.
  EXPECT_EQ(Parse("https://github.com/settings/").ToString(),
            "github.com/settings");
  EXPECT_EQ(Parse("http://example.com/*").ToString(), "example.com");
  EXPECT_EQ(Parse("example.com/").ToString(), "example.com");
  // The path keeps its case.
  EXPECT_EQ(Parse("example.com/Docs").path, "/Docs");
  // IDN as punycode.
  EXPECT_EQ(Parse("bücher.example").host, "xn--bcher-kva.example");
  EXPECT_EQ(Parse("localhost").ToString(), "localhost");
  EXPECT_EQ(Parse("127.0.0.1").ToString(), "127.0.0.1");
}

TEST(BoostMatchTest, Refuses) {
  for (const char* match :
       {"", "  ", "*", "*.com", "*.", "exa*mple.com", "example.com:8080",
        "user@example.com", "ftp://example.com", "chrome://settings",
        "file:///etc", "example.com/a?b", "example.com/a#b",
        "example.com/a*b", "*.127.0.0.1"}) {
    EXPECT_FALSE(BoostMatch::Parse(match).has_value()) << match;
  }
}

TEST(BoostMatchTest, MatchesUrls) {
  const BoostMatch exact = Parse("example.com");
  EXPECT_TRUE(exact.Matches(GURL("https://example.com/")));
  EXPECT_TRUE(exact.Matches(GURL("http://example.com/a/b?c")));
  EXPECT_TRUE(exact.Matches(GURL("https://www.example.com/")));
  EXPECT_TRUE(exact.Matches(GURL("https://example.com:8443/")));
  EXPECT_FALSE(exact.Matches(GURL("https://mail.example.com/")));
  EXPECT_FALSE(exact.Matches(GURL("https://notexample.com/")));
  EXPECT_FALSE(exact.Matches(GURL("https://example.com.evil.test/")));
  EXPECT_FALSE(exact.Matches(GURL("ftp://example.com/")));
  EXPECT_FALSE(exact.Matches(GURL("chrome://example.com/")));
  EXPECT_FALSE(exact.Matches(GURL()));

  const BoostMatch domain = Parse("*.example.com");
  EXPECT_TRUE(domain.Matches(GURL("https://example.com/")));
  EXPECT_TRUE(domain.Matches(GURL("https://www.example.com/")));
  EXPECT_TRUE(domain.Matches(GURL("https://a.b.example.com/x")));
  EXPECT_FALSE(domain.Matches(GURL("https://badexample.com/")));

  const BoostMatch path = Parse("github.com/settings");
  EXPECT_TRUE(path.Matches(GURL("https://github.com/settings")));
  EXPECT_TRUE(path.Matches(GURL("https://github.com/settings/profile")));
  EXPECT_TRUE(path.Matches(GURL("https://www.github.com/settings")));
  EXPECT_FALSE(path.Matches(GURL("https://github.com/")));
  EXPECT_FALSE(path.Matches(GURL("https://github.com/explore")));
}

TEST(BoostMatchTest, MatchesHosts) {
  EXPECT_TRUE(Parse("github.com/settings").MatchesHost("github.com"));
  EXPECT_TRUE(Parse("github.com").MatchesHost("www.github.com"));
  EXPECT_FALSE(Parse("github.com").MatchesHost("gist.github.com"));
  EXPECT_TRUE(Parse("*.github.com").MatchesHost("gist.github.com"));
}

TEST(BoostMatchTest, UrlPatterns) {
  EXPECT_EQ(Parse("example.com").ToUrlPatterns(),
            (std::vector<std::string>{"*://example.com/*",
                                      "*://www.example.com/*"}));
  EXPECT_EQ(Parse("*.example.com").ToUrlPatterns(),
            (std::vector<std::string>{"*://*.example.com/*"}));
  EXPECT_EQ(Parse("github.com/settings").ToUrlPatterns(),
            (std::vector<std::string>{"*://github.com/settings*",
                                      "*://www.github.com/settings*"}));
  EXPECT_EQ(Parse("127.0.0.1").ToUrlPatterns(),
            (std::vector<std::string>{"*://127.0.0.1/*"}));
}

Boost MakeBoost(std::string match, std::string css, std::string js = "") {
  Boost boost;
  boost.match = std::move(match);
  boost.css = std::move(css);
  boost.js = std::move(js);
  return boost;
}

TEST(NormalizeBoostTest, ChecksWhatsSaved) {
  base::expected<Boost, std::string> boost =
      NormalizeBoost(MakeBoost("WWW.Example.com", "body{}"));
  ASSERT_TRUE(boost.has_value());
  EXPECT_EQ(boost->match, "example.com");
  // No name: the match.
  EXPECT_EQ(boost->name, "example.com");
  EXPECT_EQ(boost->js_world, kIsolatedWorld);
  EXPECT_EQ(boost->author, kAuthorUser);

  Boost named = MakeBoost("example.com", "", "alert(1)");
  named.name = "  Quieter\n  example ";
  named.js_world = "main";
  named.author = "claude";
  boost = NormalizeBoost(named);
  ASSERT_TRUE(boost.has_value());
  EXPECT_EQ(boost->name, "Quieter example");
  EXPECT_EQ(boost->js_world, kMainWorld);
  EXPECT_EQ(boost->author, kAuthorClaude);

  // Nothing to run.
  EXPECT_FALSE(NormalizeBoost(MakeBoost("example.com", " ", "\n")).has_value());
  // Not a site.
  EXPECT_FALSE(NormalizeBoost(MakeBoost("*", "body{}")).has_value());
  // Another world.
  Boost world = MakeBoost("example.com", "", "x()");
  world.js_world = "page";
  EXPECT_FALSE(NormalizeBoost(world).has_value());
  // Too big: 256 KB of CSS and JS together.
  EXPECT_TRUE(NormalizeBoost(MakeBoost("example.com",
                                       std::string(kMaxBoostBytes - 1, 'a'),
                                       "b"))
                  .has_value());
  EXPECT_FALSE(NormalizeBoost(MakeBoost("example.com",
                                        std::string(kMaxBoostBytes, 'a'), "b"))
                   .has_value());
  // A name too long.
  Boost long_name = MakeBoost("example.com", "a{}");
  long_name.name = std::string(kMaxBoostNameChars + 1, 'n');
  EXPECT_FALSE(NormalizeBoost(long_name).has_value());
  // An id that isn't one.
  Boost bad_id = MakeBoost("example.com", "a{}");
  bad_id.id = "nope";
  EXPECT_FALSE(NormalizeBoost(bad_id).has_value());
}

class BoostStoreTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }

  base::FilePath Path() const {
    return dir_.GetPath().AppendASCII("Compass Boosts.json");
  }

  std::unique_ptr<BoostStore> Load() {
    auto store = std::make_unique<BoostStore>(Path());
    base::test::TestFuture<void> loaded;
    store->WhenLoaded(loaded.GetCallback());
    EXPECT_TRUE(loaded.Wait());
    return store;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir dir_;
};

TEST_F(BoostStoreTest, SaveUpdateRemovePersist) {
  std::string id;
  {
    std::unique_ptr<BoostStore> store = Load();
    ASSERT_TRUE(store->boosts().empty());
    Boost made = MakeBoost("github.com", "header{display:none}");
    made.name = "Quieter GitHub";
    made.author = kAuthorClaude;
    base::expected<Boost, std::string> saved = store->Save(made);
    ASSERT_TRUE(saved.has_value());
    id = saved->id;
    EXPECT_TRUE(base::Uuid::ParseLowercase(id).is_valid());
    EXPECT_EQ(saved->author, kAuthorClaude);
    EXPECT_FALSE(saved->created.is_null());

    task_environment_.FastForwardBy(base::Seconds(1));
    Boost edit = *saved;
    edit.js = "console.log('hi')";
    // Saving doesn't change who made it.
    edit.author = kAuthorUser;
    base::expected<Boost, std::string> updated = store->Save(edit);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->id, id);
    EXPECT_EQ(updated->author, kAuthorClaude);
    EXPECT_EQ(updated->created, saved->created);
    EXPECT_GT(updated->updated, saved->updated);

    // An id no Boost has.
    Boost unknown = edit;
    unknown.id = base::Uuid::GenerateRandomV4().AsLowercaseString();
    EXPECT_FALSE(store->Save(unknown).has_value());

    ASSERT_TRUE(store->Save(MakeBoost("example.com", "p{}")).has_value());
    EXPECT_TRUE(store->SetEnabled(id, false));
    EXPECT_FALSE(store->SetEnabled("nope", false));
    EXPECT_EQ(store->boosts().size(), 2u);
    // Destroying writes.
  }
  task_environment_.RunUntilIdle();

  // The file is an array with snake_case keys.
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(Path(), &contents));
  std::optional<base::Value> file =
      base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
  ASSERT_TRUE(file && file->is_list());
  ASSERT_EQ(file->GetList().size(), 2u);
  const base::DictValue& first = file->GetList()[0].GetDict();
  EXPECT_EQ(*first.FindString("id"), id);
  EXPECT_EQ(*first.FindString("js_world"), "isolated");
  EXPECT_EQ(*first.FindString("author"), "claude");
  EXPECT_FALSE(*first.FindBool("enabled"));
  EXPECT_TRUE(first.FindString("create_time"));
  EXPECT_TRUE(first.FindString("update_time"));

  std::unique_ptr<BoostStore> store = Load();
  ASSERT_EQ(store->boosts().size(), 2u);
  EXPECT_EQ(store->boosts()[0].id, id);
  EXPECT_EQ(store->boosts()[0].name, "Quieter GitHub");
  EXPECT_EQ(store->boosts()[0].js, "console.log('hi')");
  EXPECT_FALSE(store->boosts()[0].enabled);
  EXPECT_EQ(store->ForUrl(GURL("https://www.github.com/x")).size(), 1u);
  EXPECT_TRUE(store->ForUrl(GURL("https://gitlab.com/")).empty());

  std::optional<Boost> removed = store->Remove(id);
  ASSERT_TRUE(removed);
  EXPECT_EQ(store->boosts().size(), 1u);
  // Undo.
  EXPECT_TRUE(store->Restore(*removed));
  EXPECT_EQ(store->boosts().size(), 2u);
  EXPECT_EQ(store->boosts()[0].id, id);
}

TEST_F(BoostStoreTest, Limits) {
  std::unique_ptr<BoostStore> store = Load();
  for (size_t i = 0; i < kMaxBoosts; ++i) {
    ASSERT_TRUE(store->Save(MakeBoost("example.com", "a{}")).has_value());
  }
  EXPECT_FALSE(store->Save(MakeBoost("example.com", "a{}")).has_value());
  // Sync isn't capped: one dropped here would be deleted everywhere.
  Boost remote = MakeBoost("example.com", "a{}");
  remote.id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  store->ApplyRemote({remote}, {});
  EXPECT_EQ(store->boosts().size(), kMaxBoosts + 1);
}

TEST_F(BoostStoreTest, UnreadableEntriesAreSkipped) {
  const std::string id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  ASSERT_TRUE(base::WriteFile(
      Path(), "[{\"id\": \"" + id +
                  "\", \"match\": \"example.com\", \"css\": \"a{}\"},"
                  " {\"id\": \"x\", \"match\": \"example.com\"},"
                  " {\"id\": \"" +
                  base::Uuid::GenerateRandomV4().AsLowercaseString() +
                  "\", \"match\": \"*\"}, 7]"));
  std::unique_ptr<BoostStore> store = Load();
  ASSERT_EQ(store->boosts().size(), 1u);
  EXPECT_EQ(store->boosts()[0].id, id);
  // What's missing has its default.
  EXPECT_TRUE(store->boosts()[0].enabled);
  EXPECT_EQ(store->boosts()[0].js_world, kIsolatedWorld);
}

TEST_F(BoostStoreTest, ApplyRemoteLaterEditWins) {
  std::unique_ptr<BoostStore> store = Load();
  base::expected<Boost, std::string> local =
      store->Save(MakeBoost("example.com", "a{}"));
  ASSERT_TRUE(local.has_value());

  Boost older = *local;
  older.css = "old{}";
  older.updated = local->updated - base::Seconds(5);
  store->ApplyRemote({older}, {});
  EXPECT_EQ(store->Find(local->id)->css, "a{}");

  Boost newer = *local;
  newer.css = "new{}";
  newer.updated = local->updated + base::Seconds(5);
  Boost added = MakeBoost("*.example.org", "", "x()");
  added.id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  added.author = kAuthorClaude;
  store->ApplyRemote({newer, added}, {});
  EXPECT_EQ(store->Find(local->id)->css, "new{}");
  ASSERT_TRUE(store->Find(added.id));
  EXPECT_EQ(store->Find(added.id)->author, kAuthorClaude);

  store->ApplyRemote({}, {local->id});
  EXPECT_FALSE(store->Find(local->id));
  EXPECT_EQ(store->boosts().size(), 1u);
}

TEST_F(BoostStoreTest, ApplyRemoteWaitsForTheFile) {
  auto store = std::make_unique<BoostStore>(Path());
  Boost remote = MakeBoost("example.com", "a{}");
  remote.id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  store->ApplyRemote({remote}, {});
  base::test::TestFuture<void> loaded;
  store->WhenLoaded(loaded.GetCallback());
  ASSERT_TRUE(loaded.Wait());
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(store->Find(remote.id));
}

}  // namespace
}  // namespace compass::boosts
