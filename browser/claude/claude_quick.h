// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_QUICK_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_QUICK_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/sse_parser.h"
#include "components/keyed_service/core/keyed_service.h"

class PrefService;
class Profile;

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace compass::claude {
class OnDeviceModel;
enum class OnDeviceError;
}  // namespace compass::claude

// Claude's small requests for the sidebar: names for link folders and Tidy
// Today now, short titles later. See
// docs/design/ai-sidebar.md, "Shared core".
namespace compass::claude {

// The model a feature asks when its own setting names none (each feature's
// model pref defaults to it, e.g. prefs::kNameLinkFoldersModel).
inline constexpr char kDefaultQuickModel[] = "claude-haiku-5-5";

// A name Claude gives (a folder's): at most this many characters and words.
inline constexpr size_t kMaxAiNameLength = 32;
inline constexpr size_t kMaxAiNameWords = 3;

// |name| as the sidebar may show it: trimmed (also of quotes around it and a
// full stop at the end), its spaces collapsed, 1 to kMaxAiNameWords words of
// at most kMaxAiNameLength characters, with no emoji, control characters,
// line breaks or markup. Null if it can't be one.
std::optional<std::u16string> SanitizeAiName(std::string_view name);

// Tidy Today (docs/design/ai-sidebar.md): the most folders one reply makes,
// and the fewest entries a folder takes.
inline constexpr size_t kMaxTidyFolders = 6;
inline constexpr size_t kMinTidyFolderEntries = 2;

// A folder Tidy Today's reply asks for: its name (as SanitizeAiName() left
// it) and its entries' ids, in the reply's order.
struct TidyFolder {
  TidyFolder();
  TidyFolder(TidyFolder&&);
  TidyFolder& operator=(TidyFolder&&);
  ~TidyFolder();

  std::u16string name;
  std::vector<std::string> entries;
};

// The folders of Tidy Today's reply, {"folders": [{"name": "…", "entries":
// ["<id>", …]}, …]}, that may be made: ids not in |ids|, or in this folder
// or an earlier one already, are dropped; then a folder whose name
// SanitizeAiName() drops, or with fewer than kMinTidyFolderEntries entries
// left, is dropped. At most the first kMaxTidyFolders that pass. Empty for
// a reply of another shape.
std::vector<TidyFolder> ParseTidyReply(const base::DictValue& reply,
                                       const std::vector<std::string>& ids);

// Shorter tab titles (docs/design/ai-sidebar.md, "Shorter tab titles"): a
// title longer than this many characters gets a short one, and so does one
// with a site suffix (NeedsShortTitle()).
inline constexpr size_t kLongTitleLength = 40;
// The longest short title, in characters.
inline constexpr size_t kMaxShortTitleLength = 40;
// The longest site suffix NeedsShortTitle() takes for one (" · GitHub").
inline constexpr size_t kMaxSiteSuffixLength = 30;

// Whether |title| is worth a short title: longer than kLongTitleLength
// characters, or ending with a site suffix (" · GitHub", " - YouTube", " |
// The Verge", " — Site", " – Site").
bool NeedsShortTitle(std::u16string_view title);

// |title| as the sidebar may show it in place of a tab's title: trimmed
// (also of quotes around it and a full stop at the end), its spaces
// collapsed, 1 to kMaxShortTitleLength characters with a letter or digit,
// and none of what SanitizeAiName() refuses (emoji, control characters, line
// breaks, markup). Any number of words: unlike a name, a title may read as
// a phrase ("PR #123: fix login"). Null if it can't be one.
std::optional<std::u16string> SanitizeShortTitle(std::string_view title);

// The short titles of Shorter Tab Titles' reply, {"titles": {"<id>": "…"}},
// by id, as SanitizeShortTitle() left them: ids not in |ids|, and titles it
// drops (or empty ones: no short title), are left out. Empty for a reply of
// another shape.
std::map<std::string, std::u16string> ParseShortTitlesReply(
    const base::DictValue& reply,
    const std::vector<std::string>& ids);

// Ask in the command bar (docs/design/ai-sidebar.md, "Ask in the command
// bar"): whether |text| reads as a question, in English or in |locale|'s
// language (Italian, French, German, Spanish, Portuguese, Dutch): two words
// or more, not a URL, and ending with a question mark (also "？" or "؟"),
// starting with "¿", or starting with a question word ("how", "what",
// "why"... ; "come", "perché"... in Italian).
bool LooksLikeQuestion(std::u16string_view text, std::string_view locale);

// "About this page": the line that names the current tab for Ask, by its
// title and host only (never its URL's path or its text); empty for none.
std::string DescribeAskPage(std::string_view page_title,
                            std::string_view page_host);

// The section |id| ("## <id>") of |prompts| (common/claude/
// sidebar_prompts.md's text), trimmed, with {locale} replaced by |locale|.
// Empty if there's no such section.
// Also {language}, by the English name of |locale|'s language ("Italian"),
// which the on-device prompts spell out.
std::string SidebarPromptFrom(std::string_view prompts,
                              std::string_view id,
                              std::string_view locale);
// The same, from the file built into the browser.
std::string SidebarPrompt(std::string_view id, std::string_view locale);

// One request: a system prompt and one user message.
struct QuickRequest {
  // The feature's own switch (a boolean pref, e.g.
  // prefs::kNameLinkFolders): nothing is sent while it's off.
  std::string feature_pref;
  // The model the feature's setting names; empty for kDefaultQuickModel.
  std::string model;
  std::string system;
  std::string user;
  // Capped at ClaudeQuick::kMaxTokens.
  int max_tokens = 100;
  // An automatic request, which counts against the hourly limit and waits
  // out the back-off. One the user asked for (Suggest a Name, Tidy) doesn't.
  bool background = true;
  // A plain-text reply streamed as it comes (Stream()), not a JSON object.
  bool stream = false;
  // The request for the on-device model (docs/design/ai-sidebar.md, "On
  // this device"), used instead while kSidebarOnDevice is on: the
  // feature's prompt id ("folder_name", "tidy_today", "short_titles"),
  // whose reply has the same shape as Claude's, and its system prompt
  // (SetOnDevicePrompt()). Empty: the feature has none.
  std::string on_device_feature;
  std::string on_device_system;
};

// Sets |request|'s on-device feature to the prompt |id| and its system
// prompt to the section "<id>_on_device", or |id|'s if there's none.
void SetOnDevicePrompt(QuickRequest& request,
                       std::string_view id,
                       std::string_view locale);

// Ask's request (docs/design/ai-sidebar.md, "Ask in the command bar"): the
// "command_bar_ask" prompt section, one user message of |page_line|
// (DescribeAskPage(), may be empty) and |question|, |model| (empty for
// kDefaultQuickModel), streamed, not background.
QuickRequest MakeAskRequest(std::string_view question,
                            std::string_view page_line,
                            std::string_view model,
                            std::string_view locale);

// The Messages API body of |request|: |model|, no tools, no thinking, the
// system prompt with the cache breakpoints |cache_level| allows (and Claude
// Code's line first for a subscription token, as the chat does).
base::DictValue BuildQuickRequest(const QuickRequest& request,
                                  const std::string& model,
                                  CacheLevel cache_level,
                                  bool oauth_token);

// A reply's text as the one JSON object it must be (whitespace around it
// only). Null for anything else.
std::optional<base::DictValue> ParseQuickReply(std::string_view text);

enum class QuickError {
  // The feature's switch is off.
  kOff,
  // No key saved (or a subscription token whose risks aren't accepted).
  kNotSetUp,
  // The hour's background requests are used up.
  kRateLimited,
  // Waiting after an error.
  kBackingOff,
  // The endpoint answered with an error, or couldn't be reached.
  kFailed,
  // No answer in ClaudeQuick::kTimeout.
  kTimeout,
  // Claude declined (stop_reason "refusal").
  kRefused,
  // The answer isn't one JSON object.
  kBadReply,
};

// What a QuickError means for the user, as a sentence ("Claude took too
// long.").
std::string DescribeQuickError(QuickError error);

// A profile's short requests to Claude, through the chat's endpoint, key,
// auth style and cache level (claude_api.h), on the feature's model: one
// system prompt and one user message, no tools, at most kMaxTokens, a JSON
// object back within kTimeout. Never shows UI. Background requests are
// limited to kMaxBackgroundPerHour an hour, and an error stops them for
// kFirstBackOff, a second one in a row for kLaterBackOff. Null off the
// record and for guests: incognito never asks.
//
// With kSidebarOnDevice on (and an OnDeviceModel), a request that has an
// on-device feature asks Apple Intelligence instead: no key, Claude Code
// mode too, nothing leaves the Mac, at most kMaxOnDeviceBackgroundPerHour
// background requests an hour (for the battery).
class ClaudeQuick : public KeyedService {
 public:
  // Enough for Tidy Today's reply, which lists its folders' entries.
  static constexpr int kMaxTokens = 400;
  static constexpr base::TimeDelta kTimeout = base::Seconds(15);
  // A streamed answer's whole time (Stream()).
  static constexpr base::TimeDelta kStreamTimeout = base::Seconds(60);
  static constexpr size_t kMaxBackgroundPerHour = 30;
  static constexpr size_t kMaxOnDeviceBackgroundPerHour = 120;
  static constexpr base::TimeDelta kFirstBackOff = base::Minutes(1);
  static constexpr base::TimeDelta kLaterBackOff = base::Minutes(10);

  using Result = base::expected<base::DictValue, QuickError>;
  using Callback = base::OnceCallback<void(Result)>;
  // A streamed answer's text so far, each time more comes.
  using TextCallback = base::RepeatingCallback<void(const std::string&)>;
  using KeyCallback = base::OnceCallback<void(std::optional<std::string>)>;
  // Reads the saved key (ClaudeCredentials::GetApiKey()).
  using GetKey = base::RepeatingCallback<void(KeyCallback)>;
  // The profile's URL loader factory, asked for each request.
  using GetLoaderFactory = base::RepeatingCallback<
      scoped_refptr<network::SharedURLLoaderFactory>()>;

  // |on_device| may be null (no Apple Intelligence); it outlives this.
  ClaudeQuick(PrefService* prefs,
              GetLoaderFactory get_loader_factory,
              GetKey get_key,
              OnDeviceModel* on_device = nullptr);
  ClaudeQuick(const ClaudeQuick&) = delete;
  ClaudeQuick& operator=(const ClaudeQuick&) = delete;
  ~ClaudeQuick() override;

  static ClaudeQuick* Get(Profile* profile);

  // Whether a key is saved: the Claude API's, or a gateway's. (Claude Code
  // mode alone doesn't answer these requests.)
  bool IsSetUp() const;
  // Whether |feature_pref|'s requests go to the on-device model:
  // kSidebarOnDevice is on, there's a model, and the feature is one it
  // answers (Name Link Folders, Tidy Today, Shorter Tab Titles).
  bool UsesOnDevice(std::string_view feature_pref) const;
  // The feature's switch |feature_pref| is on, and its requests can go:
  // IsSetUp(), or UsesOnDevice() and the model isn't known to be
  // unavailable.
  bool IsEnabled(std::string_view feature_pref) const;

  // Sends |request|; |callback| gets the reply's JSON object or why not,
  // never before Ask() returns. Requests run side by side.
  void Ask(QuickRequest request, Callback callback);
  // Sends |request| for a plain-text answer, streamed: |on_text| gets the
  // text so far as it grows, then |callback| gets {"text": <all of it>} or
  // why not (kRefused, kFailed, kTimeout after kStreamTimeout...). Returns
  // an id for Cancel().
  int Stream(QuickRequest request, TextCallback on_text, Callback callback);
  // Drops request |id|: no callback runs.
  void Cancel(int id);

  // Background requests sent in the last hour.
  size_t RecentBackgroundRequests();

 private:
  struct Pending;

  // Ask() and Stream(): starts request |id|, now in pending_.
  void Start(int id);

  void OnKey(int id, std::optional<std::string> key);
  void OnEvent(int id, const SseParser::Event& event);
  void OnDone(int id, std::optional<std::string> error);
  void OnTimeout(int id);
  // The on-device model's reply to request |id|.
  void OnDeviceReply(int id, base::expected<std::string, OnDeviceError> reply);
  // Ends request |id| with |result|, later (never inside its client).
  void Finish(int id, Result result);
  void RunFinished(int id, Result result);
  void PruneRecent(base::TimeTicks now);

  const raw_ptr<PrefService> prefs_;
  const GetLoaderFactory get_loader_factory_;
  const GetKey get_key_;
  const raw_ptr<OnDeviceModel> on_device_;
  int next_id_ = 1;
  std::map<int, std::unique_ptr<Pending>> pending_;
  // When the last hour's background requests went.
  base::circular_deque<base::TimeTicks> recent_;
  // Errors in a row, and until when background requests wait.
  int failures_ = 0;
  base::TimeTicks backing_off_until_;
  base::WeakPtrFactory<ClaudeQuick> weak_factory_{this};
};

class ClaudeQuickFactory : public ProfileKeyedServiceFactory {
 public:
  static ClaudeQuickFactory* GetInstance();
  static ClaudeQuick* GetForProfile(Profile* profile);

 private:
  friend base::NoDestructor<ClaudeQuickFactory>;

  ClaudeQuickFactory();
  ~ClaudeQuickFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_QUICK_H_
