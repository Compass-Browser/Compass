// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ADBLOCK_ABP_PREPROCESSOR_H_
#define COMPASS_BROWSER_ADBLOCK_ABP_PREPROCESSOR_H_

#include <string>
#include <string_view>
#include <vector>

namespace compass::adblock {

enum class LineKind {
  // Blank, a comment or the "[Adblock Plus]" header.
  kComment,
  // A rule Compass can't apply: a scriptlet, a procedural selector, a
  // regular expression, an option like $redirect or $csp (as iOS skips
  // them, ABPConverter.swift).
  kUnsupported,
  // One or more rules for upstream's parser (subresource_filter's
  // RuleParser), in `out`.
  kRules,
};

// Turns a line of an Adblock Plus / uBlock Origin list into the rules
// upstream's parser takes: it skips what neither can apply, rewrites uBO's
// option aliases ($3p, $xhr, ...), and splits element hiding rules with
// excluded domains ("a.com,~b.a.com##.ad"), which the parser rejects, into
// a hiding rule and exceptions ("a.com##.ad", "b.a.com#@#.ad").
LineKind PreprocessLine(std::string_view line, std::vector<std::string>& out);

// The list's "! Title:" header, empty if it has none in its first lines.
std::string FindListTitle(std::string_view text);

}  // namespace compass::adblock

#endif  // COMPASS_BROWSER_ADBLOCK_ABP_PREPROCESSOR_H_
