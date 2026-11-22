// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/ruleset_builder.h"

#include <set>
#include <string_view>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "compass/browser/adblock/abp_preprocessor.h"
#include "compass/browser/adblock/filter_lists.h"
#include "components/subresource_filter/core/common/unindexed_ruleset.h"
#include "components/subresource_filter/tools/rule_parser/rule.h"
#include "components/subresource_filter/tools/rule_parser/rule_parser.h"
#include "components/url_pattern_index/proto/rules.pb.h"
#include "crypto/hash.h"
#include "third_party/protobuf/src/google/protobuf/io/zero_copy_stream_impl_lite.h"

namespace compass::adblock {

namespace {

namespace proto = url_pattern_index::proto;

// Bump when the same lists should build different rules (a preprocessor or
// builder change), so the next launch indexes them again.
constexpr char kBuilderVersion[] = "1";

// Lists larger than this aren't read (EasyList is about 2 MB).
constexpr int64_t kMaxListSize = 30 * 1024 * 1024;

// What upstream's converter does to a network rule before indexing
// (subresource_filter/tools/ruleset_converter/rule_stream.cc
// DeleteUrlRuleOrAmend): no pop-ups, only the activation types Chrome
// applies. False when nothing is left for the rule to apply to.
bool AmendUrlRule(proto::UrlRule& rule) {
  if (rule.url_pattern_type() == proto::URL_PATTERN_TYPE_REGEXP) {
    return false;
  }
  rule.set_element_types(rule.element_types() & ~proto::ELEMENT_TYPE_POPUP);
  rule.set_activation_types(
      rule.activation_types() &
      (proto::ACTIVATION_TYPE_DOCUMENT | proto::ACTIVATION_TYPE_GENERICBLOCK));
  if (!rule.activation_types()) {
    rule.clear_activation_types();
  }
  if (!rule.element_types()) {
    rule.clear_element_types();
  }
  return rule.element_types() || rule.activation_types();
}

// Fills the classes and ids upstream's style index files the rule under. False
// for a selector it won't take (one for every site with no class or id to
// look it up by, or a pseudo-class without one).
bool AmendStyleRule(proto::StyleRule& rule) {
  std::vector<std::string> classes;
  std::vector<std::string> ids;
  if (!subresource_filter::GetAnchorsIfSupported(
          rule.selector(), /*is_site_specific=*/rule.domains_size() > 0,
          classes, ids)) {
    return false;
  }
  for (std::string& name : classes) {
    rule.add_classes(std::move(name));
  }
  for (std::string& name : ids) {
    rule.add_ids(std::move(name));
  }
  return true;
}

std::string HexHash(std::string_view text) {
  auto hash = crypto::hash::Sha256(text);
  return base::ToLowerASCII(base::HexEncode(base::span(hash).first<12>()));
}

base::FilePath UnindexedDir(const base::FilePath& user_data_dir) {
  return GetAdBlockDir(user_data_dir).AppendASCII("Unindexed");
}

}  // namespace

std::string BuildUnindexedRuleset(const std::vector<ListText>& lists,
                                  std::map<std::string, ListCounts>* counts) {
  std::vector<proto::UrlRule> url_rules;
  // Element hiding exceptions and site rules, then rules for every site.
  std::vector<proto::StyleRule> site_style_rules;
  std::vector<proto::StyleRule> generic_style_rules;
  std::set<std::string> seen;

  subresource_filter::RuleParser parser;
  std::vector<std::string> rules;
  for (const ListText& list : lists) {
    ListCounts list_counts;
    std::string_view text = list.text;
    while (!text.empty()) {
      size_t end = text.find('\n');
      std::string_view line = text.substr(0, end);
      text = end == std::string_view::npos ? std::string_view()
                                           : text.substr(end + 1);
      rules.clear();
      switch (PreprocessLine(line, rules)) {
        case LineKind::kComment:
          continue;
        case LineKind::kUnsupported:
          ++list_counts.skipped;
          continue;
        case LineKind::kRules:
          break;
      }
      bool used = false;
      bool used_css = false;
      for (const std::string& rule : rules) {
        switch (parser.Parse(rule)) {
          case proto::RULE_TYPE_URL: {
            proto::UrlRule url_rule = parser.url_rule().ToProtobuf();
            if (!AmendUrlRule(url_rule)) {
              break;
            }
            used = true;
            if (seen.insert("u" + url_rule.SerializeAsString()).second) {
              url_rules.push_back(std::move(url_rule));
            }
            break;
          }
          case proto::RULE_TYPE_STYLE: {
            proto::StyleRule style_rule = parser.style_rule().ToProtobuf();
            if (!AmendStyleRule(style_rule)) {
              break;
            }
            used = used_css = true;
            if (!seen.insert("s" + style_rule.SerializeAsString()).second) {
              break;
            }
            const bool site_specific =
                style_rule.domains_size() > 0 ||
                style_rule.semantics() == proto::RULE_SEMANTICS_ALLOWLIST;
            (site_specific ? site_style_rules : generic_style_rules)
                .push_back(std::move(style_rule));
            break;
          }
          default:
            break;
        }
      }
      if (!used) {
        ++list_counts.skipped;
      } else if (used_css) {
        ++list_counts.css_rules;
      } else {
        ++list_counts.url_rules;
      }
    }
    if (counts) {
      (*counts)[list.id] = list_counts;
    }
  }

  std::string output;
  {
    google::protobuf::io::StringOutputStream stream(&output);
    subresource_filter::UnindexedRulesetWriter writer(&stream);
    for (const proto::UrlRule& rule : url_rules) {
      writer.AddUrlRule(rule);
    }
    for (const proto::StyleRule& rule : site_style_rules) {
      writer.AddStyleRule(rule);
    }
    for (const proto::StyleRule& rule : generic_style_rules) {
      writer.AddStyleRule(rule);
    }
    writer.Finish();
  }
  return output;
}

std::string ComputeContentVersion(const std::vector<ListText>& lists) {
  std::string key = kBuilderVersion;
  for (const ListText& list : lists) {
    key += '\n';
    key += list.id;
    key += ' ';
    key += HexHash(list.text);
  }
  return HexHash(key);
}

BuiltRuleset::BuiltRuleset() = default;
BuiltRuleset::BuiltRuleset(BuiltRuleset&&) = default;
BuiltRuleset& BuiltRuleset::operator=(BuiltRuleset&&) = default;
BuiltRuleset::~BuiltRuleset() = default;

std::optional<BuiltRuleset> BuildRulesetFile(
    const base::FilePath& user_data_dir,
    const std::vector<std::string>& ids) {
  std::vector<ListText> lists;
  for (const std::string& id : ids) {
    std::string text;
    if (base::ReadFileToStringWithMaxSize(GetListTextPath(user_data_dir, id),
                                          &text, kMaxListSize)) {
      lists.push_back({id, std::move(text)});
    }
  }
  // With no list downloaded or on, an empty ruleset: the one published
  // before must not go on blocking.
  BuiltRuleset built;
  built.content_version = ComputeContentVersion(lists);
  const base::FilePath dir = UnindexedDir(user_data_dir);
  built.path = dir.AppendASCII(built.content_version + ".pb");

  if (!base::PathExists(built.path)) {
    std::map<std::string, ListCounts> counts;
    std::string ruleset = BuildUnindexedRuleset(lists, &counts);
    if (!base::CreateDirectory(dir) ||
        !base::ImportantFileWriter::WriteFileAtomically(built.path, ruleset)) {
      LOG(ERROR) << "Ad blocker: can't write " << built.path;
      return std::nullopt;
    }
    built.counts = std::move(counts);
  }

  // The rulesets of earlier lists. Upstream's service copies what it indexes
  // into its own folder, so nothing reads them any more.
  base::FileEnumerator files(dir, /*recursive=*/false,
                             base::FileEnumerator::FILES);
  for (base::FilePath file = files.Next(); !file.empty();
       file = files.Next()) {
    if (file != built.path) {
      base::DeleteFile(file);
    }
  }
  return built;
}

}  // namespace compass::adblock
