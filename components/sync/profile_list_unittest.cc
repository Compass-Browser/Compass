// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/profile_list.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::sync {
namespace {

constexpr char kHome[] = "2b5c3e1a-0000-4000-8000-000000000001";
constexpr char kWork[] = "2b5c3e1a-0000-4000-8000-000000000002";
constexpr char kPlay[] = "2b5c3e1a-0000-4000-8000-000000000003";

std::vector<ChainProfile> Chain() {
  return {
      {kHome, "Personal", 26, true},
      {kWork, "Work", 3, false},
      {kPlay, "Play", -1, false},
  };
}

TEST(ChainProfilesTest, JsonRoundTrip) {
  const base::ListValue value = ChainProfilesToValue(Chain());
  EXPECT_EQ(base::WriteJson(value),
            std::string("[{\"avatar\":26,\"home\":true,\"name\":\"Personal\","
                        "\"uuid\":\"") +
                kHome +
                "\"},{\"avatar\":3,\"name\":\"Work\",\"uuid\":\"" + kWork +
                "\"},{\"name\":\"Play\",\"uuid\":\"" + kPlay + "\"}]");
  EXPECT_EQ(ChainProfilesFromValue(value), Chain());
}

TEST(ChainProfilesTest, SkipsInvalidEntries) {
  std::optional<base::Value> value = base::JSONReader::Read(
      std::string("[3, {\"name\": \"No UUID\"}, {\"uuid\": \"NOT-A-UUID\"},") +
          "{\"uuid\": \"" + kWork + "\", \"name\": \"Work\", \"avatar\": -4}," +
          "{\"uuid\": \"" + kWork + "\", \"name\": \"Again\"}]",
      base::JSON_PARSE_RFC);
  ASSERT_TRUE(value && value->is_list());
  const std::vector<ChainProfile> profiles =
      ChainProfilesFromValue(value->GetList());
  ASSERT_EQ(profiles.size(), 1u);
  EXPECT_EQ(profiles[0], (ChainProfile{kWork, "Work", -1, false}));
}

TEST(ProfileMappingTest, DefaultMatchesByNameElseNew) {
  const std::vector<LocalProfile> locals = {
      {"Profile 1", "local-1", " work ", 0},
      {"Profile 2", "local-2", "Travel", 0},
      // Its name is the home's: the home is the joining profile's, so this
      // one joins as new.
      {"Profile 3", "local-3", "Personal", 0},
      // A second "Work": the chain's is taken.
      {"Profile 4", "local-4", "Work", 0},
  };
  const ProfileMapping mapping = DefaultProfileMapping(Chain(), locals);
  EXPECT_EQ(mapping, (ProfileMapping{{"Profile 1", kWork},
                                     {"Profile 2", ""},
                                     {"Profile 3", ""},
                                     {"Profile 4", ""}}));
  EXPECT_TRUE(IsValidProfileMapping(mapping, Chain(), locals));
  // Play has no local profile: it's created.
  const std::vector<ChainProfile> create = ProfilesToCreate(Chain(), mapping);
  ASSERT_EQ(create.size(), 1u);
  EXPECT_EQ(create[0].uuid, kPlay);
}

TEST(ProfileMappingTest, ValidatesUserChoices) {
  const std::vector<LocalProfile> locals = {
      {"Profile 1", "local-1", "Job", 0},
      {"Profile 2", "local-2", "Games", 0},
  };
  // The user maps Job to Work and Games to Play: nothing to create.
  ProfileMapping mapping = {{"Profile 1", kWork}, {"Profile 2", kPlay}};
  EXPECT_TRUE(IsValidProfileMapping(mapping, Chain(), locals));
  EXPECT_TRUE(ProfilesToCreate(Chain(), mapping).empty());

  // Not the home, not twice, not an unknown profile, every local once.
  EXPECT_FALSE(IsValidProfileMapping(
      {{"Profile 1", kHome}, {"Profile 2", ""}}, Chain(), locals));
  EXPECT_FALSE(IsValidProfileMapping(
      {{"Profile 1", kWork}, {"Profile 2", kWork}}, Chain(), locals));
  EXPECT_FALSE(IsValidProfileMapping(
      {{"Profile 1", "2b5c3e1a-0000-4000-8000-0000000000ff"},
       {"Profile 2", ""}},
      Chain(), locals));
  EXPECT_FALSE(IsValidProfileMapping({{"Profile 1", ""}}, Chain(), locals));
  EXPECT_FALSE(IsValidProfileMapping({{"Profile 1", ""}, {"Profile 1", ""}},
                                     Chain(), locals));
  EXPECT_FALSE(IsValidProfileMapping({{"Profile 1", ""}, {"Profile 9", ""}},
                                     Chain(), locals));
}

// A creator that remembers what it was asked and answers when told.
class FakeCreator : public MissingProfiles::Creator {
 public:
  void CreateProfile(const ChainProfile& profile,
                     base::OnceCallback<void(bool created)> done) override {
    requests_.push_back(profile);
    pending_.push_back(std::move(done));
  }

  void Answer(size_t i, bool created) { std::move(pending_[i]).Run(created); }
  const std::vector<ChainProfile>& requests() const { return requests_; }

 private:
  std::vector<ChainProfile> requests_;
  std::vector<base::OnceCallback<void(bool)>> pending_;
};

TEST(MissingProfilesTest, CreatesUnknownProfilesOnce) {
  FakeCreator creator;
  MissingProfiles missing(&creator);

  // This device has the home and Work: Play is created, not the home.
  missing.Update(Chain(), {kHome, kWork});
  ASSERT_EQ(creator.requests().size(), 1u);
  EXPECT_EQ(creator.requests()[0], Chain()[2]);
  EXPECT_TRUE(missing.IsCreating());

  // The list comes again while it's being created: not twice.
  missing.Update(Chain(), {kHome, kWork});
  EXPECT_EQ(creator.requests().size(), 1u);

  creator.Answer(0, true);
  EXPECT_FALSE(missing.IsCreating());
  missing.Update(Chain(), {kHome, kWork, kPlay});
  EXPECT_EQ(creator.requests().size(), 1u);
}

TEST(MissingProfilesTest, AFailedCreationIsntRetriedInTheSameRun) {
  FakeCreator creator;
  MissingProfiles missing(&creator);
  missing.Update(Chain(), {});
  // Work and Play; never the home, which is the joining profile.
  ASSERT_EQ(creator.requests().size(), 2u);
  EXPECT_EQ(creator.requests()[0].uuid, kWork);
  EXPECT_EQ(creator.requests()[1].uuid, kPlay);
  creator.Answer(0, false);
  creator.Answer(1, true);
  missing.Update(Chain(), {kPlay});
  EXPECT_EQ(creator.requests().size(), 2u);
}

}  // namespace
}  // namespace compass::sync
