// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_LIVE_FOLDERS_LIVE_FOLDER_STORE_H_
#define COMPASS_BROWSER_LIVE_FOLDERS_LIVE_FOLDER_STORE_H_

#include <map>
#include <optional>
#include <set>
#include <string>

#include "base/files/file_path.h"
#include "base/files/important_file_writer.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/uuid.h"
#include "compass/browser/live_folders/live_plugin_manifest.h"

namespace os_crypt_async {
class Encryptor;
}

namespace compass {

// A profile's live folder state that doesn't sync and isn't the sidebar's:
// the secrets users entered for plugins (encrypted with the OS's key, as
// passwords are), and which folders may send cookies. Kept in
// <profile>/Compass Live Folders.json:
//   {"version": 1,
//    "secrets": {"<plugin id>": {"<key>": "<base64 ciphertext>"}},
//    "cookie_grants": ["<folder id>", ...]}
class LiveFolderStore : public base::ImportantFileWriter::DataSerializer {
 public:
  explicit LiveFolderStore(const base::FilePath& path);
  LiveFolderStore(const LiveFolderStore&) = delete;
  LiveFolderStore& operator=(const LiveFolderStore&) = delete;
  ~LiveFolderStore() override;

  // Reads the file, decrypting with |encryptor|, which later writes encrypt
  // with too. Secrets that don't decrypt (another machine's key) are
  // dropped. |done| runs once loaded, or at once if it already was.
  void Load(scoped_refptr<os_crypt_async::Encryptor> encryptor,
            base::OnceClosure done);
  bool loaded() const { return loaded_; }

  // A plugin's secrets.
  LiveSecrets GetSecrets(std::string_view plugin_id) const;
  bool HasSecret(std::string_view plugin_id, std::string_view key) const;
  // An empty |value| removes the secret.
  void SetSecret(std::string_view plugin_id,
                 std::string_view key,
                 std::string value);
  void RemoveSecrets(std::string_view plugin_id);

  bool CookiesGranted(const base::Uuid& folder_id) const;
  void SetCookiesGranted(const base::Uuid& folder_id, bool granted);

  // Writes pending changes now (shutdown, tests).
  void Flush();

  // ImportantFileWriter::DataSerializer:
  std::optional<std::string> SerializeData() override;

 private:
  void OnRead(std::optional<std::string> contents);
  void ScheduleWrite();

  scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  base::ImportantFileWriter writer_;
  scoped_refptr<os_crypt_async::Encryptor> encryptor_;
  bool loading_ = false;
  bool loaded_ = false;
  std::vector<base::OnceClosure> when_loaded_;
  std::map<std::string, LiveSecrets, std::less<>> secrets_;
  std::set<base::Uuid> cookie_grants_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<LiveFolderStore> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_LIVE_FOLDERS_LIVE_FOLDER_STORE_H_
