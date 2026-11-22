// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ADBLOCK_RULESET_BUILDER_H_
#define COMPASS_BROWSER_ADBLOCK_RULESET_BUILDER_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"

// Builds the ruleset upstream's subresource filter indexes and every
// renderer applies, from the enabled lists' text (docs/design/ad-blocker.md
// "Rules"). Runs on a background sequence.
namespace compass::adblock {

struct ListText {
  std::string id;
  std::string text;
};

struct ListCounts {
  int url_rules = 0;
  int css_rules = 0;
  int skipped = 0;

  bool operator==(const ListCounts&) const = default;
};

// The rules of `lists`, in upstream's unindexed format (a stream of
// url_pattern_index::proto::FilteringRules chunks): network rules, then
// element hiding exceptions and site rules, then the rules for every site,
// the order upstream's style index needs. A rule two lists share is written
// once. `counts` gets each list's numbers.
std::string BuildUnindexedRuleset(const std::vector<ListText>& lists,
                                  std::map<std::string, ListCounts>* counts);

// The ruleset's content version: the same lists with the same text give the
// same version, so a restart doesn't index them again (on Windows indexing
// the same version twice fails).
std::string ComputeContentVersion(const std::vector<ListText>& lists);

struct BuiltRuleset {
  BuiltRuleset();
  BuiltRuleset(BuiltRuleset&&);
  BuiltRuleset& operator=(BuiltRuleset&&);
  ~BuiltRuleset();

  std::string content_version;
  base::FilePath path;
  // Only when the ruleset was built now, not found from an earlier build.
  std::optional<std::map<std::string, ListCounts>> counts;
};

// Reads the lists `ids` downloaded (a list not downloaded yet is left out),
// and writes their ruleset to <user data>/Compass Ad Block/Unindexed/ unless
// that version is there already; removes the older ones. With no list, an
// empty ruleset. Nothing when the file can't be written.
std::optional<BuiltRuleset> BuildRulesetFile(
    const base::FilePath& user_data_dir,
    const std::vector<std::string>& ids);

}  // namespace compass::adblock

#endif  // COMPASS_BROWSER_ADBLOCK_RULESET_BUILDER_H_
