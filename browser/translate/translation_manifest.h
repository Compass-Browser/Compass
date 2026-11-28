// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_TRANSLATE_TRANSLATION_MANIFEST_H_
#define COMPASS_BROWSER_TRANSLATE_TRANSLATION_MANIFEST_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Which of Firefox's translation models Compass downloads, and how to check
// them (docs/design/translation.md, "Language models").
// tools/translation/update_models.py writes browser/translate/models.json,
// which is built in.

namespace compass::translate {

struct ModelFile {
  // "model", "vocab", "srcvocab" or "trgvocab".
  std::string type;
  // Under the manifest's attachments URL.
  std::string location;
  int64_t size = 0;
  // SHA-256 of the zstd file, then of its contents, lowercase hex.
  std::string hash;
  int64_t decompressed_size = 0;
  std::string decompressed_hash;

  bool operator==(const ModelFile&) const = default;
};

struct ModelDirection {
  std::string version;
  std::string architecture;
  std::vector<ModelFile> files;
};

struct Manifest {
  Manifest();
  Manifest(const Manifest&);
  Manifest& operator=(const Manifest&);
  ~Manifest();

  // Where `location`s are, ending in "/".
  std::string attachments;
  // By "<from>-<to>" in Mozilla's codes ("zh-Hans", "he", "nb").
  std::map<std::string, ModelDirection> directions;

  const ModelDirection* Find(std::string_view from, std::string_view to) const;
};

// Parses models.json; nullopt when it isn't one.
std::optional<Manifest> ParseManifest(std::string_view json);

// The manifest built into this Compass.
const Manifest& BuiltInManifest();

// Upstream's language codes (components/on_device_translation's
// SupportedLanguage: "iw", "no", "zh") as Mozilla's ("he", "nb", "zh-Hans").
std::string ToModelLanguage(std::string_view upstream_code);

}  // namespace compass::translate

#endif  // COMPASS_BROWSER_TRANSLATE_TRANSLATION_MANIFEST_H_
