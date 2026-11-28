// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/translatekit/engine.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <future>
#include <thread>
#include <utility>

#include "translator/definitions.h"
#include "translator/response.h"
#include "translator/response_options.h"
#include "translator/service.h"
#include "translator/translation_model.h"

namespace compass::translatekit {

namespace {

using marian::bergamot::AlignedMemory;
using marian::bergamot::MemoryBundle;
using marian::bergamot::Response;
using marian::bergamot::ResponseOptions;
using marian::bergamot::TranslationModel;

// Firefox's settings (toolkit/components/translations/content/
// translations-engine.worker.js), without the shortlist and quality
// scores, which Firefox doesn't use either. The vocabularies come from
// memory, but Marian still reads a name per side from "vocabs" (only to
// spot factored vocabularies by their extension); without them a bounds-
// checked build stops at an empty vector.
constexpr char kModelConfig[] = R"(
beam-size: 1
normalize: 1.0
word-penalty: 0
max-length-break: 128
mini-batch-words: 1024
workspace: 128
max-length-factor: 2.0
skip-cost: true
cpu-threads: 0
quiet: true
quiet-translation: true
gemm-precision: int8shiftAlphaAll
alignment: soft
vocabs:
  - in-memory-source
  - in-memory-target
)";

// More gave nothing on an M5 Max (docs/design/translation.md, "Spike").
constexpr size_t kMaxWorkers = 4;

std::string Join(const std::string& a, const std::string& b) {
  return a + "/" + b;
}

AlignedMemory ToAligned(const std::string& bytes, size_t alignment) {
  AlignedMemory memory(bytes.size(), alignment);
  std::copy(bytes.begin(), bytes.end(), memory.begin());
  return memory;
}

}  // namespace

std::string NormalizeLanguage(std::string_view code) {
  std::string result(code);
  for (char& c : result) {
    c = c == '_'
            ? '-'
            : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

// Translator

Translator::Translator(std::shared_ptr<marian::bergamot::AsyncService> service,
                       std::shared_ptr<TranslationModel> first,
                       std::shared_ptr<TranslationModel> second)
    : service_(std::move(service)),
      first_(std::move(first)),
      second_(std::move(second)) {}

Translator::~Translator() = default;

std::optional<std::string> Translator::Translate(std::string_view text,
                                                 bool html) {
  if (text.empty()) {
    return std::string();
  }
  try {
    ResponseOptions options;
    options.HTML = html;
    // Needed to put tags back where their words went.
    options.alignment = html;
    std::promise<Response> promise;
    std::future<Response> future = promise.get_future();
    auto done = [&promise](Response&& response) {
      promise.set_value(std::move(response));
    };
    if (second_) {
      service_->pivot(first_, second_, std::string(text), done, options);
    } else {
      service_->translate(first_, std::string(text), done, options);
    }
    return future.get().target.text;
  } catch (const std::exception&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

// Engine

Engine::Engine(std::unique_ptr<FileSource> files, size_t workers)
    : files_(std::move(files)), workers_(workers ? workers : DefaultWorkers()) {
  marian::setThrowExceptionOnAbort(true);
}

Engine::~Engine() {
  if (service_) {
    service_->clear();
  }
}

// static
size_t Engine::DefaultWorkers() {
  size_t cores = std::thread::hardware_concurrency();
  // Leave a core to the rest of the browser.
  return std::clamp<size_t>(cores > 1 ? cores - 1 : 1, 1, kMaxWorkers);
}

void Engine::SetPackages(std::vector<Package> packages) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Package& package : packages) {
    package.language1 = NormalizeLanguage(package.language1);
    package.language2 = NormalizeLanguage(package.language2);
    while (package.path.size() > 1 &&
           (package.path.back() == '/' || package.path.back() == '\\')) {
      package.path.pop_back();
    }
  }
  packages_ = std::move(packages);
}

std::optional<std::string> Engine::FindDirection(std::string_view source,
                                                 std::string_view target) {
  const std::string from = NormalizeLanguage(source);
  const std::string to = NormalizeLanguage(target);
  std::vector<Package> packages;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    packages = packages_;
  }
  for (const Package& package : packages) {
    bool pair = (package.language1 == from && package.language2 == to) ||
                (package.language1 == to && package.language2 == from);
    if (!pair) {
      continue;
    }
    const std::string folder = Join(package.path, from + "-" + to);
    if (files_->Exists(Join(folder, kModelFile))) {
      return folder;
    }
  }
  return std::nullopt;
}

std::shared_ptr<TranslationModel> Engine::LoadModel(const std::string& source,
                                                    const std::string& target) {
  const std::string key = source + "-" + target;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = models_.find(key); it != models_.end()) {
      if (auto model = it->second.lock()) {
        return model;
      }
    }
  }
  std::optional<std::string> folder = FindDirection(source, target);
  if (!folder) {
    return nullptr;
  }
  std::optional<std::string> model = files_->Read(Join(*folder, kModelFile));
  if (!model || model->empty()) {
    return nullptr;
  }
  MemoryBundle memory;
  // Marian's int8 paths need the model aligned to 256 bytes.
  memory.models.push_back(ToAligned(*model, 256));
  model.reset();
  if (std::optional<std::string> vocab =
          files_->Read(Join(*folder, kVocabFile))) {
    auto shared = std::make_shared<AlignedMemory>(ToAligned(*vocab, 64));
    memory.vocabs = {shared, shared};
  } else {
    std::optional<std::string> source_vocab =
        files_->Read(Join(*folder, kSourceVocabFile));
    std::optional<std::string> target_vocab =
        files_->Read(Join(*folder, kTargetVocabFile));
    if (!source_vocab || !target_vocab) {
      return nullptr;
    }
    memory.vocabs = {
        std::make_shared<AlignedMemory>(ToAligned(*source_vocab, 64)),
        std::make_shared<AlignedMemory>(ToAligned(*target_vocab, 64))};
  }
  std::shared_ptr<TranslationModel> loaded;
  try {
    loaded = std::make_shared<TranslationModel>(std::string(kModelConfig),
                                                std::move(memory), workers_);
  } catch (...) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  models_[key] = loaded;
  return loaded;
}

std::unique_ptr<Translator> Engine::CreateTranslator(std::string_view source,
                                                     std::string_view target) {
  const std::string from = NormalizeLanguage(source);
  const std::string to = NormalizeLanguage(target);
  if (from.empty() || to.empty() || from == to) {
    return nullptr;
  }
  if (FindDirection(from, to)) {
    auto model = LoadModel(from, to);
    if (!model) {
      return nullptr;
    }
    return std::unique_ptr<Translator>(
        new Translator(service(), model, nullptr));
  }
  if (from == kPivotLanguage || to == kPivotLanguage ||
      !FindDirection(from, kPivotLanguage) ||
      !FindDirection(kPivotLanguage, to)) {
    return nullptr;
  }
  auto first = LoadModel(from, kPivotLanguage);
  auto second = first ? LoadModel(kPivotLanguage, to) : nullptr;
  if (!second) {
    return nullptr;
  }
  return std::unique_ptr<Translator>(new Translator(service(), first, second));
}

std::shared_ptr<marian::bergamot::AsyncService> Engine::service() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!service_) {
    marian::bergamot::AsyncService::Config config;
    config.numWorkers = workers_;
    // Bergamot's per-sentence cache; a page repeats itself (menus, footers).
    config.cacheSize = 2000;
    service_ = std::make_shared<marian::bergamot::AsyncService>(config);
  }
  return service_;
}

}  // namespace compass::translatekit
