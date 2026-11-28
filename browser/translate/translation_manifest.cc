// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/translate/translation_manifest.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "compass/browser/translate/models_json.h"

namespace compass::translate {

namespace {

constexpr std::string_view kFileTypes[] = {"model", "vocab", "srcvocab",
                                           "trgvocab"};

bool IsSha256(const std::string& hex) {
  return hex.size() == 64 &&
         std::ranges::all_of(hex, [](char c) { return base::IsHexDigit(c); });
}

bool IsKnownType(std::string_view type) {
  return std::ranges::find(kFileTypes, type) != std::end(kFileTypes);
}

bool IsComplete(const std::vector<ModelFile>& files) {
  auto has = [&](std::string_view type) {
    return std::ranges::any_of(
        files, [&](const ModelFile& f) { return f.type == type; });
  };
  return has("model") && (has("vocab") || (has("srcvocab") && has("trgvocab")));
}

// Names and paths come from Mozilla: keep them to what a CDN path holds.
bool IsSafeLocation(const std::string& location) {
  return !location.empty() && location.front() != '/' &&
         location.find("..") == std::string::npos &&
         location.find('\\') == std::string::npos &&
         base::IsStringASCII(location);
}

std::optional<ModelFile> ParseFile(const base::DictValue& dict) {
  ModelFile file;
  const std::string* type = dict.FindString("type");
  const std::string* location = dict.FindString("location");
  const std::string* hash = dict.FindString("hash");
  const std::string* decompressed_hash = dict.FindString("decompressed_hash");
  std::optional<double> size = dict.FindDouble("size");
  std::optional<double> decompressed_size =
      dict.FindDouble("decompressed_size");
  if (!type || !location || !hash || !decompressed_hash || !size ||
      !decompressed_size || !IsKnownType(*type) || !IsSafeLocation(*location) ||
      !IsSha256(*hash) || !IsSha256(*decompressed_hash) || *size <= 0 ||
      *decompressed_size <= 0) {
    return std::nullopt;
  }
  file.type = *type;
  file.location = *location;
  file.hash = *hash;
  file.decompressed_hash = *decompressed_hash;
  file.size = static_cast<int64_t>(*size);
  file.decompressed_size = static_cast<int64_t>(*decompressed_size);
  return file;
}

}  // namespace

Manifest::Manifest() = default;
Manifest::Manifest(const Manifest&) = default;
Manifest& Manifest::operator=(const Manifest&) = default;
Manifest::~Manifest() = default;

const ModelDirection* Manifest::Find(std::string_view from,
                                     std::string_view to) const {
  auto it = directions.find(base::StrCat({from, "-", to}));
  return it == directions.end() ? nullptr : &it->second;
}

std::optional<Manifest> ParseManifest(std::string_view json) {
  std::optional<base::Value> value =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  const base::DictValue& dict = value->GetDict();
  const std::string* attachments = dict.FindString("attachments");
  const base::DictValue* directions = dict.FindDict("directions");
  if (!attachments || !directions ||
      !base::StartsWith(*attachments, "https://") ||
      !base::EndsWith(*attachments, "/")) {
    return std::nullopt;
  }
  Manifest manifest;
  manifest.attachments = *attachments;
  for (const auto [name, entry] : *directions) {
    if (!entry.is_dict()) {
      return std::nullopt;
    }
    ModelDirection direction;
    const std::string* version = entry.GetDict().FindString("version");
    const std::string* architecture =
        entry.GetDict().FindString("architecture");
    const base::ListValue* files = entry.GetDict().FindList("files");
    if (!version || !architecture || !files) {
      return std::nullopt;
    }
    direction.version = *version;
    direction.architecture = *architecture;
    for (const base::Value& file : *files) {
      if (!file.is_dict()) {
        return std::nullopt;
      }
      std::optional<ModelFile> parsed = ParseFile(file.GetDict());
      if (!parsed) {
        return std::nullopt;
      }
      direction.files.push_back(std::move(*parsed));
    }
    if (!IsComplete(direction.files)) {
      return std::nullopt;
    }
    manifest.directions.emplace(name, std::move(direction));
  }
  return manifest;
}

const Manifest& BuiltInManifest() {
  static const base::NoDestructor<Manifest> kManifest([] {
    std::optional<Manifest> manifest = ParseManifest(kModelsJson);
    CHECK(manifest) << "browser/translate/models.json doesn't parse";
    return std::move(*manifest);
  }());
  return *kManifest;
}

std::string ToModelLanguage(std::string_view upstream_code) {
  if (upstream_code == "iw") {
    return "he";
  }
  if (upstream_code == "no") {
    return "nb";
  }
  if (upstream_code == "zh") {
    return "zh-Hans";
  }
  return std::string(upstream_code);
}

}  // namespace compass::translate
