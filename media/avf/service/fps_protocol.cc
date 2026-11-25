// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/fps_protocol.h"

#include <algorithm>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "base/memory/raw_ptr_exclusion.h"

namespace compass::avf {
namespace {

constexpr uint32_t kPssh = 0x70737368;  // 'pssh'
constexpr uint32_t kKreq = 0x6B726571;  // 'kreq'
constexpr uint32_t kOkrk = 0x6F6B726B;  // 'okrk'
constexpr uint32_t kSpcs = 0x73706373;  // 'spcs'
constexpr uint32_t kCkcs = 0x636B6373;  // 'ckcs'
constexpr uint32_t kOkcs = 0x6F6B6373;  // 'okcs'
// FairPlay's system ID, 94CE86FB-07FF-4F43-ADB8-93D2FA968CA2.
constexpr uint8_t kFairPlaySystemId[16] = {0x94, 0xCE, 0x86, 0xFB, 0x07, 0xFF,
                                           0x4F, 0x43, 0xAD, 0xB8, 0x93, 0xD2,
                                           0xFA, 0x96, 0x8C, 0xA2};

class Reader {
 public:
  explicit Reader(base::span<const uint8_t> data) : data_(data) {}

  bool U32(uint32_t* value) {
    if (offset_ + 4 > data_.size()) {
      return false;
    }
    *value = (static_cast<uint32_t>(data_[offset_]) << 24) |
             (static_cast<uint32_t>(data_[offset_ + 1]) << 16) |
             (static_cast<uint32_t>(data_[offset_ + 2]) << 8) |
             data_[offset_ + 3];
    offset_ += 4;
    return true;
  }

  bool Bytes(size_t size, std::vector<uint8_t>* out) {
    if (offset_ + size > data_.size()) {
      return false;
    }
    auto bytes = data_.subspan(offset_, size);
    out->assign(bytes.begin(), bytes.end());
    offset_ += size;
    return true;
  }

  bool Sized(std::vector<uint8_t>* out) {
    uint32_t size;
    return U32(&size) && Bytes(size, out);
  }

  bool KeyId(std::string* out) {
    std::vector<uint8_t> bytes;
    if (!Bytes(16, &bytes)) {
      return false;
    }
    out->assign(bytes.begin(), bytes.end());
    return true;
  }

  size_t offset() const { return offset_; }
  size_t size() const { return data_.size(); }

 private:
  // Stack-only, over a buffer that outlives it.
  RAW_PTR_EXCLUSION base::span<const uint8_t> data_;
  size_t offset_ = 0;
};

std::optional<std::vector<FpsKeyRequest>> ReadKeyList(Reader& reader) {
  uint32_t count;
  if (!reader.U32(&count) || count > 64) {
    return std::nullopt;
  }
  std::vector<FpsKeyRequest> keys;
  for (uint32_t i = 0; i < count; ++i) {
    FpsKeyRequest key;
    std::vector<uint8_t> ssc;
    uint32_t versions;
    if (!reader.KeyId(&key.key_id) || !reader.Sized(&key.asset_id) ||
        !reader.Sized(&ssc) || !reader.U32(&versions) || versions > 64) {
      return std::nullopt;
    }
    for (uint32_t v = 0; v < versions; ++v) {
      uint32_t version;
      if (!reader.U32(&version)) {
        return std::nullopt;
      }
      key.versions.push_back(version);
    }
    keys.push_back(std::move(key));
  }
  return keys;
}

void WriteU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(value >> 24);
  out.push_back((value >> 16) & 0xFF);
  out.push_back((value >> 8) & 0xFF);
  out.push_back(value & 0xFF);
}

}  // namespace

namespace {

constexpr uint32_t kFpsd = 0x66707364;  // 'fpsd'
constexpr uint32_t kFpsk = 0x6670736B;  // 'fpsk'
constexpr uint32_t kFkri = 0x666B7269;  // 'fkri'
constexpr uint32_t kFkai = 0x666B6169;  // 'fkai'
constexpr uint32_t kFkvl = 0x666B766C;  // 'fkvl'

// Child boxes of `data`: (type, payload).
std::optional<std::vector<std::pair<uint32_t, std::vector<uint8_t>>>> Boxes(
    base::span<const uint8_t> data) {
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> boxes;
  Reader reader(data);
  while (reader.offset() < reader.size()) {
    uint32_t size, type;
    std::vector<uint8_t> payload;
    if (!reader.U32(&size) || !reader.U32(&type) || size < 8 ||
        !reader.Bytes(size - 8, &payload)) {
      return std::nullopt;
    }
    boxes.emplace_back(type, std::move(payload));
  }
  return boxes;
}

// Safari's 'fpsd' box.
std::optional<std::vector<FpsKeyRequest>> ParseFpsd(
    const std::vector<uint8_t>& fpsd) {
  auto children = Boxes(fpsd);
  if (!children) {
    return std::nullopt;
  }
  std::vector<FpsKeyRequest> keys;
  for (const auto& [type, payload] : *children) {
    if (type != kFpsk) {
      continue;  // 'fpsi': version and scheme.
    }
    auto parts = Boxes(payload);
    if (!parts) {
      return std::nullopt;
    }
    FpsKeyRequest key;
    for (const auto& [part_type, part] : *parts) {
      if (part_type == kFkri && part.size() >= 20) {
        key.key_id.assign(part.begin() + 4, part.begin() + 20);
      } else if (part_type == kFkai) {
        key.asset_id = part;
      } else if (part_type == kFkvl) {
        for (size_t i = 0; i + 4 <= part.size(); i += 4) {
          key.versions.push_back((static_cast<uint32_t>(part[i]) << 24) |
                                 (static_cast<uint32_t>(part[i + 1]) << 16) |
                                 (static_cast<uint32_t>(part[i + 2]) << 8) |
                                 part[i + 3]);
        }
      }
    }
    if (key.key_id.size() != 16) {
      return std::nullopt;
    }
    keys.push_back(std::move(key));
  }
  return keys;
}

}  // namespace

std::optional<FpsInitData> ParseFpsInitData(
    const std::vector<uint8_t>& init_data) {
  Reader boxes(init_data);
  while (boxes.offset() + 8 <= boxes.size()) {
    size_t start = boxes.offset();
    uint32_t size, type;
    if (!boxes.U32(&size) || !boxes.U32(&type) || size < 32 ||
        start + size > init_data.size()) {
      return std::nullopt;
    }
    std::vector<uint8_t> box;
    if (!boxes.Bytes(size - 8, &box)) {
      return std::nullopt;
    }
    // version + flags (4), system ID (16), [key IDs], data size (4), data.
    if (type != kPssh ||
        !std::equal(box.begin() + 4, box.begin() + 20, kFairPlaySystemId)) {
      continue;
    }
    Reader pssh(base::span(box).subspan(20u));
    if (box[0] > 0) {
      uint32_t kid_count;
      std::vector<uint8_t> ignored;
      if (!pssh.U32(&kid_count) || !pssh.Bytes(16ull * kid_count, &ignored)) {
        return std::nullopt;
      }
    }
    std::vector<uint8_t> data;
    if (!pssh.Sized(&data)) {
      return std::nullopt;
    }
    // Safari's: one 'fpsd' box.
    if (data.size() >= 8 && data[4] == 'f' && data[5] == 'p' &&
        data[6] == 's' && data[7] == 'd') {
      auto fpsd = Boxes(data);
      if (!fpsd || fpsd->empty() || fpsd->front().first != kFpsd) {
        return std::nullopt;
      }
      std::optional<std::vector<FpsKeyRequest>> keys =
          ParseFpsd(fpsd->front().second);
      if (!keys) {
        return std::nullopt;
      }
      return FpsInitData{FpsFormat::kWebKit, std::move(*keys)};
    }
    Reader fps(data);
    uint32_t scheme, version;
    if (!fps.U32(&scheme) || !fps.U32(&version)) {
      return std::nullopt;
    }
    std::optional<std::vector<FpsKeyRequest>> keys = ReadKeyList(fps);
    if (!keys) {
      return std::nullopt;
    }
    return FpsInitData{FpsFormat::kBinary, std::move(*keys)};
  }
  return std::nullopt;
}

std::optional<std::vector<FpsKeyRequest>> ParseFpsKeyRequestMessage(
    const std::vector<uint8_t>& message) {
  Reader reader(message);
  uint32_t tag;
  if (!reader.U32(&tag) || (tag != kKreq && tag != kOkrk)) {
    return std::nullopt;
  }
  return ReadKeyList(reader);
}

std::optional<std::vector<FpsLicense>> ParseFpsLicenseMessage(
    const std::vector<uint8_t>& message) {
  Reader reader(message);
  uint32_t tag, count;
  if (!reader.U32(&tag) || (tag != kCkcs && tag != kOkcs) ||
      !reader.U32(&count) || count > 64) {
    return std::nullopt;
  }
  std::vector<FpsLicense> licenses;
  for (uint32_t i = 0; i < count; ++i) {
    FpsLicense license;
    if (!reader.KeyId(&license.key_id) ||
        !reader.U32(&license.expiry_seconds) || !reader.Sized(&license.ckc)) {
      return std::nullopt;
    }
    licenses.push_back(std::move(license));
  }
  return licenses;
}

std::vector<uint8_t> MakeFpsSpcMessage(const std::string& key_id,
                                       const std::vector<uint8_t>& spc) {
  std::vector<uint8_t> out;
  WriteU32(out, kSpcs);
  WriteU32(out, 1);
  out.insert(out.end(), key_id.begin(), key_id.end());
  WriteU32(out, static_cast<uint32_t>(spc.size()));
  out.insert(out.end(), spc.begin(), spc.end());
  return out;
}

std::vector<uint8_t> MakeFpsJsonMessage(const std::string& key_id,
                                        const std::vector<uint8_t>& payload) {
  base::DictValue entry;
  entry.Set("keyID", base::Base64Encode(base::as_byte_span(key_id)));
  entry.Set("payload", base::Base64Encode(payload));
  base::ListValue list;
  list.Append(std::move(entry));
  std::string json = base::WriteJson(list).value_or("[]");
  return std::vector<uint8_t>(json.begin(), json.end());
}

std::optional<std::vector<FpsLicense>> ParseFpsJsonLicenses(
    const std::vector<uint8_t>& message) {
  std::optional<base::Value> json = base::JSONReader::Read(
      std::string_view(reinterpret_cast<const char*>(message.data()),
                       message.size()),
      base::JSON_PARSE_RFC);
  if (!json || !json->is_list()) {
    return std::nullopt;
  }
  std::vector<FpsLicense> licenses;
  for (const base::Value& entry : json->GetList()) {
    const base::DictValue* dict = entry.GetIfDict();
    const std::string* key_id = dict ? dict->FindString("keyID") : nullptr;
    const std::string* payload = dict ? dict->FindString("payload") : nullptr;
    FpsLicense license;
    std::string decoded_key_id;
    if (!key_id || !payload ||
        !base::Base64Decode(*key_id, &decoded_key_id) ||
        decoded_key_id.size() != 16) {
      return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> ckc = base::Base64Decode(*payload);
    if (!ckc) {
      return std::nullopt;
    }
    license.key_id = std::move(decoded_key_id);
    license.ckc = std::move(*ckc);
    licenses.push_back(std::move(license));
  }
  return licenses;
}

}  // namespace compass::avf
