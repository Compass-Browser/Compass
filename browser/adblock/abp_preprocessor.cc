// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/abp_preprocessor.h"

#include <algorithm>
#include <array>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/strcat.h"

namespace compass::adblock {

namespace {

// Pseudo-classes only uBO and ABP's scripts understand: a stylesheet can't
// apply them, and one in a selector group would drop the whole group.
constexpr auto kProceduralPseudoClasses = std::to_array<std::string_view>({
    ":-abp-",          ":contains(",     ":has-text(",
    ":if(",            ":if-not(",       ":matches-attr(",
    ":matches-css",    ":matches-media(", ":matches-path(",
    ":matches-prop(", ":min-text-length(", ":nth-ancestor(",
    ":others(",        ":remove(",       ":remove-attr(",
    ":remove-class(", ":shadow(",       ":spath(",
    ":style(",         ":upward(",       ":watch-attr(",
    ":xpath(",
});

// The options upstream's parser takes (subresource_filter's rule_options.h
// and rule_parser.cc), after RewriteOption().
constexpr auto kKnownOptions = std::to_array<std::string_view>({
    "document",     "domain",     "elemhide",       "font",
    "genericblock", "generichide", "image",         "match-case",
    "media",        "object",     "object-subrequest", "other",
    "ping",         "script",     "stylesheet",     "subdocument",
    "third-party",  "webbundle",  "websocket",      "webtransport",
    "xmlhttprequest",
});

// The options that name what a rule applies to (element and activation
// types), so a rule left with none of them after dropping $popup applies to
// nothing and is dropped too.
constexpr auto kTypeOptions = std::to_array<std::string_view>({
    "document",   "elemhide",   "font",         "genericblock",
    "generichide", "image",     "media",        "object",
    "object-subrequest", "other", "ping",       "script",
    "stylesheet", "subdocument", "webbundle",   "websocket",
    "webtransport", "xmlhttprequest",
});

bool IsAscii(std::string_view text) {
  return std::ranges::all_of(text,
                             [](char c) { return !(c & 0x80); });
}

// A domain list's entry Compass can't match: uBO's entity form
// ("google.*"), or a regular expression.
bool IsUnsupportedDomain(std::string_view domain) {
  return domain.empty() || domain.contains('*') ||
         domain.contains('/');
}

// uBO's names for options upstream's parser knows under another name. An
// empty result drops the option.
std::string RewriteOption(std::string_view option) {
  bool negated = base::StartsWith(option, "~");
  std::string_view name = negated ? option.substr(1) : option;
  std::string_view value;
  if (size_t equals = name.find('='); equals != std::string_view::npos) {
    value = name.substr(equals);
    name = name.substr(0, equals);
  }
  std::string_view prefix = negated ? "~" : "";
  std::string_view opposite = negated ? "" : "~";
  if (name == "3p") {
    return base::StrCat({prefix, "third-party"});
  }
  if (name == "1p" || name == "first-party") {
    return base::StrCat({opposite, "third-party"});
  }
  if (name == "xhr") {
    return base::StrCat({prefix, "xmlhttprequest"});
  }
  if (name == "frame") {
    return base::StrCat({prefix, "subdocument"});
  }
  if (name == "css") {
    return base::StrCat({prefix, "stylesheet"});
  }
  if (name == "doc") {
    return base::StrCat({prefix, "document"});
  }
  if (name == "ehide") {
    return base::StrCat({prefix, "elemhide"});
  }
  if (name == "ghide") {
    return base::StrCat({prefix, "generichide"});
  }
  if (name == "from") {
    return base::StrCat({prefix, "domain", value});
  }
  // Every rule here is as strong as the others, and "all" means the default
  // types, which the rule has without it.
  if (name == "important" || name == "all") {
    return std::string();
  }
  return std::string(option);
}

LineKind PreprocessUrlRule(std::string_view rule,
                           std::vector<std::string>& out) {
  if (!IsAscii(rule)) {
    return LineKind::kUnsupported;
  }
  std::string_view pattern = rule;
  std::string_view options;
  // As upstream's parser does: the last '$' starts the options, unless the
  // rule is a regular expression ending in '/'.
  if (size_t dollar = rule.rfind('$');
      dollar != std::string_view::npos && rule.back() != '/') {
    pattern = rule.substr(0, dollar);
    options = rule.substr(dollar + 1);
  }
  std::string_view bare = pattern;
  if (base::StartsWith(bare, "@@")) {
    bare.remove_prefix(2);
  }
  // Regular expressions: upstream's index skips them.
  if (bare.size() > 2 && bare.front() == '/' && bare.back() == '/') {
    return LineKind::kUnsupported;
  }
  if (options.empty()) {
    out.emplace_back(rule);
    return LineKind::kRules;
  }

  std::vector<std::string> kept;
  bool had_popup = false;
  bool has_type = false;
  for (std::string_view option : base::SplitStringPiece(
           options, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    std::string rewritten = RewriteOption(option);
    if (rewritten.empty()) {
      continue;
    }
    std::string_view name = rewritten;
    if (base::StartsWith(name, "~")) {
      name.remove_prefix(1);
    }
    std::string_view value;
    if (size_t equals = name.find('='); equals != std::string_view::npos) {
      value = name.substr(equals + 1);
      name = name.substr(0, equals);
    }
    // Pop-ups aren't subresources: nothing here can block them.
    if (name == "popup") {
      had_popup = true;
      continue;
    }
    if (!std::ranges::contains(kKnownOptions, name)) {
      return LineKind::kUnsupported;
    }
    if (name == "domain") {
      for (std::string_view domain : base::SplitStringPiece(
               value, "|", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
        if (base::StartsWith(domain, "~")) {
          domain.remove_prefix(1);
        }
        if (IsUnsupportedDomain(domain)) {
          return LineKind::kUnsupported;
        }
      }
    }
    has_type |= std::ranges::contains(kTypeOptions, name);
    kept.push_back(std::move(rewritten));
  }
  if (had_popup && !has_type) {
    return LineKind::kUnsupported;
  }
  if (kept.empty()) {
    out.emplace_back(pattern);
  } else {
    out.push_back(base::StrCat({pattern, "$", base::JoinString(kept, ",")}));
  }
  return LineKind::kRules;
}

LineKind PreprocessStyleRule(std::string_view domains_part,
                             bool is_exception,
                             std::string_view selector,
                             std::vector<std::string>& out) {
  selector = base::TrimWhitespaceASCII(selector, base::TRIM_ALL);
  // "##+js(...)" scriptlets and "##^" HTML filters.
  if (selector.empty() || base::StartsWith(selector, "+js(") ||
      base::StartsWith(selector, "^")) {
    return LineKind::kUnsupported;
  }
  for (std::string_view pseudo : kProceduralPseudoClasses) {
    if (selector.contains(pseudo)) {
      return LineKind::kUnsupported;
    }
  }
  // A selector that would end the stylesheet's rule early.
  if (selector.contains('{') || selector.contains('}')) {
    return LineKind::kUnsupported;
  }

  std::vector<std::string_view> included;
  std::vector<std::string_view> excluded;
  for (std::string_view domain : base::SplitStringPiece(
           domains_part, ",", base::TRIM_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    const bool exclude = base::StartsWith(domain, "~");
    if (exclude) {
      domain.remove_prefix(1);
    }
    if (IsUnsupportedDomain(domain) || !IsAscii(domain)) {
      return LineKind::kUnsupported;
    }
    (exclude ? excluded : included).push_back(domain);
  }

  if (is_exception) {
    // An exception for every site isn't something upstream's index takes,
    // and "~a.com#@#" (an exception everywhere but there) isn't either.
    if (included.empty()) {
      return LineKind::kUnsupported;
    }
    out.push_back(base::StrCat(
        {base::JoinString(included, ","), "#@#", selector}));
    return LineKind::kRules;
  }

  out.push_back(
      base::StrCat({base::JoinString(included, ","), "##", selector}));
  for (std::string_view domain : excluded) {
    out.push_back(base::StrCat({domain, "#@#", selector}));
  }
  return LineKind::kRules;
}

}  // namespace

LineKind PreprocessLine(std::string_view line, std::vector<std::string>& out) {
  line = base::TrimWhitespaceASCII(line, base::TRIM_ALL);
  if (line.empty() || line.front() == '!' || line.front() == '[') {
    return LineKind::kComment;
  }
  // uBO's HTML filters ("example.com$$script") and their exceptions.
  if (line.contains("$$") || line.contains("$@$")) {
    return LineKind::kUnsupported;
  }

  // Element hiding and its relatives: "##", "#@#", and the ones only
  // scripts can apply ("#?#", "#$#", "#%#" and their "#@" exceptions).
  for (size_t pos = line.find('#'); pos != std::string_view::npos;
       pos = line.find('#', pos + 1)) {
    std::string_view rest = line.substr(pos);
    if (base::StartsWith(rest, "##")) {
      return PreprocessStyleRule(line.substr(0, pos), /*is_exception=*/false,
                                 rest.substr(2), out);
    }
    if (base::StartsWith(rest, "#@#")) {
      return PreprocessStyleRule(line.substr(0, pos), /*is_exception=*/true,
                                 rest.substr(3), out);
    }
    for (std::string_view marker :
         {"#?#", "#$#", "#%#", "#@?#", "#@$#", "#@%#"}) {
      if (base::StartsWith(rest, marker)) {
        return LineKind::kUnsupported;
      }
    }
  }
  return PreprocessUrlRule(line, out);
}

std::string FindListTitle(std::string_view text) {
  constexpr size_t kHeaderLines = 40;
  for (size_t count = 0; count < kHeaderLines && !text.empty(); ++count) {
    size_t end = text.find('\n');
    std::string_view line = base::TrimWhitespaceASCII(
        text.substr(0, end), base::TRIM_ALL);
    text = end == std::string_view::npos ? std::string_view()
                                         : text.substr(end + 1);
    if (line.empty() || line.front() != '!') {
      continue;
    }
    line.remove_prefix(1);
    line = base::TrimWhitespaceASCII(line, base::TRIM_LEADING);
    if (base::StartsWith(line, "Title:",
                         base::CompareCase::INSENSITIVE_ASCII)) {
      return std::string(base::TrimWhitespaceASCII(
          line.substr(sizeof("Title:") - 1), base::TRIM_ALL));
    }
  }
  return std::string();
}

}  // namespace compass::adblock
