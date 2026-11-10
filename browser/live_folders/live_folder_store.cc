// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_folder_store.h"

#include <utility>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace compass {

namespace {

constexpr int kVersion = 1;
constexpr base::TimeDelta kCommitInterval = base::Seconds(1);

std::optional<std::string> ReadFile(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return std::nullopt;
  }
  return contents;
}

}  // namespace

LiveFolderStore::LiveFolderStore(const base::FilePath& path)
    : file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      writer_(path, file_task_runner_, kCommitInterval, "CompassLiveFolders") {
}

LiveFolderStore::~LiveFolderStore() {
  Flush();
}

void LiveFolderStore::Load(scoped_refptr<os_crypt_async::Encryptor> encryptor,
                           base::OnceClosure done) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (loaded_) {
    std::move(done).Run();
    return;
  }
  when_loaded_.push_back(std::move(done));
  if (loading_) {
    return;
  }
  loading_ = true;
  encryptor_ = std::move(encryptor);
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadFile, writer_.path()),
      base::BindOnce(&LiveFolderStore::OnRead, weak_factory_.GetWeakPtr()));
}

void LiveFolderStore::OnRead(std::optional<std::string> contents) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::optional<base::Value> value =
      contents ? base::JSONReader::Read(*contents, base::JSON_PARSE_RFC)
               : std::nullopt;
  const base::DictValue* data = value ? value->GetIfDict() : nullptr;
  if (data && data->FindInt("version") == kVersion) {
    if (const base::DictValue* secrets = data->FindDict("secrets")) {
      for (const auto [plugin_id, keys] : *secrets) {
        if (!keys.is_dict()) {
          continue;
        }
        for (const auto [key, encoded] : keys.GetDict()) {
          std::string ciphertext;
          std::string plaintext;
          if (encoded.is_string() &&
              base::Base64Decode(encoded.GetString(), &ciphertext) &&
              encryptor_ && encryptor_->DecryptString(ciphertext, &plaintext) &&
              !plaintext.empty()) {
            secrets_[plugin_id][key] = std::move(plaintext);
          }
        }
      }
    }
    if (const base::ListValue* grants = data->FindList("cookie_grants")) {
      for (const base::Value& grant : *grants) {
        if (const std::string* id = grant.GetIfString()) {
          base::Uuid uuid = base::Uuid::ParseLowercase(*id);
          if (uuid.is_valid()) {
            cookie_grants_.insert(uuid);
          }
        }
      }
    }
  }
  loaded_ = true;
  std::vector<base::OnceClosure> callbacks = std::move(when_loaded_);
  for (base::OnceClosure& callback : callbacks) {
    std::move(callback).Run();
  }
}

LiveSecrets LiveFolderStore::GetSecrets(std::string_view plugin_id) const {
  auto it = secrets_.find(plugin_id);
  return it == secrets_.end() ? LiveSecrets() : it->second;
}

bool LiveFolderStore::HasSecret(std::string_view plugin_id,
                                std::string_view key) const {
  auto it = secrets_.find(plugin_id);
  return it != secrets_.end() && it->second.find(key) != it->second.end();
}

void LiveFolderStore::SetSecret(std::string_view plugin_id,
                                std::string_view key,
                                std::string value) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (value.empty()) {
    auto it = secrets_.find(plugin_id);
    if (it == secrets_.end()) {
      return;
    }
    it->second.erase(std::string(key));
    if (it->second.empty()) {
      secrets_.erase(it);
    }
  } else {
    secrets_[std::string(plugin_id)][std::string(key)] = std::move(value);
  }
  ScheduleWrite();
}

void LiveFolderStore::RemoveSecrets(std::string_view plugin_id) {
  auto it = secrets_.find(plugin_id);
  if (it != secrets_.end()) {
    secrets_.erase(it);
    ScheduleWrite();
  }
}

bool LiveFolderStore::CookiesGranted(const base::Uuid& folder_id) const {
  return cookie_grants_.contains(folder_id);
}

void LiveFolderStore::SetCookiesGranted(const base::Uuid& folder_id,
                                        bool granted) {
  const bool changed = granted ? cookie_grants_.insert(folder_id).second
                               : cookie_grants_.erase(folder_id) > 0;
  if (changed) {
    ScheduleWrite();
  }
}

void LiveFolderStore::Flush() {
  if (writer_.HasPendingWrite()) {
    writer_.DoScheduledWrite();
  }
}

void LiveFolderStore::ScheduleWrite() {
  // Before loading, a write would replace what's on disk.
  if (loaded_) {
    writer_.ScheduleWrite(this);
  }
}

std::optional<std::string> LiveFolderStore::SerializeData() {
  base::DictValue secrets;
  for (const auto& [plugin_id, keys] : secrets_) {
    base::DictValue encrypted;
    for (const auto& [key, plaintext] : keys) {
      std::string ciphertext;
      if (encryptor_ && encryptor_->EncryptString(plaintext, &ciphertext)) {
        encrypted.Set(key, base::Base64Encode(ciphertext));
      }
    }
    secrets.Set(plugin_id, std::move(encrypted));
  }
  base::ListValue grants;
  for (const base::Uuid& id : cookie_grants_) {
    grants.Append(id.AsLowercaseString());
  }
  return base::WriteJson(base::DictValue()
                             .Set("version", kVersion)
                             .Set("secrets", std::move(secrets))
                             .Set("cookie_grants", std::move(grants)));
}

}  // namespace compass
