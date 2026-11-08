// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/profile_list.h"

#include <map>
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

// The Windows device of 2026-11-07: Work and Play were created three times
// (the first pair still on the old server and seed), and the home's
// compass.sync.profile_dirs has the last pair, which the switcher lists.
TEST(PlanProfileDirsTest, KeepsTheListedCopyAndRetiresTheOthers) {
  const std::vector<ProfileDir> dirs = {
      {"Default", kHome, "B", "new", false, true, true},
      {"Profile 1", kWork, "A", "old", true, false, false},
      {"Profile 2", kPlay, "A", "old", true, false, false},
      {"Profile 3", kWork, "B", "new", true, false, false},
      {"Profile 4", kPlay, "B", "new", true, false, false},
      {"Profile 5", kWork, "B", "new", true, true, true},
      {"Profile 6", kPlay, "B", "new", true, true, true},
  };
  const ProfileDirPlan plan = PlanProfileDirs(
      dirs, kHome, "B", "new",
      {{kWork, "Profile 5"}, {kPlay, "Profile 6"}});
  EXPECT_EQ(plan.keep, (std::map<std::string, std::string>{
                           {kWork, "Profile 5"}, {kPlay, "Profile 6"}}));
  EXPECT_EQ(plan.retire, (std::vector<std::string>{"Profile 1", "Profile 2",
                                                   "Profile 3", "Profile 4"}));
}

// compass.sync.profile_dirs was cleared (the chain was left and joined
// again): the existing directory is found again, nothing is retired on a
// guess, and no profile needs creating.
TEST(PlanProfileDirsTest, FindsAnUnrecordedProfileWithoutRetiring) {
  const std::vector<ProfileDir> dirs = {
      {"Profile 1", kWork, "A", "old", true, false, false},
      {"Profile 3", kWork, "B", "new", true, false, false},
  };
  const ProfileDirPlan plan = PlanProfileDirs(dirs, kHome, "B", "new", {});
  EXPECT_EQ(plan.keep,
            (std::map<std::string, std::string>{{kWork, "Profile 3"}}));
  EXPECT_TRUE(plan.retire.empty());
}

// A profile the user had before joining (listed, not sync's own account)
// is never retired, and a listed one is kept over the recorded one.
TEST(PlanProfileDirsTest, NeverRetiresTheUsersOwnProfiles) {
  const std::vector<ProfileDir> dirs = {
      {"Default", kHome, "B", "new", false, true, true},
      {"Profile 1", kWork, "B", "new", false, true, true},
      {"Profile 2", kWork, "B", "new", true, false, false},
      {"Profile 3", kWork, "B", "new", false, false, false},
      {"Profile 4", kHome, "B", "new", true, false, false},
  };
  const ProfileDirPlan plan =
      PlanProfileDirs(dirs, kHome, "B", "new", {{kWork, "Profile 2"}});
  EXPECT_EQ(plan.keep,
            (std::map<std::string, std::string>{{kWork, "Profile 1"}}));
  // Profile 3 isn't sync's own account; the home's UUID is left alone.
  EXPECT_EQ(plan.retire, (std::vector<std::string>{"Profile 2"}));
}

// The Windows device of 2026-11-08: a crash came after Profile 3's
// Preferences were written but before Local State was, so Chromium doesn't
// know it (and never deletes it). It's removed from disk; Profile 1, the
// recorded one, is kept.
TEST(PlanProfileDirsTest, RemovesAnUnregisteredCopy) {
  ProfileDir lost = {"Profile 3", kWork, "A", "old", true, false, false};
  lost.registered = false;
  const std::vector<ProfileDir> dirs = {
      {"Default", kHome, "B", "new", false, true, true},
      {"Profile 1", kWork, "A", "old", true, true, true},
      lost,
  };
  const ProfileDirPlan plan =
      PlanProfileDirs(dirs, kHome, "B", "new", {{kWork, "Profile 1"}});
  EXPECT_EQ(plan.keep,
            (std::map<std::string, std::string>{{kWork, "Profile 1"}}));
  EXPECT_TRUE(plan.retire.empty());
  EXPECT_EQ(plan.remove, (std::vector<std::string>{"Profile 3"}));
}

// An unregistered directory is never kept, and without a registered one
// for its UUID nothing is removed on a guess (the profile is created again).
TEST(PlanProfileDirsTest, KeepsNoUnregisteredDirectory) {
  ProfileDir lost = {"Profile 3", kWork, "B", "new", true, false, false};
  lost.registered = false;
  ProfileDir own = {"Profile 4", kPlay, "B", "new", false, false, false};
  own.registered = false;
  const ProfileDirPlan plan = PlanProfileDirs({lost, own}, kHome, "B", "new",
                                              {{kWork, "Profile 3"}});
  EXPECT_TRUE(plan.keep.empty());
  EXPECT_TRUE(plan.retire.empty());
  EXPECT_TRUE(plan.remove.empty());
}

}  // namespace
}  // namespace compass::sync
