// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/translate/translation_pack_installer.h"

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "compass/browser/translate/translation_manifest.h"
#include "components/on_device_translation/public/language_pack.h"
#include "components/on_device_translation/public/pref_names.h"
#include "components/prefs/testing_pref_service.h"
#include "crypto/hash.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/zstd/src/lib/zstd.h"

namespace compass::translate {
namespace {

using on_device_translation::GetComponentPathPrefName;
using on_device_translation::GetLanguagePackComponentConfig;
using on_device_translation::GetRegisteredFlagPrefName;
using on_device_translation::LanguagePackKey;

constexpr char kCdn[] = "https://cdn.example/";
constexpr LanguagePackKey kGerman = LanguagePackKey::kDe_En;

std::string Zstd(const std::string& plain) {
  std::string out(ZSTD_compressBound(plain.size()), '\0');
  size_t size =
      ZSTD_compress(out.data(), out.size(), plain.data(), plain.size(), 3);
  CHECK(!ZSTD_isError(size));
  out.resize(size);
  return out;
}

std::string Sha256(const std::string& data) {
  return base::ToLowerASCII(base::HexEncode(crypto::hash::Sha256(data)));
}

class PackInstallerTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    on_device_translation::RegisterLocalStatePrefs(prefs_.registry());
    RegisterLocalStatePrefs(prefs_.registry());
    prefs_.SetBoolean(kModelDownloadsAllowed, true);
    manifest_.attachments = kCdn;
    AddDirection(&manifest_, "de-en", "de2en");
    AddDirection(&manifest_, "en-de", "en2de");
    installer_.SetForTesting(
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &factory_),
        &manifest_, dir_.GetPath());
    installer_.SetResultCallbackForTesting(
        base::BindLambdaForTesting([this](LanguagePackKey, bool ok) {
          result_ = ok;
          if (run_loop_) {
            run_loop_->Quit();
          }
        }));
  }

  // A direction whose model and vocabulary hold `name`'s text, served by
  // the factory.
  void AddDirection(Manifest* manifest,
                    const std::string& direction,
                    const std::string& name,
                    bool serve = true) {
    ModelDirection& entry = manifest->directions[direction];
    entry.version = "3.0";
    entry.architecture = "base";
    entry.files.clear();
    for (const char* type : {"model", "vocab"}) {
      const std::string plain = name + "-" + type + std::string(100, 'x');
      const std::string compressed = Zstd(plain);
      ModelFile file{type,
                     name + "/" + type + ".zst",
                     static_cast<int64_t>(compressed.size()),
                     Sha256(compressed),
                     static_cast<int64_t>(plain.size()),
                     Sha256(plain)};
      if (serve) {
        factory_.AddResponse(kCdn + file.location, compressed);
      }
      entry.files.push_back(std::move(file));
    }
  }

  // Installs, and waits for the result.
  std::optional<bool> Install() {
    result_.reset();
    run_loop_.emplace();
    installer_.Install(kGerman, &prefs_,
                       base::BindLambdaForTesting([this] { ++ready_; }));
    run_loop_->Run();
    run_loop_.reset();
    return result_;
  }

  base::FilePath Pack() { return dir_.GetPath().AppendASCII("de_en"); }

  std::string Read(const std::string& relative) {
    std::string text;
    base::ReadFileToString(Pack().AppendASCII(relative), &text);
    return text;
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
  TestingPrefServiceSimple prefs_;
  network::TestURLLoaderFactory factory_;
  Manifest manifest_;
  PackInstaller installer_;
  std::optional<base::RunLoop> run_loop_;
  std::optional<bool> result_;
  int ready_ = 0;
};

TEST_F(PackInstallerTest, InstallsBothDirections) {
  EXPECT_EQ(Install(), true);
  EXPECT_EQ(ready_, 1);
  EXPECT_TRUE(base::StartsWith(Read("de-en/model.bin"), "de2en-model"));
  EXPECT_TRUE(base::StartsWith(Read("de-en/vocab.spm"), "de2en-vocab"));
  EXPECT_TRUE(base::StartsWith(Read("en-de/model.bin"), "en2de-model"));
  EXPECT_FALSE(base::PathExists(dir_.GetPath().AppendASCII(".staging-de_en")));
  const auto& config = GetLanguagePackComponentConfig(kGerman);
  EXPECT_EQ(prefs_.GetFilePath(GetComponentPathPrefName(config)), Pack());
  EXPECT_TRUE(prefs_.GetBoolean(GetRegisteredFlagPrefName(config)));
}

TEST_F(PackInstallerTest, NothingWithoutConsent) {
  prefs_.SetBoolean(kModelDownloadsAllowed, false);
  installer_.Install(kGerman, &prefs_, base::DoNothing());
  EXPECT_EQ(result_, false);
  EXPECT_EQ(factory_.NumPending(), 0);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(factory_.total_requests(), 0u);
  EXPECT_FALSE(base::PathExists(Pack()));
}

TEST_F(PackInstallerTest, RefusesAFileThatDoesntMatch) {
  factory_.AddResponse(kCdn + manifest_.directions["en-de"].files[0].location,
                       Zstd("something else"));
  EXPECT_EQ(Install(), false);
  EXPECT_EQ(ready_, 0);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(base::PathExists(Pack()));
  const auto& config = GetLanguagePackComponentConfig(kGerman);
  EXPECT_TRUE(prefs_.GetFilePath(GetComponentPathPrefName(config)).empty());
}

TEST_F(PackInstallerTest, FailsWhenAFileIsMissing) {
  factory_.AddResponse(kCdn + manifest_.directions["en-de"].files[1].location,
                       "", net::HTTP_NOT_FOUND);
  EXPECT_EQ(Install(), false);
  EXPECT_EQ(ready_, 0);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(base::PathExists(Pack()));
  EXPECT_FALSE(base::PathExists(dir_.GetPath().AppendASCII(".staging-de_en")));
}

// Real models are tens of MB: more than SimpleURLLoader keeps in a string
// (5 MB), so they go to a file.
TEST_F(PackInstallerTest, DownloadsLargeModels) {
  std::string plain(6 * 1024 * 1024, '\0');
  uint32_t state = 12345;
  for (char& c : plain) {
    state = state * 1664525u + 1013904223u;  // Doesn't compress.
    c = static_cast<char>(state >> 24);
  }
  const std::string compressed = Zstd(plain);
  ASSERT_GT(compressed.size(), 5u * 1024 * 1024);
  ModelFile& model = manifest_.directions["en-de"].files[0];
  model.location = "big/model.zst";
  model.size = static_cast<int64_t>(compressed.size());
  model.hash = Sha256(compressed);
  model.decompressed_size = static_cast<int64_t>(plain.size());
  model.decompressed_hash = Sha256(plain);
  factory_.AddResponse(kCdn + model.location, compressed);

  // The pill's progress: rising, to every file's bytes.
  int64_t expected_total = 0;
  for (const auto& [direction, files] : manifest_.directions) {
    for (const ModelFile& file : files.files) {
      expected_total += file.size;
    }
  }
  std::vector<std::pair<int64_t, int64_t>> progress;
  installer_.OnProgress(
      kGerman, base::BindLambdaForTesting([&](int64_t received, int64_t total) {
        progress.emplace_back(received, total);
      }));
  EXPECT_EQ(Install(), true);
  EXPECT_EQ(Read("en-de/model.bin"), plain);
  ASSERT_FALSE(progress.empty());
  EXPECT_TRUE(std::ranges::is_sorted(progress));
  EXPECT_EQ(progress.back(), std::make_pair(expected_total, expected_total));
}

TEST_F(PackInstallerTest, UninstallForgetsAndDeletes) {
  ASSERT_EQ(Install(), true);
  installer_.Uninstall(kGerman, &prefs_);
  task_environment_.RunUntilIdle();
  const auto& config = GetLanguagePackComponentConfig(kGerman);
  EXPECT_TRUE(prefs_.GetFilePath(GetComponentPathPrefName(config)).empty());
  EXPECT_FALSE(prefs_.GetBoolean(GetRegisteredFlagPrefName(config)));
  EXPECT_FALSE(base::PathExists(Pack()));
}

}  // namespace
}  // namespace compass::translate
