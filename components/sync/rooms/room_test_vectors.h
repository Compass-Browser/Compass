// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_TEST_VECTORS_H_
#define COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_TEST_VECTORS_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

// common/rooms/vectors.json, which the iOS tests read too.
namespace compass::sync::rooms::test {

inline base::DictValue LoadVectors() {
  base::FilePath root;
  EXPECT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
  std::string contents;
  EXPECT_TRUE(base::ReadFileToString(
      root.AppendASCII("compass/common/rooms/vectors.json"), &contents));
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  EXPECT_TRUE(dict);
  return dict ? std::move(*dict) : base::DictValue();
}

// The dictionaries in vectors[key]; fails the test if there are none.
inline std::vector<base::DictValue> Section(std::string_view key) {
  const base::DictValue vectors = LoadVectors();
  std::vector<base::DictValue> out;
  if (const base::ListValue* list = vectors.FindList(key)) {
    for (const base::Value& item : *list) {
      if (item.is_dict()) {
        out.push_back(item.GetDict().Clone());
      }
    }
  }
  EXPECT_FALSE(out.empty()) << key;
  return out;
}

inline std::string Str(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  EXPECT_TRUE(value) << key;
  return value ? *value : std::string();
}

inline std::vector<uint8_t> Bytes(const base::DictValue& dict,
                                  std::string_view key) {
  const std::string hex = Str(dict, key);
  std::vector<uint8_t> out;
  // HexStringToBytes() refuses "", which is a valid empty value here.
  EXPECT_TRUE(hex.empty() || base::HexStringToBytes(hex, &out)) << key;
  return out;
}

template <size_t N>
std::array<uint8_t, N> Array(const base::DictValue& dict,
                             std::string_view key) {
  const std::vector<uint8_t> bytes = Bytes(dict, key);
  std::array<uint8_t, N> out{};
  EXPECT_EQ(bytes.size(), N) << key;
  std::copy_n(bytes.begin(), std::min(N, bytes.size()), out.begin());
  return out;
}

inline uint32_t Epoch(const base::DictValue& dict) {
  // Epochs past INT_MAX come back as doubles.
  const base::Value* value = dict.Find("epoch");
  EXPECT_TRUE(value && (value->is_int() || value->is_double()));
  if (!value) {
    return 0;
  }
  return value->is_int() ? static_cast<uint32_t>(value->GetInt())
                         : static_cast<uint32_t>(value->GetDouble());
}

}  // namespace compass::sync::rooms::test

#endif  // COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_TEST_VECTORS_H_
