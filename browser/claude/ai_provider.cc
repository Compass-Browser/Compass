// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/ai_provider.h"

#include "base/functional/callback.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "ui/base/l10n/l10n_util.h"

namespace compass::claude {

namespace {

// The ids known on 2026-11-18 (docs/design/ai-providers.md, "Settings");
// Test connection's list is the truth.
constexpr KnownModel kOpenAIModels[] = {
    {"gpt-6.2-astra", "GPT-6.2 Astra"},
    {"gpt-6.2-sol", "GPT-6.2 Sol"},
    {"gpt-6.1-terra", "GPT-6.1 Terra"},
    {"gpt-6-luna", "GPT-6 Luna"},
};
constexpr KnownModel kGeminiModels[] = {
    {"gemini-2.5-pro", "Gemini 2.5 Pro"},
    {"gemini-2.5-flash", "Gemini 2.5 Flash"},
    {"gemini-2.5-flash-lite", "Gemini 2.5 Flash-Lite"},
};

}  // namespace

std::string_view ProviderId(AIProvider provider) {
  switch (provider) {
    case AIProvider::kAnthropic:
      return "anthropic";
    case AIProvider::kOpenAI:
      return "openai";
    case AIProvider::kGemini:
      return "gemini";
    case AIProvider::kCompatible:
      return "compatible";
  }
  return "anthropic";
}

std::optional<AIProvider> ParseProvider(std::string_view id) {
  for (AIProvider provider : kAllProviders) {
    if (ProviderId(provider) == id) {
      return provider;
    }
  }
  return std::nullopt;
}

std::string ProviderDisplayName(AIProvider provider) {
  switch (provider) {
    case AIProvider::kAnthropic:
      return "Anthropic";
    case AIProvider::kOpenAI:
      return "OpenAI";
    case AIProvider::kGemini:
      return "Google Gemini";
    case AIProvider::kCompatible:
      return l10n_util::GetStringUTF8(IDS_COMPASS_AI_PROVIDER_COMPATIBLE);
  }
  return "";
}

std::string_view AssistantName(AIProvider provider) {
  switch (provider) {
    case AIProvider::kAnthropic:
      return "Claude";
    case AIProvider::kOpenAI:
      return "GPT";
    case AIProvider::kGemini:
      return "Gemini";
    case AIProvider::kCompatible:
      return "AI";
  }
  return "AI";
}

std::u16string AskTitle(AIProvider provider) {
  if (provider == AIProvider::kCompatible) {
    return l10n_util::GetStringUTF16(IDS_COMPASS_COMMAND_BAR_ASK_AI);
  }
  return l10n_util::GetStringFUTF16(
      IDS_COMPASS_COMMAND_BAR_ASK_ASSISTANT,
      base::UTF8ToUTF16(AssistantName(provider)));
}

std::u16string ChatWithTitle(AIProvider provider) {
  if (provider == AIProvider::kCompatible) {
    return l10n_util::GetStringUTF16(IDS_COMPASS_COMMAND_BAR_CHAT_WITH_AI);
  }
  return l10n_util::GetStringFUTF16(
      IDS_COMPASS_COMMAND_BAR_CHAT_WITH_ASSISTANT,
      base::UTF8ToUTF16(AssistantName(provider)));
}

bool UsesOpenAIFormat(AIProvider provider) {
  return provider != AIProvider::kAnthropic;
}

bool UsesResponsesApi(AIProvider provider) {
  return provider == AIProvider::kOpenAI;
}

std::string_view DefaultBaseUrl(AIProvider provider) {
  switch (provider) {
    case AIProvider::kAnthropic:
      return "https://api.anthropic.com";
    case AIProvider::kOpenAI:
      return "https://api.openai.com/v1";
    case AIProvider::kGemini:
      return "https://generativelanguage.googleapis.com/v1beta/openai";
    case AIProvider::kCompatible:
      return "";
  }
  return "";
}

std::string_view DefaultChatModel(AIProvider provider) {
  switch (provider) {
    case AIProvider::kAnthropic:
      return "claude-opus-5-5";
    case AIProvider::kOpenAI:
      return "gpt-6.1-terra";
    case AIProvider::kGemini:
      return "gemini-2.5-pro";
    case AIProvider::kCompatible:
      return "";
  }
  return "";
}

std::string_view DefaultQuickModel(AIProvider provider) {
  switch (provider) {
    case AIProvider::kAnthropic:
      return "claude-haiku-5-5";
    case AIProvider::kOpenAI:
      return "gpt-6-luna";
    case AIProvider::kGemini:
      return "gemini-2.5-flash";
    case AIProvider::kCompatible:
      return "";
  }
  return "";
}

std::string_view DefaultAskModel(AIProvider provider) {
  switch (provider) {
    case AIProvider::kAnthropic:
      return "claude-sonnet-5-5";
    case AIProvider::kOpenAI:
      return "gpt-6-luna";
    case AIProvider::kGemini:
      return "gemini-2.5-flash";
    case AIProvider::kCompatible:
      return "";
  }
  return "";
}

base::span<const KnownModel> KnownModels(AIProvider provider) {
  switch (provider) {
    case AIProvider::kOpenAI:
      return kOpenAIModels;
    case AIProvider::kGemini:
      return kGeminiModels;
    case AIProvider::kAnthropic:
    case AIProvider::kCompatible:
      return {};
  }
  return {};
}

std::string KeyPlaceholder(AIProvider provider) {
  switch (provider) {
    case AIProvider::kAnthropic:
      return "sk-ant-…";
    case AIProvider::kOpenAI:
      return "sk-…";
    case AIProvider::kGemini:
      return "AIza…";
    case AIProvider::kCompatible:
      return l10n_util::GetStringUTF8(
          IDS_COMPASS_AI_KEY_PLACEHOLDER_COMPATIBLE);
  }
  return "";
}

std::string PrivacyNote(AIProvider provider) {
  switch (provider) {
    case AIProvider::kAnthropic:
      return l10n_util::GetStringUTF8(IDS_COMPASS_AI_PRIVACY_ANTHROPIC);
    case AIProvider::kOpenAI:
      return l10n_util::GetStringUTF8(IDS_COMPASS_AI_PRIVACY_OPENAI);
    case AIProvider::kGemini:
      return l10n_util::GetStringUTF8(IDS_COMPASS_AI_PRIVACY_GEMINI);
    case AIProvider::kCompatible:
      return l10n_util::GetStringUTF8(IDS_COMPASS_AI_PRIVACY_COMPATIBLE);
  }
  return "";
}

}  // namespace compass::claude

namespace compass::claude::prefs {

std::string ProviderPref(std::string_view base, AIProvider provider) {
  if (provider == AIProvider::kAnthropic) {
    return std::string(base);
  }
  // The setting's own name, after "compass.claude.".
  const size_t dot = base.rfind('.');
  const std::string_view name =
      dot == std::string_view::npos ? base : base.substr(dot + 1);
  return base::StrCat(
      {"compass.ai_providers.", ProviderId(provider), ".", name});
}

std::string LegacyProviderPref(std::string_view base, AIProvider provider) {
  if (provider == AIProvider::kAnthropic) {
    return std::string();
  }
  return base::StrCat({base, ".", ProviderId(provider)});
}

AIProvider CurrentProvider(const PrefService& prefs) {
  return ParseProvider(prefs.GetString(kProvider))
      .value_or(AIProvider::kAnthropic);
}

std::string CurrentPref(const PrefService& prefs, std::string_view base) {
  return ProviderPref(base, CurrentProvider(prefs));
}

void ObserveProviderPrefs(PrefChangeRegistrar& registrar,
                          std::initializer_list<std::string_view> bases,
                          const base::RepeatingClosure& callback) {
  if (!registrar.IsObserved(kProvider)) {
    registrar.Add(kProvider, callback);
  }
  for (std::string_view base : bases) {
    for (AIProvider provider : kAllProviders) {
      const std::string name = ProviderPref(base, provider);
      if (!registrar.IsObserved(name)) {
        registrar.Add(name, callback);
      }
    }
  }
}

}  // namespace compass::claude::prefs
