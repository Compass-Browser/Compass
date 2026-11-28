// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/translatekit/ios/bergamot_kit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compass/components/translatekit/disk_file_source.h"
#include "compass/components/translatekit/engine.h"
#include "third_party/zstd/src/lib/zstd.h"

struct BKEngine {
  explicit BKEngine(size_t workers)
      : engine(std::make_unique<compass::translatekit::DiskFileSource>(),
               workers) {}
  compass::translatekit::Engine engine;
};

struct BKTranslator {
  std::unique_ptr<compass::translatekit::Translator> translator;
};

BKEngine* bk_engine_create(size_t workers) {
  try {
    return new BKEngine(workers);
  } catch (...) {
    return nullptr;
  }
}

void bk_engine_destroy(BKEngine* engine) {
  delete engine;
}

void bk_engine_set_packages(BKEngine* engine,
                            const char* const* languages1,
                            const char* const* languages2,
                            const char* const* paths,
                            size_t count) {
  if (!engine) {
    return;
  }
  std::vector<compass::translatekit::Package> packages;
  packages.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    packages.push_back({languages1[i], languages2[i], paths[i]});
  }
  engine->engine.SetPackages(std::move(packages));
}

BKTranslator* bk_translator_create(BKEngine* engine,
                                   const char* source,
                                   const char* target) {
  if (!engine || !source || !target) {
    return nullptr;
  }
  try {
    std::unique_ptr<compass::translatekit::Translator> translator =
        engine->engine.CreateTranslator(source, target);
    if (!translator) {
      return nullptr;
    }
    return new BKTranslator{std::move(translator)};
  } catch (...) {
    return nullptr;
  }
}

void bk_translator_destroy(BKTranslator* translator) {
  delete translator;
}

char* bk_translate(BKTranslator* translator,
                   const char* text,
                   size_t length,
                   int html) {
  if (!translator || !text) {
    return nullptr;
  }
  try {
    std::optional<std::string> out = translator->translator->Translate(
        std::string_view(text, length), html != 0);
    if (!out) {
      return nullptr;
    }
    char* copy = static_cast<char*>(std::malloc(out->size() + 1));
    if (!copy) {
      return nullptr;
    }
    std::memcpy(copy, out->data(), out->size());
    copy[out->size()] = '\0';
    return copy;
  } catch (...) {
    return nullptr;
  }
}

void bk_free(char* text) {
  std::free(text);
}

int bk_zstd_decompress_file(const char* source, const char* destination) {
  std::FILE* in = std::fopen(source, "rb");
  if (!in) {
    return 0;
  }
  std::FILE* out = std::fopen(destination, "wb");
  if (!out) {
    std::fclose(in);
    return 0;
  }
  ZSTD_DCtx* context = ZSTD_createDCtx();
  std::vector<char> input(ZSTD_DStreamInSize());
  std::vector<char> output(ZSTD_DStreamOutSize());
  bool ok = context != nullptr;
  size_t last = 0;
  while (ok) {
    size_t read = std::fread(input.data(), 1, input.size(), in);
    if (read == 0) {
      // A frame must have ended.
      ok = !std::ferror(in) && last == 0;
      break;
    }
    ZSTD_inBuffer chunk{input.data(), read, 0};
    while (ok && chunk.pos < chunk.size) {
      ZSTD_outBuffer decompressed{output.data(), output.size(), 0};
      last = ZSTD_decompressStream(context, &decompressed, &chunk);
      ok = !ZSTD_isError(last) &&
           std::fwrite(output.data(), 1, decompressed.pos, out) ==
               decompressed.pos;
    }
  }
  ZSTD_freeDCtx(context);
  std::fclose(in);
  ok = std::fclose(out) == 0 && ok;
  return ok ? 1 : 0;
}
