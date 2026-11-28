// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/translatekit/engine.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/string_number_conversions.h"
#include "compass/components/translatekit/disk_file_source.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::translatekit {
namespace {

// Files in memory, with the paths asked for.
class FakeFileSource : public FileSource {
 public:
  explicit FakeFileSource(std::map<std::string, std::string> files)
      : files_(std::move(files)) {}

  bool Exists(const std::string& path) override {
    return files_.count(path) > 0;
  }
  std::optional<std::string> Read(const std::string& path) override {
    auto it = files_.find(path);
    if (it == files_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

 private:
  std::map<std::string, std::string> files_;
};

std::unique_ptr<Engine> MakeEngine(std::map<std::string, std::string> files,
                                   std::vector<Package> packages) {
  auto engine = std::make_unique<Engine>(
      std::make_unique<FakeFileSource>(std::move(files)), /*workers=*/1);
  engine->SetPackages(std::move(packages));
  return engine;
}

TEST(TranslateKitEngineTest, NormalizesLanguages) {
  EXPECT_EQ(NormalizeLanguage("de"), "de");
  EXPECT_EQ(NormalizeLanguage("zh-Hant"), "zh-hant");
  EXPECT_EQ(NormalizeLanguage("zh_Hant"), "zh-hant");
  EXPECT_EQ(NormalizeLanguage("EN"), "en");
}

TEST(TranslateKitEngineTest, FindsDirectionsInEitherPackageOrder) {
  auto engine = MakeEngine(
      {
          {"/0/de-en/model.bin", "m"},
          {"/0/en-de/model.bin", "m"},
          {"/1/it-en/model.bin", "m"},
      },
      {{"en", "de", "/0/"}, {"it", "en", "/1"}});
  EXPECT_EQ(engine->FindDirection("de", "en"), "/0/de-en");
  EXPECT_EQ(engine->FindDirection("EN", "de"), "/0/en-de");
  EXPECT_EQ(engine->FindDirection("it", "en"), "/1/it-en");
  // The package has the pair, but not this direction.
  EXPECT_EQ(engine->FindDirection("en", "it"), std::nullopt);
  EXPECT_EQ(engine->FindDirection("fr", "en"), std::nullopt);
}

TEST(TranslateKitEngineTest, NoTranslatorWithoutModels) {
  auto engine = MakeEngine({{"/0/de-en/model.bin", "m"}}, {{"de", "en", "/0"}});
  EXPECT_FALSE(engine->CreateTranslator("en", "de"));
  EXPECT_FALSE(engine->CreateTranslator("de", "de"));
  EXPECT_FALSE(engine->CreateTranslator("", "en"));
  // Pivoting needs both directions through English.
  EXPECT_FALSE(engine->CreateTranslator("de", "it"));
}

TEST(TranslateKitEngineTest, BrokenModelGivesNoTranslator) {
  // Marian aborts on a model that isn't one; the engine must survive it.
  auto engine = MakeEngine(
      {
          {"/0/de-en/model.bin", "not a model"},
          {"/0/de-en/vocab.spm", "not a vocabulary"},
      },
      {{"de", "en", "/0"}});
  EXPECT_FALSE(engine->CreateTranslator("de", "en"));
}

TEST(TranslateKitEngineTest, MissingVocabularyGivesNoTranslator) {
  auto engine = MakeEngine({{"/0/de-en/model.bin", "m"}}, {{"de", "en", "/0"}});
  EXPECT_FALSE(engine->CreateTranslator("de", "en"));
}

TEST(TranslateKitEngineTest, DefaultWorkers) {
  EXPECT_GE(Engine::DefaultWorkers(), 1u);
  EXPECT_LE(Engine::DefaultWorkers(), 4u);
}

// With real models: COMPASS_TRANSLATEKIT_TEST_PACKAGES names a folder holding
// a "de-en" package with the de-en direction (translatekit_cli's layout).
// Skipped otherwise.
TEST(TranslateKitEngineTest, TranslatesWithRealModels) {
  std::optional<std::string> packages =
      base::Environment::Create()->GetVar("COMPASS_TRANSLATEKIT_TEST_PACKAGES");
  if (!packages || packages->empty()) {
    GTEST_SKIP() << "COMPASS_TRANSLATEKIT_TEST_PACKAGES isn't set";
  }
  Engine engine(std::make_unique<DiskFileSource>(), /*workers=*/2);
  engine.SetPackages({{"de", "en", *packages + "/de-en"}});
  auto translator = engine.CreateTranslator("de", "en");
  ASSERT_TRUE(translator);

  auto text = translator->Translate("Guten Morgen. Wie geht es dir?", false);
  ASSERT_TRUE(text);
  EXPECT_NE(text->find("Good morning"), std::string::npos) << *text;

  auto html = translator->Translate(
      "<p>Klicken Sie <a href=\"/hilfe\">hier</a>, um fortzufahren.</p>", true);
  ASSERT_TRUE(html);
  EXPECT_NE(html->find("<a href=\"/hilfe\">"), std::string::npos) << *html;
  EXPECT_NE(html->find("</a>"), std::string::npos) << *html;
  EXPECT_EQ(html->find("Klicken"), std::string::npos) << *html;

  EXPECT_EQ(translator->Translate("", false), "");
}

// Every worker loads its own copy of the model the first time it gets a
// batch; Bergamot freed the model's memory after the first one, so the
// others found none (patches/0005-load-every-worker-backend.patch). A page
// long enough to keep 4 workers busy.
TEST(TranslateKitEngineTest, EveryWorkerTranslates) {
  std::optional<std::string> packages =
      base::Environment::Create()->GetVar("COMPASS_TRANSLATEKIT_TEST_PACKAGES");
  if (!packages || packages->empty()) {
    GTEST_SKIP() << "COMPASS_TRANSLATEKIT_TEST_PACKAGES isn't set";
  }
  Engine engine(std::make_unique<DiskFileSource>(), /*workers=*/4);
  engine.SetPackages({{"de", "en", *packages + "/de-en"}});
  auto translator = engine.CreateTranslator("de", "en");
  ASSERT_TRUE(translator);

  std::string page;
  for (int i = 0; i < 200; ++i) {
    page += "Am " + base::NumberToString(i + 1) +
            ". Tag regnete es in der Stadt. Die Kinder spielten trotzdem "
            "im Park.\n\n";
  }
  auto text = translator->Translate(page, false);
  ASSERT_TRUE(text);
  // "On the 3. Day it was raining in the city. The kids were still playing
  // in the park.", for each day: all translated, none left in German.
  EXPECT_NE(text->find("park"), std::string::npos);
  EXPECT_EQ(text->find("Kinder"), std::string::npos);
  EXPECT_EQ(text->find("Park."), std::string::npos);
}

}  // namespace
}  // namespace compass::translatekit
