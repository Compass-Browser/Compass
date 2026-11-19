// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_quick.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/i18n/case_conversion.h"
#include "base/i18n/char_iterator.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/claude/claude_assets.h"
#include "compass/browser/claude/claude_client.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/claude/claude_request.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "third_party/icu/source/common/unicode/uchar.h"

namespace compass::claude {

namespace {

// A reply this long isn't a name, or anything else these requests ask for.
constexpr size_t kMaxReplyText = 8 * 1024;

base::DictValue TextBlock(std::string text) {
  return base::DictValue().Set("type", "text").Set("text", std::move(text));
}

std::u16string Trimmed(std::u16string_view text) {
  return std::u16string(base::TrimWhitespace(text, base::TRIM_ALL));
}

base::DictValue Ephemeral() {
  return base::DictValue().Set("type", "ephemeral");
}

bool IsQuote(char16_t c) {
  return c == u'"' || c == u'\'' || c == u'`' || c == u'\u201C' ||
         c == u'\u201D' || c == u'\u2018' || c == u'\u2019' ||
         c == u'\u00AB' || c == u'\u00BB';
}

// What a name may not hold: emoji, control and format characters (which
// emoji sequences and bidi tricks use), private use, line breaks, and the
// leftovers of markup or JSON.
bool IsRefused(UChar32 c) {
  if (c == '<' || c == '>' || c == '{' || c == '}' || c == '[' || c == ']' ||
      c == '"' || c == '`' || c == '\\' || c == '|') {
    return true;
  }
  if (u_hasBinaryProperty(c, UCHAR_EXTENDED_PICTOGRAPHIC) ||
      u_hasBinaryProperty(c, UCHAR_EMOJI_PRESENTATION) ||
      u_hasBinaryProperty(c, UCHAR_REGIONAL_INDICATOR) ||
      u_hasBinaryProperty(c, UCHAR_EMOJI_MODIFIER)) {
    return true;
  }
  const uint32_t category = U_GET_GC_MASK(c);
  return (category & (U_GC_CC_MASK | U_GC_CF_MASK | U_GC_CO_MASK |
                      U_GC_CS_MASK | U_GC_CN_MASK | U_GC_ZL_MASK |
                      U_GC_ZP_MASK)) != 0;
}

}  // namespace

std::optional<std::u16string> SanitizeAiName(std::string_view name) {
  std::u16string text;
  if (!base::UTF8ToUTF16(name.data(), name.size(), &text)) {
    return std::nullopt;
  }
  // Line breaks never belong in a name; anything else around it goes.
  if (text.find_first_of(u"\r\n") != std::u16string::npos) {
    return std::nullopt;
  }
  text = Trimmed(text);
  // Quotes around it, and a full stop after it.
  while (text.size() >= 2 && IsQuote(text.front()) && IsQuote(text.back())) {
    text = Trimmed(std::u16string_view(text).substr(1, text.size() - 2));
  }
  while (!text.empty() && (text.back() == u'.' || text.back() == u'\u3002')) {
    text.pop_back();
  }
  text = base::CollapseWhitespace(text, /*trim_sequences_with_line_breaks=*/
                                  true);
  if (text.empty()) {
    return std::nullopt;
  }
  size_t characters = 0;
  bool has_alnum = false;
  for (base::i18n::UTF16CharIterator it(text); !it.end(); it.Advance()) {
    const UChar32 c = it.get();
    if (IsRefused(c)) {
      return std::nullopt;
    }
    has_alnum |= u_isalnum(c) != 0;
    ++characters;
  }
  if (!has_alnum || characters > kMaxAiNameLength) {
    return std::nullopt;
  }
  const size_t words =
      base::SplitStringPiece(text, u" ", base::TRIM_WHITESPACE,
                             base::SPLIT_WANT_NONEMPTY)
          .size();
  if (words == 0 || words > kMaxAiNameWords) {
    return std::nullopt;
  }
  return text;
}

bool NeedsShortTitle(std::u16string_view title) {
  const std::u16string trimmed(base::TrimWhitespace(title, base::TRIM_ALL));
  size_t characters = 0;
  for (base::i18n::UTF16CharIterator it(trimmed); !it.end(); it.Advance()) {
    ++characters;
  }
  if (characters > kLongTitleLength) {
    return true;
  }
  // A site's name after the page's: "Page · Site", "Page - Site", ...
  static constexpr std::u16string_view kSeparators[] = {
      u" \u00B7 ", u" - ", u" | ", u" \u2014 ", u" \u2013 ", u" :: "};
  for (std::u16string_view separator : kSeparators) {
    const size_t at = trimmed.rfind(separator);
    if (at == std::u16string::npos || at == 0) {
      continue;
    }
    const size_t suffix = trimmed.size() - at - separator.size();
    if (suffix > 0 && suffix <= kMaxSiteSuffixLength) {
      return true;
    }
  }
  return false;
}

std::optional<std::u16string> SanitizeShortTitle(std::string_view title) {
  std::u16string text;
  if (!base::UTF8ToUTF16(title.data(), title.size(), &text)) {
    return std::nullopt;
  }
  if (text.find_first_of(u"\r\n") != std::u16string::npos) {
    return std::nullopt;
  }
  text = Trimmed(text);
  while (text.size() >= 2 && IsQuote(text.front()) && IsQuote(text.back())) {
    text = Trimmed(std::u16string_view(text).substr(1, text.size() - 2));
  }
  while (!text.empty() && (text.back() == u'.' || text.back() == u'\u3002')) {
    text.pop_back();
  }
  text = base::CollapseWhitespace(text, /*trim_sequences_with_line_breaks=*/
                                  true);
  if (text.empty()) {
    return std::nullopt;
  }
  size_t characters = 0;
  bool has_alnum = false;
  for (base::i18n::UTF16CharIterator it(text); !it.end(); it.Advance()) {
    const UChar32 c = it.get();
    if (IsRefused(c)) {
      return std::nullopt;
    }
    has_alnum |= u_isalnum(c) != 0;
    ++characters;
  }
  if (!has_alnum || characters > kMaxShortTitleLength) {
    return std::nullopt;
  }
  return text;
}

std::map<std::string, std::u16string> ParseShortTitlesReply(
    const base::DictValue& reply,
    const std::vector<std::string>& ids) {
  std::map<std::string, std::u16string> titles;
  const base::DictValue* dict = reply.FindDict("titles");
  if (!dict) {
    return titles;
  }
  for (const std::string& id : ids) {
    const std::string* title = dict->FindString(id);
    if (!title) {
      continue;
    }
    if (std::optional<std::u16string> sanitized = SanitizeShortTitle(*title)) {
      titles.emplace(id, std::move(*sanitized));
    }
  }
  return titles;
}

bool LooksLikeQuestion(std::u16string_view text, std::string_view locale) {
  const std::u16string lower = base::i18n::ToLower(
      std::u16string(base::TrimWhitespace(text, base::TRIM_ALL)));
  const std::vector<std::u16string_view> words = base::SplitStringPiece(
      lower, u" ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (words.size() < 2 || lower.find(u"://") != std::u16string::npos) {
    return false;
  }
  const char16_t last = lower.back();
  if (last == u'?' || last == u'\uFF1F' || last == u'\u061F' ||
      lower.front() == u'\u00BF') {
    return true;
  }
  // The first word's letters ("what's" is "what").
  std::u16string first;
  for (base::i18n::UTF16CharIterator it(words[0]); !it.end(); it.Advance()) {
    if (!u_isalpha(it.get())) {
      break;
    }
    base::WriteUnicodeCharacter(it.get(), &first);
  }
  const std::u16string two_words =
      std::u16string(words[0]) + u" " + std::u16string(words[1]);
  // Question words by language ("<language>:<word>"), and pairs of words
  // that start a question ("es:por qué").
  static constexpr std::u16string_view kQuestionWords[] = {
      u"en:who", u"en:what", u"en:why", u"en:how", u"en:when", u"en:where",
      u"en:which", u"en:whose", u"en:whom",
      u"it:chi", u"it:cosa", u"it:che", u"it:perch\u00E9", u"it:perche",
      u"it:come", u"it:quando", u"it:dove", u"it:quale", u"it:quali",
      u"it:quanto", u"it:quanta", u"it:quanti", u"it:quante",
      u"fr:qui", u"fr:que", u"fr:quoi", u"fr:pourquoi", u"fr:comment",
      u"fr:quand", u"fr:o\u00F9", u"fr:quel", u"fr:quelle", u"fr:quels",
      u"fr:quelles", u"fr:combien", u"fr:est-ce que", u"fr:qu'est-ce que",
      u"de:wer", u"de:was", u"de:warum", u"de:wieso", u"de:weshalb",
      u"de:wie", u"de:wann", u"de:wo", u"de:welche", u"de:welcher",
      u"de:welches", u"de:woher", u"de:wohin", u"de:wieviel",
      u"es:qu\u00E9", u"es:qui\u00E9n", u"es:qui\u00E9nes", u"es:c\u00F3mo",
      u"es:cu\u00E1ndo", u"es:d\u00F3nde", u"es:cu\u00E1l", u"es:cu\u00E1les",
      u"es:cu\u00E1nto", u"es:cu\u00E1nta", u"es:cu\u00E1ntos",
      u"es:cu\u00E1ntas", u"es:por qu\u00E9",
      u"pt:quem", u"pt:como", u"pt:quando", u"pt:onde", u"pt:qual",
      u"pt:quais", u"pt:quanto", u"pt:quanta", u"pt:quantos", u"pt:quantas",
      u"pt:o que", u"pt:por que", u"pt:por qu\u00EA",
      u"nl:wie", u"nl:wat", u"nl:waarom", u"nl:hoe", u"nl:wanneer",
      u"nl:waar", u"nl:welke", u"nl:hoeveel",
  };
  const std::u16string language = base::ASCIIToUTF16(base::ToLowerASCII(
      locale.substr(0, std::min(locale.find_first_of("-_"), locale.size()))));
  for (std::u16string_view entry : kQuestionWords) {
    const size_t colon = entry.find(u':');
    const std::u16string_view code = entry.substr(0, colon);
    const std::u16string_view word = entry.substr(colon + 1);
    if ((code == u"en" || code == language) &&
        (word == first || word == two_words)) {
      return true;
    }
  }
  return false;
}

std::string DescribeAskPage(std::string_view page_title,
                            std::string_view page_host) {
  if (page_title.empty() && page_host.empty()) {
    return std::string();
  }
  std::string title(page_title);
  std::ranges::replace_if(
      title, [](char c) { return c == '\r' || c == '\n' || c == '\t'; }, ' ');
  std::string text = "I'm looking at a page";
  if (!title.empty()) {
    base::StrAppend(&text, {" titled \"", title, "\""});
  }
  if (!page_host.empty()) {
    base::StrAppend(&text, {" on ", page_host});
  }
  text += ".";
  return text;
}

QuickRequest MakeAskRequest(std::string_view question,
                            std::string_view page_line,
                            std::string_view model,
                            std::string_view locale) {
  QuickRequest request;
  request.feature_pref = prefs::kCommandBarAsk;
  request.model = std::string(model);
  request.system = SidebarPrompt("command_bar_ask", locale);
  request.user = page_line.empty()
                     ? std::string(question)
                     : base::StrCat({page_line, "\n\n", question});
  request.max_tokens = ClaudeQuick::kMaxTokens;
  // The user asked: no hourly limit, no waiting out a back-off.
  request.background = false;
  request.stream = true;
  return request;
}

TidyFolder::TidyFolder() = default;
TidyFolder::TidyFolder(TidyFolder&&) = default;
TidyFolder& TidyFolder::operator=(TidyFolder&&) = default;
TidyFolder::~TidyFolder() = default;

std::vector<TidyFolder> ParseTidyReply(const base::DictValue& reply,
                                       const std::vector<std::string>& ids) {
  std::vector<TidyFolder> folders;
  const base::ListValue* list = reply.FindList("folders");
  if (!list) {
    return folders;
  }
  const std::set<std::string> known(ids.begin(), ids.end());
  std::set<std::string> taken;
  for (const base::Value& item : *list) {
    if (folders.size() == kMaxTidyFolders) {
      break;
    }
    const base::DictValue* dict = item.GetIfDict();
    const std::string* name = dict ? dict->FindString("name") : nullptr;
    const base::ListValue* entries = dict ? dict->FindList("entries") : nullptr;
    if (!name || !entries) {
      continue;
    }
    std::optional<std::u16string> sanitized = SanitizeAiName(*name);
    if (!sanitized) {
      continue;
    }
    TidyFolder folder;
    folder.name = std::move(*sanitized);
    std::set<std::string> in_folder;
    for (const base::Value& entry : *entries) {
      // Ids are strings; a number for one is taken too.
      std::string id;
      if (entry.is_string()) {
        id = entry.GetString();
      } else if (entry.is_int()) {
        id = base::NumberToString(entry.GetInt());
      } else {
        continue;
      }
      if (!known.contains(id) || taken.contains(id) ||
          !in_folder.insert(id).second) {
        continue;
      }
      folder.entries.push_back(std::move(id));
    }
    if (folder.entries.size() < kMinTidyFolderEntries) {
      continue;
    }
    taken.insert(folder.entries.begin(), folder.entries.end());
    folders.push_back(std::move(folder));
  }
  return folders;
}

std::string SidebarPromptFrom(std::string_view prompts,
                              std::string_view id,
                              std::string_view locale) {
  const std::string heading = "## " + std::string(id);
  std::vector<std::string_view> section;
  bool in_section = false;
  for (std::string_view line : base::SplitStringPiece(
           prompts, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    const std::string_view trimmed =
        base::TrimWhitespaceASCII(line, base::TRIM_TRAILING);
    if (trimmed.starts_with("## ")) {
      if (in_section) {
        break;
      }
      in_section = trimmed == heading;
      continue;
    }
    if (in_section) {
      section.push_back(trimmed);
    }
  }
  const std::string joined = base::JoinString(section, "\n");
  std::string text(base::TrimWhitespaceASCII(joined, base::TRIM_ALL));
  base::ReplaceSubstringsAfterOffset(&text, 0, "{locale}", locale);
  return text;
}

std::string SidebarPrompt(std::string_view id, std::string_view locale) {
  return SidebarPromptFrom(assets::kSidebarPrompts, id, locale);
}

base::DictValue BuildQuickRequest(const QuickRequest& request,
                                  const std::string& model,
                                  CacheLevel cache_level,
                                  bool oauth_token) {
  base::ListValue system;
  if (oauth_token) {
    system.Append(TextBlock(kOAuthSystemLine));
  }
  system.Append(TextBlock(request.system));
  if (cache_level == CacheLevel::kNormal) {
    system.back().GetDict().Set("cache_control", Ephemeral());
  }
  base::DictValue body;
  body.Set("model", model);
  body.Set("max_tokens",
           std::clamp(request.max_tokens, 1, ClaudeQuick::kMaxTokens));
  body.Set("system", std::move(system));
  body.Set("messages",
           base::ListValue().Append(
               base::DictValue()
                   .Set("role", "user")
                   .Set("content",
                        base::ListValue().Append(TextBlock(request.user)))));
  if (cache_level != CacheLevel::kNone) {
    body.Set("cache_control", Ephemeral());
  }
  return body;
}

std::optional<base::DictValue> ParseQuickReply(std::string_view text) {
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(text, base::TRIM_ALL);
  if (!trimmed.starts_with('{')) {
    return std::nullopt;
  }
  return base::JSONReader::ReadDict(trimmed, base::JSON_PARSE_RFC);
}

std::string DescribeQuickError(QuickError error) {
  switch (error) {
    case QuickError::kOff:
      return "It's turned off in Claude settings.";
    case QuickError::kNotSetUp:
      return "Claude isn't set up.";
    case QuickError::kRateLimited:
    case QuickError::kBackingOff:
    case QuickError::kFailed:
      return "Claude couldn't be reached.";
    case QuickError::kTimeout:
      return "Claude took too long.";
    case QuickError::kRefused:
      return "Claude declined to answer.";
    case QuickError::kBadReply:
      return "Claude's answer didn't come through.";
  }
  return std::string();
}

struct ClaudeQuick::Pending {
  QuickRequest request;
  Callback callback;
  TextCallback on_text;
  std::unique_ptr<ClaudeClient> client;
  base::OneShotTimer timer;
  std::string text;
  std::string stop_reason;
  bool finishing = false;
};

ClaudeQuick::ClaudeQuick(PrefService* prefs,
                         GetLoaderFactory get_loader_factory,
                         GetKey get_key)
    : prefs_(prefs),
      get_loader_factory_(std::move(get_loader_factory)),
      get_key_(std::move(get_key)) {}

ClaudeQuick::~ClaudeQuick() = default;

// static
ClaudeQuick* ClaudeQuick::Get(Profile* profile) {
  return ClaudeQuickFactory::GetForProfile(profile);
}

bool ClaudeQuick::IsSetUp() const {
  return ClaudeCredentials::IsSetUp(*prefs_);
}

bool ClaudeQuick::IsEnabled(std::string_view feature_pref) const {
  if (!IsSetUp() || feature_pref.empty()) {
    return false;
  }
  const PrefService::Preference* pref =
      prefs_->FindPreference(feature_pref);
  return pref && pref->GetValue()->is_bool() && pref->GetValue()->GetBool();
}

size_t ClaudeQuick::RecentBackgroundRequests() {
  PruneRecent(base::TimeTicks::Now());
  return recent_.size();
}

void ClaudeQuick::PruneRecent(base::TimeTicks now) {
  while (!recent_.empty() && now - recent_.front() >= base::Hours(1)) {
    recent_.pop_front();
  }
}

void ClaudeQuick::Ask(QuickRequest request, Callback callback) {
  const int id = next_id_++;
  auto pending = std::make_unique<Pending>();
  request.stream = false;
  pending->request = std::move(request);
  pending->callback = std::move(callback);
  pending_.emplace(id, std::move(pending));
  Start(id);
}

int ClaudeQuick::Stream(QuickRequest request,
                        TextCallback on_text,
                        Callback callback) {
  const int id = next_id_++;
  auto pending = std::make_unique<Pending>();
  request.stream = true;
  pending->request = std::move(request);
  pending->callback = std::move(callback);
  pending->on_text = std::move(on_text);
  pending_.emplace(id, std::move(pending));
  Start(id);
  return id;
}

void ClaudeQuick::Cancel(int id) {
  auto it = pending_.find(id);
  if (it == pending_.end()) {
    return;
  }
  Pending& pending = *it->second;
  pending.finishing = true;
  pending.timer.Stop();
  pending.callback.Reset();
  pending.on_text.Reset();
  // Not from inside the client, which may be running this.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<ClaudeQuick> quick, int id) {
                       if (!quick) {
                         return;
                       }
                       auto it = quick->pending_.find(id);
                       if (it == quick->pending_.end()) {
                         return;
                       }
                       if (it->second->client) {
                         it->second->client->Cancel();
                       }
                       quick->pending_.erase(it);
                     },
                     weak_factory_.GetWeakPtr(), id));
}

void ClaudeQuick::Start(int id) {
  const bool background = pending_.at(id)->request.background;
  if (!IsSetUp()) {
    Finish(id, base::unexpected(QuickError::kNotSetUp));
    return;
  }
  if (!IsEnabled(pending_.at(id)->request.feature_pref)) {
    Finish(id, base::unexpected(QuickError::kOff));
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  if (background) {
    if (now < backing_off_until_) {
      Finish(id, base::unexpected(QuickError::kBackingOff));
      return;
    }
    PruneRecent(now);
    if (recent_.size() >= kMaxBackgroundPerHour) {
      Finish(id, base::unexpected(QuickError::kRateLimited));
      return;
    }
    recent_.push_back(now);
  }
  Pending& entry = *pending_.at(id);
  // The answer, or the time it took to give up, includes reading the key.
  entry.timer.Start(FROM_HERE,
                    entry.request.stream ? kStreamTimeout : kTimeout,
                    base::BindOnce(&ClaudeQuick::OnTimeout,
                                   weak_factory_.GetWeakPtr(), id));
  get_key_.Run(
      base::BindOnce(&ClaudeQuick::OnKey, weak_factory_.GetWeakPtr(), id));
}

void ClaudeQuick::OnKey(int id, std::optional<std::string> key) {
  auto it = pending_.find(id);
  if (it == pending_.end() || it->second->finishing) {
    return;
  }
  Endpoint endpoint = EndpointFromPrefs(*prefs_);
  // An OpenAI-compatible server may take no key.
  if (!key || (key->empty() && endpoint.provider != AIProvider::kCompatible)) {
    Finish(id, base::unexpected(QuickError::kNotSetUp));
    return;
  }
  endpoint.api_key = std::move(*key);
  if (NeedsTokenRisksAccepted(endpoint) || !endpoint.base_url.is_valid()) {
    Finish(id, base::unexpected(QuickError::kNotSetUp));
    return;
  }
  Pending& pending = *it->second;
  // No model: the provider's for these requests, else (an OpenAI-compatible
  // server) the chat's.
  std::string model = pending.request.model;
  if (model.empty()) {
    model = endpoint.provider == AIProvider::kAnthropic
                ? std::string(kDefaultQuickModel)
                : std::string(DefaultQuickModel(endpoint.provider));
  }
  if (model.empty()) {
    model = prefs_->GetString(prefs::CurrentPref(*prefs_, prefs::kModel));
  }
  base::DictValue body = BuildQuickRequest(
      pending.request, model, CacheLevelFor(*prefs_, endpoint.base_url),
      UsesOAuthToken(endpoint));
  pending.client = std::make_unique<ClaudeClient>(get_loader_factory_.Run(),
                                                  std::move(endpoint));
  // Another provider is asked for the one JSON object these replies are.
  pending.client->set_json_reply(!pending.request.stream);
  pending.client->StreamMessage(
      std::move(body), /*betas=*/{},
      base::BindRepeating(&ClaudeQuick::OnEvent, weak_factory_.GetWeakPtr(),
                          id),
      base::BindOnce(&ClaudeQuick::OnDone, weak_factory_.GetWeakPtr(), id));
}

void ClaudeQuick::OnEvent(int id, const SseParser::Event& event) {
  auto it = pending_.find(id);
  if (it == pending_.end() || it->second->finishing) {
    return;
  }
  Pending& pending = *it->second;
  if (event.name != "content_block_delta" && event.name != "message_delta") {
    return;
  }
  std::optional<base::DictValue> data =
      base::JSONReader::ReadDict(event.data, base::JSON_PARSE_RFC);
  const base::DictValue* delta = data ? data->FindDict("delta") : nullptr;
  if (!delta) {
    return;
  }
  if (event.name == "message_delta") {
    if (const std::string* reason = delta->FindString("stop_reason")) {
      pending.stop_reason = *reason;
    }
    return;
  }
  const std::string* type = delta->FindString("type");
  const std::string* text = delta->FindString("text");
  if (type && *type == "text_delta" && text &&
      pending.text.size() < kMaxReplyText) {
    pending.text.append(*text);
    if (pending.on_text) {
      pending.on_text.Run(pending.text);
    }
  }
}

void ClaudeQuick::OnDone(int id, std::optional<std::string> error) {
  auto it = pending_.find(id);
  if (it == pending_.end()) {
    return;
  }
  Pending& pending = *it->second;
  if (error) {
    Finish(id, base::unexpected(QuickError::kFailed));
    return;
  }
  if (pending.stop_reason == "refusal") {
    Finish(id, base::unexpected(QuickError::kRefused));
    return;
  }
  if (pending.request.stream) {
    Finish(id, base::DictValue().Set("text", pending.text));
    return;
  }
  std::optional<base::DictValue> reply = ParseQuickReply(pending.text);
  if (!reply) {
    Finish(id, base::unexpected(QuickError::kBadReply));
    return;
  }
  Finish(id, std::move(*reply));
}

void ClaudeQuick::OnTimeout(int id) {
  auto it = pending_.find(id);
  if (it != pending_.end() && it->second->client) {
    it->second->client->Cancel();
  }
  Finish(id, base::unexpected(QuickError::kTimeout));
}

void ClaudeQuick::Finish(int id, Result result) {
  auto it = pending_.find(id);
  if (it == pending_.end() || it->second->finishing) {
    return;
  }
  it->second->finishing = true;
  it->second->timer.Stop();
  // Posted: the client may be running this from inside, and the caller of
  // Ask() may not be done.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&ClaudeQuick::RunFinished,
                                weak_factory_.GetWeakPtr(), id,
                                std::move(result)));
}

void ClaudeQuick::RunFinished(int id, Result result) {
  auto it = pending_.find(id);
  if (it == pending_.end()) {
    return;
  }
  std::unique_ptr<Pending> pending = std::move(it->second);
  pending_.erase(it);
  if (result.has_value()) {
    failures_ = 0;
  } else if (result.error() == QuickError::kFailed ||
             result.error() == QuickError::kTimeout) {
    // The endpoint is down, or refusing: wait before asking again.
    ++failures_;
    backing_off_until_ = base::TimeTicks::Now() +
                         (failures_ == 1 ? kFirstBackOff : kLaterBackOff);
  }
  // None after Cancel().
  if (pending->callback) {
    std::move(pending->callback).Run(std::move(result));
  }
}

// static
ClaudeQuickFactory* ClaudeQuickFactory::GetInstance() {
  static base::NoDestructor<ClaudeQuickFactory> instance;
  return instance.get();
}

// static
ClaudeQuick* ClaudeQuickFactory::GetForProfile(Profile* profile) {
  return static_cast<ClaudeQuick*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

ClaudeQuickFactory::ClaudeQuickFactory()
    : ProfileKeyedServiceFactory(
          "CompassClaudeQuick",
          // Incognito never asks: its own profile has none.
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {}

ClaudeQuickFactory::~ClaudeQuickFactory() = default;

std::unique_ptr<KeyedService>
ClaudeQuickFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  return std::make_unique<ClaudeQuick>(
      profile->GetPrefs(),
      base::BindRepeating(
          [](Profile* profile) {
            return profile->GetDefaultStoragePartition()
                ->GetURLLoaderFactoryForBrowserProcess();
          },
          base::Unretained(profile)),
      base::BindRepeating(
          [](Profile* profile, ClaudeQuick::KeyCallback callback) {
            ClaudeCredentials::GetApiKey(profile, std::move(callback));
          },
          base::Unretained(profile)));
}

}  // namespace compass::claude
