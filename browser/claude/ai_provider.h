// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_AI_PROVIDER_H_
#define COMPASS_BROWSER_CLAUDE_AI_PROVIDER_H_

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/functional/callback_forward.h"

class PrefService;
class PrefChangeRegistrar;

// Who answers the chat and the sidebar's requests (docs/design/
// ai-providers.md). Anthropic speaks the Messages API, as Compass does
// inside; OpenAI gets the Responses API, the others Chat Completions,
// translated at the edge (openai_edge.h). iOS: CompassClaude's AIProvider.
namespace compass::claude {

enum class AIProvider {
  kAnthropic = 0,
  kOpenAI = 1,
  kGemini = 2,
  // Any server that speaks OpenAI's Chat Completions, at its base URL.
  kCompatible = 3,
};

inline constexpr AIProvider kAllProviders[] = {
    AIProvider::kAnthropic, AIProvider::kOpenAI, AIProvider::kGemini,
    AIProvider::kCompatible};

// "anthropic", "openai", "gemini", "compatible": prefs::kProvider's values
// and the per-provider prefs' suffixes (iOS's raw values).
std::string_view ProviderId(AIProvider provider);
std::optional<AIProvider> ParseProvider(std::string_view id);

// "Anthropic", "OpenAI", "Google Gemini", "Other (OpenAI-compatible)".
std::string_view ProviderDisplayName(AIProvider provider);
// What the UI calls the assistant: "Claude", "GPT", "Gemini", "AI".
std::string_view AssistantName(AIProvider provider);

// Chat Completions or the Responses API, not the Messages API.
bool UsesOpenAIFormat(AIProvider provider);
// OpenAI's own models take tools only through the Responses API.
bool UsesResponsesApi(AIProvider provider);

// The base URL when the user gives none; empty for kCompatible, which
// needs one.
std::string_view DefaultBaseUrl(AIProvider provider);
// The chat's model until the user picks one; empty for kCompatible, whose
// models only its server knows.
std::string_view DefaultChatModel(AIProvider provider);
// The sidebar's small requests' model; empty is the chat's.
std::string_view DefaultQuickModel(AIProvider provider);
// Ask in the command bar's model; empty is the chat's.
std::string_view DefaultAskModel(AIProvider provider);

struct KnownModel {
  const char* id;
  const char* display_name;
};
// Offered until "Test connection" lists the key's own. Empty for Anthropic
// (the UI keeps its own list, resources/sidebar/claude_models.ts and
// ClaudeChatUi.kt) and kCompatible.
base::span<const KnownModel> KnownModels(AIProvider provider);

// What a key looks like, for the field's placeholder.
std::string_view KeyPlaceholder(AIProvider provider);
// Where what's sent goes, for the settings.
std::string_view PrivacyNote(AIProvider provider);

}  // namespace compass::claude

namespace compass::claude::prefs {

// Who answers (ProviderId()); "anthropic" by default.
inline constexpr char kProvider[] = "compass.claude.provider";

// A per-provider setting's name: Anthropic's as it always was (|base|, a
// "compass.claude." name), another's under its own
// "compass.ai_providers.<ProviderId()>." with |base|'s last part
// ("compass.ai_providers.openai.tidy_today_model"). The key, its hint, the
// base URL, the chat's model and each sidebar feature's model are per
// provider.
//
// Not "<base>.<id>": prefs are stored by dotted path, so that made
// Anthropic's string pref a dictionary of the other providers' values,
// losing Anthropic's (see MigrateProviderPrefs()).
std::string ProviderPref(std::string_view base, AIProvider provider);

// The name ProviderPref() gave before ("<base>.<id>"), kept registered only
// for MigrateProviderPrefs(). Empty for Anthropic.
std::string LegacyProviderPref(std::string_view base, AIProvider provider);

// The provider prefs::kProvider names.
AIProvider CurrentProvider(const PrefService& prefs);
// ProviderPref(|base|, CurrentProvider()).
std::string CurrentPref(const PrefService& prefs, std::string_view base);

// Watches prefs::kProvider and every provider's variant of each of |bases|
// in |registrar|, calling |callback| when any changes.
void ObserveProviderPrefs(PrefChangeRegistrar& registrar,
                          std::initializer_list<std::string_view> bases,
                          const base::RepeatingClosure& callback);

}  // namespace compass::claude::prefs

#endif  // COMPASS_BROWSER_CLAUDE_AI_PROVIDER_H_
