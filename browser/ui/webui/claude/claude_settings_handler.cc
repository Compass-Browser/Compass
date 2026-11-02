// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/claude/claude_settings_handler.h"

#include <utility>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_paths_internal.h"
#include "compass/browser/claude/claude_client.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace compass::claude {

namespace {

mojom::AuthStyle ToMojo(AuthStyle style) {
  return style == AuthStyle::kBearer ? mojom::AuthStyle::kBearer
                                     : mojom::AuthStyle::kApiKey;
}

AuthStyle FromMojo(mojom::AuthStyle style) {
  return style == mojom::AuthStyle::kBearer ? AuthStyle::kBearer
                                            : AuthStyle::kApiKey;
}

// |text| as one shell word: quoted when it holds anything but safe
// characters ("Application Support" has a space).
std::string QuoteForShell(const std::string& text) {
  if (!text.empty() &&
      base::ContainsOnlyChars(text,
                              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRST"
                              "UVWXYZ0123456789/._-=+:,@")) {
    return text;
  }
  std::string quoted = "'";
  for (char c : text) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  return quoted + "'";
}

// "claude mcp add compass -- <the app's compass-mcp>", with the user data
// directory when it isn't the default one (development builds).
std::string McpCommand() {
  base::FilePath exe_dir;
  base::PathService::Get(base::DIR_EXE, &exe_dir);
  std::string command = base::StrCat(
      {"claude mcp add compass -- ",
       QuoteForShell(exe_dir.Append("compass-mcp").value())});
  base::FilePath user_data;
  base::FilePath default_user_data;
  base::PathService::Get(chrome::DIR_USER_DATA, &user_data);
  if (chrome::GetDefaultUserDataDirectory(&default_user_data) &&
      user_data != default_user_data) {
    base::StrAppend(&command,
                    {" ", QuoteForShell("--user-data-dir=" + user_data.value())});
  }
  return command;
}

}  // namespace

ClaudeSettingsHandler::ClaudeSettingsHandler(
    Profile* profile,
    mojo::PendingReceiver<mojom::SettingsHandler> receiver)
    : profile_(profile), receiver_(this, std::move(receiver)) {}

ClaudeSettingsHandler::~ClaudeSettingsHandler() = default;

void ClaudeSettingsHandler::SetLoaderFactoryForTesting(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  loader_factory_ = std::move(factory);
}

void ClaudeSettingsHandler::GetState(GetStateCallback callback) {
  const PrefService* prefs = profile_->GetPrefs();
  auto state = mojom::SettingsState::New();
  state->has_key = ClaudeCredentials::HasApiKey(profile_);
  state->key_hint = ClaudeCredentials::GetApiKeyHint(profile_);
  state->base_url = prefs->GetString(prefs::kBaseUrl);
  state->auth_style = ToMojo(EndpointFromPrefs(*prefs).auth_style);
  state->model = prefs->GetString(prefs::kModel);
  state->browser_scope = prefs->GetBoolean(prefs::kBrowserScope);
#if BUILDFLAG(IS_POSIX)
  state->mcp_supported = true;
#endif
  state->mcp_enabled = prefs->GetBoolean(prefs::kMcpEnabled);
  state->mcp_command = McpCommand();
  for (const base::Value& client : prefs->GetList(prefs::kMcpAllowedClients)) {
    if (client.is_string()) {
      state->mcp_allowed_clients.push_back(client.GetString());
    }
  }
  std::move(callback).Run(std::move(state));
}

void ClaudeSettingsHandler::SetApiKey(const std::string& key,
                                      SetApiKeyCallback callback) {
  ClaudeCredentials::SetApiKey(profile_, key, std::move(callback));
}

void ClaudeSettingsHandler::SetEndpoint(const std::string& base_url,
                                        mojom::AuthStyle auth_style,
                                        SetEndpointCallback callback) {
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(base_url, base::TRIM_ALL);
  std::optional<GURL> url = ParseBaseUrl(trimmed);
  if (!url) {
    std::move(callback).Run(false);
    return;
  }
  PrefService* prefs = profile_->GetPrefs();
  // The Claude API is the default, kept as no URL.
  if (trimmed.empty() || *url == GURL(kDefaultBaseUrl)) {
    prefs->ClearPref(prefs::kBaseUrl);
  } else {
    prefs->SetString(prefs::kBaseUrl, url->spec());
  }
  prefs->SetInteger(prefs::kAuthStyle,
                    static_cast<int>(FromMojo(auth_style)));
  std::move(callback).Run(true);
}

void ClaudeSettingsHandler::SetModel(const std::string& model) {
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(model, base::TRIM_ALL);
  if (trimmed.empty()) {
    profile_->GetPrefs()->ClearPref(prefs::kModel);
  } else {
    profile_->GetPrefs()->SetString(prefs::kModel, std::string(trimmed));
  }
}

void ClaudeSettingsHandler::SetBrowserScope(bool enabled) {
  profile_->GetPrefs()->SetBoolean(prefs::kBrowserScope, enabled);
}

void ClaudeSettingsHandler::SetMcpEnabled(bool enabled) {
  profile_->GetPrefs()->SetBoolean(prefs::kMcpEnabled, enabled);
}

void ClaudeSettingsHandler::ForgetMcpClient(const std::string& client) {
  ScopedListPrefUpdate update(profile_->GetPrefs(), prefs::kMcpAllowedClients);
  update->EraseValue(base::Value(client));
}

void ClaudeSettingsHandler::TestConnection(TestConnectionCallback callback) {
  ClaudeCredentials::GetApiKey(
      profile_, base::BindOnce(&ClaudeSettingsHandler::OnKeyForTest,
                               weak_factory_.GetWeakPtr(), std::move(callback)));
}

void ClaudeSettingsHandler::OnKeyForTest(TestConnectionCallback callback,
                                         std::optional<std::string> key) {
  auto result = mojom::ConnectionResult::New();
  if (!key) {
    result->error = ClaudeCredentials::HasApiKey(profile_)
                        ? "The saved key can't be read any more. Enter it "
                          "again."
                        : "Enter an API key first.";
    std::move(callback).Run(std::move(result));
    return;
  }
  Endpoint endpoint = EndpointFromPrefs(*profile_->GetPrefs());
  endpoint.api_key = std::move(*key);
  scoped_refptr<network::SharedURLLoaderFactory> factory =
      loader_factory_ ? loader_factory_
                      : profile_->GetDefaultStoragePartition()
                            ->GetURLLoaderFactoryForBrowserProcess();
  client_ = std::make_unique<ClaudeClient>(std::move(factory),
                                           std::move(endpoint));
  client_->ListModels(base::BindOnce(&ClaudeSettingsHandler::OnModels,
                                     weak_factory_.GetWeakPtr(),
                                     std::move(callback)));
}

void ClaudeSettingsHandler::OnModels(
    TestConnectionCallback callback,
    base::expected<std::vector<ModelInfo>, std::string> models) {
  auto result = mojom::ConnectionResult::New();
  if (!models.has_value()) {
    result->error = models.error();
  } else {
    result->ok = true;
    for (const ModelInfo& model : *models) {
      result->models.push_back(
          mojom::Model::New(model.id, model.display_name));
    }
  }
  std::move(callback).Run(std::move(result));
}

}  // namespace compass::claude
