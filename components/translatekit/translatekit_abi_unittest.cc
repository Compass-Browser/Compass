// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// libcompass_translatekit through its C ABI, loaded the way upstream's
// TranslateKitClient loads it: every export there, a version the service
// accepts, packages and files through the storage callbacks, and the HTML
// marker.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/base_paths.h"
#include "base/compiler_specific.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/native_library.h"
#include "base/path_service.h"
#include "base/scoped_native_library.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "compass/components/translatekit/html_marker.h"
#include "components/on_device_translation/features.h"
#include "components/on_device_translation/service/proto/translate_kit_api.pb.h"
#include "components/on_device_translation/service/translate_kit_structs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::translatekit {
namespace {

using GetVersionFn = bool (*)(TranslateKitVersion*);
using InitStorageFn = void (*)(FileExistsFn,
                               OpenForReadOnlyMemoryMapFn,
                               DeleteReadOnlyMemoryRegionFn,
                               ReadOnlyMemoryRegionDataFn,
                               ReadOnlyMemoryRegionLengthFn,
                               std::uintptr_t);
using CreateKitFn = std::uintptr_t (*)();
using DeleteFn = void (*)(std::uintptr_t);
using SetPackagesFn = bool (*)(std::uintptr_t,
                               TranslateKitSetLanguagePackagesArgs);
using CreateTranslatorFn = std::uintptr_t (*)(std::uintptr_t,
                                              TranslateKitLanguage,
                                              TranslateKitLanguage);
using OutputFn = void (*)(TranslateKitOutputText, std::uintptr_t);
using TranslateFn = bool (*)(std::uintptr_t,
                             TranslateKitInputText,
                             OutputFn,
                             std::uintptr_t);
using SplitFn = bool (*)(TranslateKitInputText,
                         TranslateKitLanguage,
                         OutputFn,
                         std::uintptr_t);

base::FilePath LibraryPath() {
  base::FilePath dir;
  CHECK(base::PathService::Get(base::DIR_MODULE, &dir));
#if BUILDFLAG(IS_MAC)
  return dir.Append(FILE_PATH_LITERAL("libcompass_translatekit.dylib"));
#elif BUILDFLAG(IS_WIN)
  return dir.Append(FILE_PATH_LITERAL("compass_translatekit.dll"));
#else
  return dir.Append(FILE_PATH_LITERAL("libcompass_translatekit.so"));
#endif
}

// Virtual package paths ("/0/de-en/model.bin") to real files, as the
// service's FileOperationProxy maps them.
class Files {
 public:
  void Add(std::string virtual_path, base::FilePath real) {
    files_[std::move(virtual_path)] = std::move(real);
  }
  std::map<std::string, base::FilePath> files_;
};

Files* g_files = nullptr;

bool FileExists(const char* name,
                size_t size,
                bool* is_directory,
                std::uintptr_t) {
  *is_directory = false;
  return g_files->files_.count(std::string(name, size)) > 0;
}

std::uintptr_t Open(const char* name, size_t size, std::uintptr_t) {
  auto it = g_files->files_.find(std::string(name, size));
  if (it == g_files->files_.end()) {
    return 0;
  }
  auto mapped = std::make_unique<base::MemoryMappedFile>();
  if (!mapped->Initialize(it->second)) {
    return 0;
  }
  return reinterpret_cast<std::uintptr_t>(mapped.release());
}

void Close(std::uintptr_t region, std::uintptr_t) {
  delete reinterpret_cast<base::MemoryMappedFile*>(region);
}

const void* Data(std::uintptr_t region, std::uintptr_t) {
  return reinterpret_cast<base::MemoryMappedFile*>(region)->bytes().data();
}

uint64_t Length(std::uintptr_t region, std::uintptr_t) {
  return reinterpret_cast<base::MemoryMappedFile*>(region)->bytes().size();
}

void Collect(TranslateKitOutputText text, std::uintptr_t user_data) {
  reinterpret_cast<std::string*>(user_data)->append(text.buffer,
                                                    text.buffer_size);
}

void CollectLines(TranslateKitOutputText text, std::uintptr_t user_data) {
  auto* lines = reinterpret_cast<std::string*>(user_data);
  lines->append(text.buffer, text.buffer_size);
  lines->append("|");
}

class TranslateKitAbiTest : public testing::Test {
 protected:
  void SetUp() override {
    library_ = std::make_unique<base::ScopedNativeLibrary>(LibraryPath());
    ASSERT_TRUE(library_->is_valid()) << library_->GetError()->ToString();
    get_version_ = Get<GetVersionFn>("GetTranslateKitVersion");
    init_storage_ = Get<InitStorageFn>("InitializeStorageBackend");
    create_kit_ = Get<CreateKitFn>("CreateTranslateKit");
    delete_kit_ = Get<DeleteFn>("DeleteTranslateKit");
    set_packages_ = Get<SetPackagesFn>("TranslateKitSetLanguagePackages");
    create_translator_ =
        Get<CreateTranslatorFn>("TranslateKitCreateTranslator");
    delete_translator_ = Get<DeleteFn>("DeleteTranslator");
    translate_ = Get<TranslateFn>("TranslatorTranslate");
    split_ = Get<SplitFn>("TranslateKitSplitSentences");
    g_files = &files_;
  }

  void TearDown() override { g_files = nullptr; }

  template <typename T>
  T Get(const char* name) {
    return reinterpret_cast<T>(library_->GetFunctionPointer(name));
  }

  // A kit with `packages` (language1, language2) at /0, /1, ...
  std::uintptr_t MakeKit(
      std::initializer_list<std::pair<std::string, std::string>> packages) {
    init_storage_(&FileExists, &Open, &Close, &Data, &Length, 0);
    std::uintptr_t kit = create_kit_();
    chrome::on_device_translation::TranslateKitLanguagePackageConfig config;
    int index = 0;
    for (const auto& [language1, language2] : packages) {
      auto* package = config.add_packages();
      package->set_language1(language1);
      package->set_language2(language2);
      package->set_package_path("/" + base::NumberToString(index++));
    }
    std::string bytes = config.SerializeAsString();
    EXPECT_TRUE(set_packages_(kit, {bytes.data(), bytes.size()}));
    return kit;
  }

  std::unique_ptr<base::ScopedNativeLibrary> library_;
  Files files_;
  GetVersionFn get_version_;
  InitStorageFn init_storage_;
  CreateKitFn create_kit_;
  DeleteFn delete_kit_;
  SetPackagesFn set_packages_;
  CreateTranslatorFn create_translator_;
  DeleteFn delete_translator_;
  TranslateFn translate_;
  SplitFn split_;
};

TEST_F(TranslateKitAbiTest, ExportsWhatTheServiceLoads) {
  EXPECT_TRUE(get_version_);
  EXPECT_TRUE(init_storage_);
  EXPECT_TRUE(create_kit_);
  EXPECT_TRUE(delete_kit_);
  EXPECT_TRUE(set_packages_);
  EXPECT_TRUE(create_translator_);
  EXPECT_TRUE(delete_translator_);
  EXPECT_TRUE(translate_);
  EXPECT_TRUE(split_);
}

TEST_F(TranslateKitAbiTest, VersionIsOneTheServiceAccepts) {
  // As TranslateKitClient asks for it.
  std::string buffer(
      on_device_translation::kTranslationAPILibraryVersionStringSize, '\0');
  TranslateKitVersion version{buffer.data(), buffer.size()};
  ASSERT_TRUE(get_version_(&version));
  std::string_view text(version.buffer, version.buffer_size);
  EXPECT_TRUE(on_device_translation::IsValidTranslateKitVersion(text)) << text;

  char small[4];
  TranslateKitVersion too_small{small, sizeof(small)};
  EXPECT_FALSE(get_version_(&too_small));
}

TEST_F(TranslateKitAbiTest, NoTranslatorWithoutModels) {
  std::uintptr_t kit = MakeKit({{"de", "en"}});
  EXPECT_EQ(create_translator_(kit, {"de", 2}, {"en", 2}), 0u);

  // A model that isn't one: Marian aborts, the library survives.
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  base::FilePath junk = dir.GetPath().AppendASCII("junk");
  ASSERT_TRUE(base::WriteFile(junk, "not a model"));
  files_.Add("/0/de-en/model.bin", junk);
  files_.Add("/0/de-en/vocab.spm", junk);
  EXPECT_EQ(create_translator_(kit, {"de", 2}, {"en", 2}), 0u);

  std::string garbage = "\x01\x02 not a proto";
  EXPECT_FALSE(set_packages_(kit, {garbage.data(), garbage.size()}));
  delete_kit_(kit);
}

TEST_F(TranslateKitAbiTest, SplitsSentences) {
  std::string text = "Hello there. How are you? Fine!";
  std::string lines;
  ASSERT_TRUE(split_({text.data(), text.size()}, {"en", 2}, &CollectLines,
                     reinterpret_cast<std::uintptr_t>(&lines)));
  EXPECT_EQ(lines, "Hello there.|How are you?|Fine!|");
}

// With real models, as engine_unittest.cc's: COMPASS_TRANSLATEKIT_TEST_PACKAGES
// holds "de-en/de-en/{model.bin,vocab.spm}".
TEST_F(TranslateKitAbiTest, TranslatesWithRealModels) {
  std::optional<std::string> root =
      base::Environment::Create()->GetVar("COMPASS_TRANSLATEKIT_TEST_PACKAGES");
  if (!root || root->empty()) {
    GTEST_SKIP() << "COMPASS_TRANSLATEKIT_TEST_PACKAGES isn't set";
  }
  base::FilePath direction =
      base::FilePath::FromUTF8Unsafe(*root).AppendASCII("de-en/de-en");
  files_.Add("/0/de-en/model.bin", direction.AppendASCII("model.bin"));
  files_.Add("/0/de-en/vocab.spm", direction.AppendASCII("vocab.spm"));
  std::uintptr_t kit = MakeKit({{"en", "de"}});
  std::uintptr_t translator = create_translator_(kit, {"de", 2}, {"en", 2});
  ASSERT_NE(translator, 0u);

  std::string text = "Guten Morgen.";
  std::string out;
  ASSERT_TRUE(translate_(translator, {text.data(), text.size()}, &Collect,
                         reinterpret_cast<std::uintptr_t>(&out)));
  EXPECT_EQ(out, "Good morning.");

  std::string html =
      std::string(kHtmlMarker) + "Klicken Sie <a href=\"/x\">hier</a>.";
  out.clear();
  ASSERT_TRUE(translate_(translator, {html.data(), html.size()}, &Collect,
                         reinterpret_cast<std::uintptr_t>(&out)));
  EXPECT_NE(out.find("<a href=\"/x\">"), std::string::npos) << out;
  EXPECT_EQ(out.find(kHtmlMarker), std::string::npos) << out;

  delete_translator_(translator);
  delete_kit_(kit);
}

}  // namespace
}  // namespace compass::translatekit
