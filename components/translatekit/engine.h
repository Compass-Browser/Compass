// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_TRANSLATEKIT_ENGINE_H_
#define COMPASS_COMPONENTS_TRANSLATEKIT_ENGINE_H_

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Bergamot behind Chromium's TranslateKit ABI (translatekit.cc), without the
// ABI: packages, models, translators. No //base, so the library stays small
// and its own. docs/design/translation.md, "The TranslateKit ABI".

namespace marian::bergamot {
class AsyncService;
class TranslationModel;
}  // namespace marian::bergamot

namespace compass::translatekit {

// Inside a language package, one folder per direction it holds, named
// "<from>-<to>" in the package's own language codes, with:
inline constexpr char kModelFile[] = "model.bin";
// Either one vocabulary for both sides...
inline constexpr char kVocabFile[] = "vocab.spm";
// ...or one per side.
inline constexpr char kSourceVocabFile[] = "srcvocab.spm";
inline constexpr char kTargetVocabFile[] = "trgvocab.spm";

// Every model is to or from English; other pairs go through it.
inline constexpr char kPivotLanguage[] = "en";

// Reads the files of the packages. In the service, the storage callbacks
// Chromium gives the library; in tests, the disk.
class FileSource {
 public:
  virtual ~FileSource() = default;
  virtual bool Exists(const std::string& path) = 0;
  // The whole file, or nullopt.
  virtual std::optional<std::string> Read(const std::string& path) = 0;
};

struct Package {
  std::string language1;
  std::string language2;
  std::string path;  // No trailing separator.
};

class Translator {
 public:
  ~Translator();
  Translator(const Translator&) = delete;
  Translator& operator=(const Translator&) = delete;

  // `html`: `text` is HTML; tags are kept and moved with the words they
  // belong to. Nullopt on failure.
  std::optional<std::string> Translate(std::string_view text, bool html);

 private:
  friend class Engine;
  Translator(std::shared_ptr<marian::bergamot::AsyncService> service,
             std::shared_ptr<marian::bergamot::TranslationModel> first,
             std::shared_ptr<marian::bergamot::TranslationModel> second);

  // Shared with the engine, so a translator outliving it still works.
  std::shared_ptr<marian::bergamot::AsyncService> service_;
  std::shared_ptr<marian::bergamot::TranslationModel> first_;
  // Set when pivoting: first_ translates into English, second_ out of it.
  std::shared_ptr<marian::bergamot::TranslationModel> second_;
};

class Engine {
 public:
  // `workers`: translation threads; 0 picks from the machine's cores.
  Engine(std::unique_ptr<FileSource> files, size_t workers = 0);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void SetPackages(std::vector<Package> packages);

  // Null when no installed package translates `source` into `target`
  // (directly, or through English), or a model doesn't load.
  std::unique_ptr<Translator> CreateTranslator(std::string_view source,
                                               std::string_view target);

  // What workers=0 picks on this machine.
  static size_t DefaultWorkers();

  // The folder of the direction, or nullopt. Exposed for tests.
  std::optional<std::string> FindDirection(std::string_view source,
                                           std::string_view target);

 private:
  std::shared_ptr<marian::bergamot::TranslationModel> LoadModel(
      const std::string& source,
      const std::string& target);
  std::shared_ptr<marian::bergamot::AsyncService> service();

  std::unique_ptr<FileSource> files_;
  size_t workers_;
  std::mutex mutex_;
  std::vector<Package> packages_;
  // Loaded models by "<from>-<to>", shared by the translators using them.
  std::map<std::string, std::weak_ptr<marian::bergamot::TranslationModel>>
      models_;
  std::shared_ptr<marian::bergamot::AsyncService> service_;
};

// Lowercase, "_" as "-": what packages and requests are compared by.
std::string NormalizeLanguage(std::string_view code);

}  // namespace compass::translatekit

#endif  // COMPASS_COMPONENTS_TRANSLATEKIT_ENGINE_H_
