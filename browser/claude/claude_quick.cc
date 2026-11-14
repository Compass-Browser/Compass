// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_quick.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
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
#include "third_party/icu/source/common/unicode/utf16.h"

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
  for (size_t i = 0; i < text.size();) {
    UChar32 c;
    U16_NEXT(text.data(), i, text.size(), c);
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

struct ClaudeQuick::Pending {
  QuickRequest request;
  Callback callback;
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
  return !prefs_->GetString(prefs::kApiKey).empty();
}

bool ClaudeQuick::IsEnabled() const {
  return IsSetUp() && prefs_->GetBoolean(prefs::kSidebarAi);
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
  pending->request = std::move(request);
  pending->callback = std::move(callback);
  const bool background = pending->request.background;
  pending_.emplace(id, std::move(pending));

  if (!IsSetUp()) {
    Finish(id, base::unexpected(QuickError::kNotSetUp));
    return;
  }
  if (!prefs_->GetBoolean(prefs::kSidebarAi)) {
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
  entry.timer.Start(FROM_HERE, kTimeout,
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
  if (!key || key->empty()) {
    Finish(id, base::unexpected(QuickError::kNotSetUp));
    return;
  }
  Endpoint endpoint = EndpointFromPrefs(*prefs_);
  endpoint.api_key = std::move(*key);
  if (NeedsTokenRisksAccepted(endpoint)) {
    Finish(id, base::unexpected(QuickError::kNotSetUp));
    return;
  }
  Pending& pending = *it->second;
  const std::string model = prefs_->GetString(prefs::kFastModel);
  base::DictValue body = BuildQuickRequest(
      pending.request, model.empty() ? kDefaultFastModel : model,
      CacheLevelFor(*prefs_, endpoint.base_url), UsesOAuthToken(endpoint));
  pending.client = std::make_unique<ClaudeClient>(get_loader_factory_.Run(),
                                                  std::move(endpoint));
  pending.client->StreamMessage(
      std::move(body), /*betas=*/{},
      base::BindRepeating(&ClaudeQuick::OnEvent, weak_factory_.GetWeakPtr(),
                          id),
      base::BindOnce(&ClaudeQuick::OnDone, weak_factory_.GetWeakPtr(), id));
}

void ClaudeQuick::OnEvent(int id, const SseParser::Event& event) {
  auto it = pending_.find(id);
  if (it == pending_.end()) {
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
  std::move(pending->callback).Run(std::move(result));
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
