// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_memory.h"

#include <memory>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/uuid.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

class ClaudeMemoryTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }

  base::FilePath Path() const {
    return dir_.GetPath().AppendASCII("Compass Claude Memory.json");
  }

  std::unique_ptr<ClaudeMemory> Load() {
    auto memory = std::make_unique<ClaudeMemory>(Path());
    base::test::TestFuture<void> loaded;
    memory->WhenLoaded(loaded.GetCallback());
    EXPECT_TRUE(loaded.Wait());
    return memory;
  }

  MemoryEntry Entry(std::string text, base::Time updated) {
    MemoryEntry entry;
    entry.id = base::Uuid::GenerateRandomV4().AsLowercaseString();
    entry.text = std::move(text);
    entry.created = updated;
    entry.updated = updated;
    return entry;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir dir_;
};

TEST_F(ClaudeMemoryTest, AddUpdateRemovePersist) {
  std::string id;
  {
    std::unique_ptr<ClaudeMemory> memory = Load();
    ASSERT_TRUE(memory->entries().empty());
    auto added = memory->Add("  Prefers   answers\nin Italian ");
    ASSERT_TRUE(added.has_value());
    EXPECT_EQ(added->text, "Prefers answers in Italian");
    id = added->id;
    // The same fact again is the same memory.
    EXPECT_EQ(memory->Add("prefers answers in italian")->id, id);
    // Later, so the order (by creation) is defined.
    task_environment_.FastForwardBy(base::Seconds(1));
    ASSERT_TRUE(memory->Add("Lives in Milan").has_value());
    EXPECT_EQ(memory->entries().size(), 2u);

    // By the short id the prompt shows.
    auto updated = memory->Update(id.substr(0, 8), "Prefers Italian");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->id, id);
    EXPECT_FALSE(memory->Update("0000", "x").has_value());
    // Destroying writes.
  }
  task_environment_.RunUntilIdle();

  std::unique_ptr<ClaudeMemory> memory = Load();
  ASSERT_EQ(memory->entries().size(), 2u);
  EXPECT_EQ(memory->entries()[0].text, "Prefers Italian");
  EXPECT_EQ(memory->entries()[1].text, "Lives in Milan");

  std::optional<MemoryEntry> removed = memory->Remove(id);
  ASSERT_TRUE(removed);
  EXPECT_EQ(memory->entries().size(), 1u);
  // Undo puts it back in its place.
  EXPECT_TRUE(memory->Restore(*removed));
  EXPECT_EQ(memory->entries()[0].id, id);
  memory->RemoveAll();
  EXPECT_TRUE(memory->entries().empty());
}

TEST_F(ClaudeMemoryTest, Limits) {
  std::unique_ptr<ClaudeMemory> memory = Load();
  EXPECT_FALSE(memory->Add("   ").has_value());
  EXPECT_FALSE(memory->Add(std::string(kMaxMemoryChars + 1, 'a')).has_value());
  EXPECT_TRUE(memory->Add(std::string(kMaxMemoryChars, 'a')).has_value());
  for (size_t i = 1; i < kMaxMemories; ++i) {
    ASSERT_TRUE(memory->Add("Fact " + base::NumberToString(i)).has_value());
  }
  auto full = memory->Add("One more");
  ASSERT_FALSE(full.has_value());
  EXPECT_NE(full.error().find("full"), std::string::npos);
}

TEST_F(ClaudeMemoryTest, RemoteChangesLaterEditWins) {
  std::unique_ptr<ClaudeMemory> memory = Load();
  const base::Time now = base::Time::Now();
  MemoryEntry a = Entry("A", now);
  MemoryEntry b = Entry("B", now);
  memory->ApplyRemote({a, b}, {});
  ASSERT_EQ(memory->entries().size(), 2u);

  // An older remote edit doesn't overwrite a newer local one.
  task_environment_.FastForwardBy(base::Seconds(5));
  ASSERT_TRUE(memory->Update(a.id, "A local").has_value());
  MemoryEntry stale = a;
  stale.text = "A stale";
  stale.updated = now + base::Seconds(1);
  memory->ApplyRemote({stale}, {});
  EXPECT_EQ(memory->Find(a.id)->text, "A local");

  // A newer one does; deletions go.
  MemoryEntry fresh = a;
  fresh.text = "A fresh";
  fresh.updated = now + base::Minutes(1);
  memory->ApplyRemote({fresh}, {b.id});
  EXPECT_EQ(memory->Find(a.id)->text, "A fresh");
  EXPECT_FALSE(memory->Find(b.id));
}

TEST_F(ClaudeMemoryTest, RemoteChangesWaitForTheFile) {
  {
    std::unique_ptr<ClaudeMemory> memory = Load();
    ASSERT_TRUE(memory->Add("Kept").has_value());
    ASSERT_TRUE(memory->Add("Deleted elsewhere").has_value());
  }
  task_environment_.RunUntilIdle();
  std::string deleted_id;
  {
    std::unique_ptr<ClaudeMemory> memory = Load();
    deleted_id = memory->entries()[1].id;
  }
  task_environment_.RunUntilIdle();

  auto memory = std::make_unique<ClaudeMemory>(Path());
  ASSERT_FALSE(memory->loaded());
  memory->ApplyRemote({Entry("From sync", base::Time::Now())}, {deleted_id});
  base::test::TestFuture<void> loaded;
  memory->WhenLoaded(loaded.GetCallback());
  ASSERT_TRUE(loaded.Wait());
  task_environment_.RunUntilIdle();
  ASSERT_EQ(memory->entries().size(), 2u);
  EXPECT_EQ(memory->entries()[0].text, "Kept");
  EXPECT_EQ(memory->entries()[1].text, "From sync");
}

TEST(ClaudeMemoryBlockTest, Format) {
  EXPECT_EQ(FormatMemoryBlock({}), "");
  MemoryEntry entry;
  entry.id = "0123abcd-0000-4000-8000-000000000000";
  entry.text = "Prefers Italian";
  const std::string block = FormatMemoryBlock({entry});
  EXPECT_TRUE(block.starts_with("<memory>\n"));
  EXPECT_TRUE(block.ends_with("</memory>"));
  EXPECT_NE(block.find("- [0123abcd] Prefers Italian\n"), std::string::npos);
}

}  // namespace
}  // namespace compass::claude
