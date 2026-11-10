// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_MEMORY_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_MEMORY_H_

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/important_file_writer.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace compass::claude {

// A fact Claude saved about the user (docs/design/claude.md, "Memory").
struct MemoryEntry {
  // A lowercase UUID; also its sync client tag.
  std::string id;
  std::string text;
  base::Time created;
  base::Time updated;

  bool operator==(const MemoryEntry&) const = default;
};

// A memory's longest text, in characters (UTF-8 bytes may be more).
inline constexpr size_t kMaxMemoryChars = 500;
// The most memories Claude or the user can add (sync may bring more).
inline constexpr size_t kMaxMemories = 200;

// The <memory> system block for |entries|: empty for none. Ids are cut to
// their first 8 characters, which the memory tools accept.
std::string FormatMemoryBlock(const std::vector<MemoryEntry>& entries);

// Normalizes a memory's text: whitespace collapsed to single spaces, trimmed.
// Error if empty or over kMaxMemoryChars.
base::expected<std::string, std::string> NormalizeMemoryText(
    std::string_view text);

// A profile's memories, kept in <profile>/Compass Claude Memory.json:
//   {"version": 1, "memories": [{"id", "text", "created", "updated"}]}
// with times as microseconds since the Windows epoch, in strings. Not in
// Incognito or Guest. Syncs with COMPASS_SPACES (docs/design/sync.md).
class ClaudeMemory : public KeyedService,
                     public base::ImportantFileWriter::DataSerializer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // Any change: local, or from sync.
    virtual void OnMemoriesChanged() = 0;
  };

  explicit ClaudeMemory(const base::FilePath& path);
  ClaudeMemory(const ClaudeMemory&) = delete;
  ClaudeMemory& operator=(const ClaudeMemory&) = delete;
  ~ClaudeMemory() override;

  // Null for profiles without memory (Incognito, Guest).
  static ClaudeMemory* Get(Profile* profile);

  // |done| runs once the file is read, or at once if it was.
  void WhenLoaded(base::OnceClosure done);
  bool loaded() const { return loaded_; }

  // Oldest first.
  const std::vector<MemoryEntry>& entries() const { return entries_; }
  // By id, or by a prefix of at least 8 characters (FormatMemoryBlock()).
  const MemoryEntry* Find(std::string_view id) const;

  base::expected<MemoryEntry, std::string> Add(std::string_view text);
  base::expected<MemoryEntry, std::string> Update(std::string_view id,
                                                  std::string_view text);
  // The removed entry, if there was one.
  std::optional<MemoryEntry> Remove(std::string_view id);
  void RemoveAll();
  // Puts |entry| back as it was (Undo). False if it can't (full, invalid).
  bool Restore(const MemoryEntry& entry);

  // Sync: |upserts| replace or add entries by id, unless the local one was
  // updated later; |deleted| go. Before the file is read, waits for it.
  void ApplyRemote(const std::vector<MemoryEntry>& upserts,
                   const std::set<std::string>& deleted);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Writes pending changes now (shutdown, tests).
  void Flush();

  base::WeakPtr<ClaudeMemory> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // ImportantFileWriter::DataSerializer:
  std::optional<std::string> SerializeData() override;

  // KeyedService:
  void Shutdown() override;

 private:
  void OnRead(std::optional<std::string> contents);
  void Changed();
  std::vector<MemoryEntry>::iterator FindIt(std::string_view id);

  scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  base::ImportantFileWriter writer_;
  bool loaded_ = false;
  std::vector<base::OnceClosure> when_loaded_;
  std::vector<MemoryEntry> entries_;
  base::ObserverList<Observer> observers_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<ClaudeMemory> weak_factory_{this};
};

class ClaudeMemoryFactory : public ProfileKeyedServiceFactory {
 public:
  static ClaudeMemoryFactory* GetInstance();

 private:
  friend base::NoDestructor<ClaudeMemoryFactory>;

  ClaudeMemoryFactory();
  ~ClaudeMemoryFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_MEMORY_H_
