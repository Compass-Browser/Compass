// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_prefs.h"

#include <string>

#include "compass/browser/claude/ai_provider.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace compass::claude::prefs {

namespace {

// The settings each provider has its own of (ProviderPref()).
constexpr const char* kPerProvider[] = {
    kApiKey,
    kApiKeyHint,
    kBaseUrl,
    kModel,
    kNameLinkFoldersModel,
    kTidyTodayModel,
    kShortTabTitlesModel,
    kCommandBarAskModel,
};

}  // namespace

void RegisterProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterStringPref(kApiKey, std::string());
  registry->RegisterStringPref(kApiKeyHint, std::string());
  registry->RegisterStringPref(kBaseUrl, std::string());
  registry->RegisterIntegerPref(kAuthStyle, 0);
  registry->RegisterIntegerPref(kBackend, 0);
  registry->RegisterStringPref(kClaudeCodePath, std::string());
  // claude_api.h's kDefaultModel; this target stays free of the client's
  // dependencies, as upstream's prefs registration reaches it.
  registry->RegisterStringPref(kModel, "claude-opus-5-5");
  registry->RegisterBooleanPref(kBrowserScope, false);
  registry->RegisterBooleanPref(kAcceptTokenRisks, false);
  registry->RegisterBooleanPref(kMcpEnabled, false);
  registry->RegisterListPref(kMcpAllowedClients);
  registry->RegisterBooleanPref(kMemoryEnabled, true);
  registry->RegisterBooleanPref(kMemorySave, true);
  registry->RegisterIntegerPref(kFilesAccess,
                                static_cast<int>(FilesAccess::kOff));
  registry->RegisterStringPref(kFilesFolder, std::string());
  registry->RegisterBooleanPref(kClaudeCodeOwnTools, false);
  registry->RegisterBooleanPref(kDevTools, false);
  registry->RegisterBooleanPref(kBoosts, false);
  registry->RegisterStringPref(kCacheLevelUrl, std::string());
  registry->RegisterIntegerPref(kCacheLevel, 0);
  registry->RegisterBooleanPref(kNameLinkFolders, false);
  // claude_quick.h's kDefaultQuickModel.
  registry->RegisterStringPref(kNameLinkFoldersModel, "claude-haiku-5-5");
  registry->RegisterBooleanPref(kTidyToday, false);
  registry->RegisterStringPref(kTidyTodayModel, "claude-haiku-5-5");
  registry->RegisterBooleanPref(kShortTabTitles, false);
  registry->RegisterStringPref(kShortTabTitlesModel, "claude-haiku-5-5");
  registry->RegisterBooleanPref(kCommandBarAsk, false);
  registry->RegisterStringPref(kCommandBarAskModel, kDefaultAskModel);

  // Other AI providers (docs/design/ai-providers.md): Anthropic's settings
  // above keep their names; another provider's key, base URL and models
  // have its id after them (ProviderPref()).
  registry->RegisterStringPref(kProvider,
                               std::string(ProviderId(AIProvider::kAnthropic)));
  for (AIProvider provider : kAllProviders) {
    if (provider == AIProvider::kAnthropic) {
      continue;
    }
    registry->RegisterStringPref(ProviderPref(kApiKey, provider),
                                 std::string());
    registry->RegisterStringPref(ProviderPref(kApiKeyHint, provider),
                                 std::string());
    registry->RegisterStringPref(ProviderPref(kBaseUrl, provider),
                                 std::string());
    registry->RegisterStringPref(ProviderPref(kModel, provider),
                                 std::string(DefaultChatModel(provider)));
    const std::string quick(DefaultQuickModel(provider));
    for (const char* feature_model :
         {kNameLinkFoldersModel, kTidyTodayModel, kShortTabTitlesModel}) {
      registry->RegisterStringPref(ProviderPref(feature_model, provider),
                                   quick);
    }
    registry->RegisterStringPref(ProviderPref(kCommandBarAskModel, provider),
                                 std::string(DefaultAskModel(provider)));
    // The names before, only to move their values (MigrateProviderPrefs()).
    for (const char* base : kPerProvider) {
      registry->RegisterStringPref(LegacyProviderPref(base, provider),
                                   std::string());
    }
  }
}

void MigrateProviderPrefs(PrefService* prefs) {
  for (AIProvider provider : kAllProviders) {
    if (provider == AIProvider::kAnthropic) {
      continue;
    }
    for (const char* base : kPerProvider) {
      const std::string legacy = LegacyProviderPref(base, provider);
      const base::Value* value = prefs->GetUserPrefValue(legacy);
      if (!value) {
        continue;
      }
      const std::string current = ProviderPref(base, provider);
      if (prefs->FindPreference(current)->IsDefaultValue()) {
        prefs->SetString(current, value->GetString());
      }
      prefs->ClearPref(legacy);
    }
  }
  // The old names stored the other providers' values inside Anthropic's
  // setting, making it a dictionary its string pref ignores. What's left of
  // one goes: Anthropic's own value was already replaced.
  for (const char* base : kPerProvider) {
    // A user value of another type is there but read as the default (a
    // string equal to the default goes too, which changes nothing).
    if (prefs->FindPreference(base)->HasUserSetting() &&
        prefs->GetValue(base) == *prefs->GetDefaultPrefValue(base)) {
      prefs->ClearPref(base);
    }
  }
}

}  // namespace compass::claude::prefs
