// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// libcompass_translatekit: Bergamot behind the C ABI Chromium's on-device
// translation service loads (components/on_device_translation/service/
// translate_kit_client.cc). Text starting with kHtmlMarker is translated as
// HTML (html_marker.h). docs/design/translation.md, "The TranslateKit ABI".
//
// Every export catches: an exception must never cross into the service.

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compass/components/translatekit/engine.h"
#include "compass/components/translatekit/html_marker.h"
#include "compass/components/translatekit/translatekit_export.h"
#include "compass/components/translatekit/version.h"
#include "components/on_device_translation/service/proto/translate_kit_api.pb.h"
#include "components/on_device_translation/service/translate_kit_structs.h"
#include "ssplit.h"

namespace {

using compass::translatekit::Engine;
using compass::translatekit::FileSource;
using compass::translatekit::Package;
using compass::translatekit::Translator;

struct StorageBackend {
  FileExistsFn file_exists = nullptr;
  OpenForReadOnlyMemoryMapFn open = nullptr;
  DeleteReadOnlyMemoryRegionFn close = nullptr;
  ReadOnlyMemoryRegionDataFn data = nullptr;
  ReadOnlyMemoryRegionLengthFn length = nullptr;
  std::uintptr_t user_data = 0;
};

StorageBackend& Storage() {
  static StorageBackend storage;
  return storage;
}

// The package files through the service's callbacks: the sandbox lets the
// library open nothing itself.
class CallbackFileSource : public FileSource {
 public:
  bool Exists(const std::string& path) override {
    const StorageBackend& storage = Storage();
    if (!storage.file_exists) {
      return false;
    }
    bool is_directory = false;
    return storage.file_exists(path.data(), path.size(), &is_directory,
                               storage.user_data) &&
           !is_directory;
  }

  std::optional<std::string> Read(const std::string& path) override {
    const StorageBackend& storage = Storage();
    if (!storage.open || !Exists(path)) {
      return std::nullopt;
    }
    std::uintptr_t region =
        storage.open(path.data(), path.size(), storage.user_data);
    if (!region) {
      return std::nullopt;
    }
    const char* data =
        static_cast<const char*>(storage.data(region, storage.user_data));
    uint64_t length = storage.length(region, storage.user_data);
    std::optional<std::string> result;
    if (data || !length) {
      result.emplace(data, static_cast<size_t>(length));
    }
    storage.close(region, storage.user_data);
    return result;
  }
};

typedef void (*OutputCallback)(TranslateKitOutputText, std::uintptr_t);

bool Translate(std::uintptr_t translator_ptr,
               TranslateKitInputText input,
               OutputCallback callback,
               std::uintptr_t user_data) {
  if (!translator_ptr || !callback) {
    return false;
  }
  try {
    std::string_view text(input.input_text, input.input_text_size);
    const bool html = text.starts_with(compass::translatekit::kHtmlMarker);
    if (html) {
      text.remove_prefix(compass::translatekit::kHtmlMarker.size());
    }
    std::optional<std::string> output =
        reinterpret_cast<Translator*>(translator_ptr)->Translate(text, html);
    if (!output) {
      return false;
    }
    callback(TranslateKitOutputText{output->data(), output->size()}, user_data);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

extern "C" {

TRANSLATEKIT_EXPORT bool GetTranslateKitVersion(TranslateKitVersion* version) {
  if (!version || !version->buffer) {
    return false;
  }
  const size_t size = std::strlen(compass::translatekit::kVersion);
  if (version->buffer_size < size) {
    return false;
  }
  std::memcpy(version->buffer, compass::translatekit::kVersion, size);
  version->buffer_size = size;
  return true;
}

TRANSLATEKIT_EXPORT void InitializeStorageBackend(
    FileExistsFn file_exists,
    OpenForReadOnlyMemoryMapFn open_for_read_only_memory_map,
    DeleteReadOnlyMemoryRegionFn delete_read_only_memory_region,
    ReadOnlyMemoryRegionDataFn read_only_memory_region_data,
    ReadOnlyMemoryRegionLengthFn read_only_memory_region_length,
    std::uintptr_t user_data) {
  StorageBackend& storage = Storage();
  storage.file_exists = file_exists;
  storage.open = open_for_read_only_memory_map;
  storage.close = delete_read_only_memory_region;
  storage.data = read_only_memory_region_data;
  storage.length = read_only_memory_region_length;
  storage.user_data = user_data;
}

TRANSLATEKIT_EXPORT std::uintptr_t CreateTranslateKit() {
  try {
    return reinterpret_cast<std::uintptr_t>(
        new Engine(std::make_unique<CallbackFileSource>()));
  } catch (...) {
    return 0;
  }
}

TRANSLATEKIT_EXPORT void DeleteTranslateKit(std::uintptr_t kit) {
  try {
    delete reinterpret_cast<Engine*>(kit);
  } catch (...) {
  }
}

TRANSLATEKIT_EXPORT bool TranslateKitSetLanguagePackages(
    std::uintptr_t kit,
    TranslateKitSetLanguagePackagesArgs args) {
  if (!kit) {
    return false;
  }
  try {
    chrome::on_device_translation::TranslateKitLanguagePackageConfig config;
    if (!config.ParseFromArray(args.package_config,
                               static_cast<int>(args.package_config_size))) {
      return false;
    }
    std::vector<Package> packages;
    for (const auto& package : config.packages()) {
      packages.push_back(
          {package.language1(), package.language2(), package.package_path()});
    }
    reinterpret_cast<Engine*>(kit)->SetPackages(std::move(packages));
    return true;
  } catch (...) {
    return false;
  }
}

TRANSLATEKIT_EXPORT std::uintptr_t TranslateKitCreateTranslator(
    std::uintptr_t kit,
    TranslateKitLanguage source,
    TranslateKitLanguage target) {
  if (!kit) {
    return 0;
  }
  try {
    std::unique_ptr<Translator> translator =
        reinterpret_cast<Engine*>(kit)->CreateTranslator(
            std::string_view(source.language_code, source.language_code_size),
            std::string_view(target.language_code, target.language_code_size));
    return reinterpret_cast<std::uintptr_t>(translator.release());
  } catch (...) {
    return 0;
  }
}

TRANSLATEKIT_EXPORT void DeleteTranslator(std::uintptr_t translator) {
  try {
    delete reinterpret_cast<Translator*>(translator);
  } catch (...) {
  }
}

TRANSLATEKIT_EXPORT bool TranslatorTranslate(std::uintptr_t translator,
                                             TranslateKitInputText input,
                                             OutputCallback callback,
                                             std::uintptr_t user_data) {
  return Translate(translator, input, callback, user_data);
}

TRANSLATEKIT_EXPORT bool TranslateKitSplitSentences(
    TranslateKitInputText input,
    TranslateKitLanguage /*language*/,
    OutputCallback callback,
    std::uintptr_t user_data) {
  if (!callback) {
    return false;
  }
  try {
    // Bergamot's splitter without a language's prefix list: rules only.
    static const ug::ssplit::SentenceSplitter splitter;
    ug::ssplit::SentenceStream sentences(
        input.input_text, input.input_text_size, splitter,
        ug::ssplit::SentenceStream::splitmode::wrapped_text);
    std::string_view sentence;
    while (sentences >> sentence) {
      callback(TranslateKitOutputText{sentence.data(), sentence.size()},
               user_data);
    }
    return true;
  } catch (...) {
    return false;
  }
}

}  // extern "C"
