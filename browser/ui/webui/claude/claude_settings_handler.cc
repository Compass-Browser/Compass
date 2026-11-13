// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/claude/claude_settings_handler.h"

#include <utility>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/task/thread_pool.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_paths_internal.h"
#include "compass/browser/claude/claude_client.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_memory.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/ui/claude/claude_files_config.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "ui/shell_dialogs/selected_file_info.h"

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

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)
// compass-mcp talks to the browser over a Unix socket, or a named pipe on
// Windows (the page says MCP isn't supported elsewhere).

#if BUILDFLAG(IS_WIN)
constexpr char kMcpRelayName[] = "compass-mcp.exe";

// |text| as one word for cmd and PowerShell alike: in double quotes when it
// holds a space or a quote character (paths don't hold '"').
std::string QuoteForShell(const std::string& text) {
  if (!text.empty() && text.find_first_of(" \t'&()^%!;,`$\"") ==
                           std::string::npos) {
    return text;
  }
  return "\"" + text + "\"";
}
#else
constexpr char kMcpRelayName[] = "compass-mcp";

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
#endif

// "claude mcp add compass -- <the app's compass-mcp>", with the user data
// directory when it isn't the default one (development builds).
std::string McpCommand() {
  base::FilePath exe_dir;
  base::PathService::Get(base::DIR_EXE, &exe_dir);
  std::string command = base::StrCat(
      {"claude mcp add compass -- ",
       QuoteForShell(exe_dir.AppendASCII(kMcpRelayName).AsUTF8Unsafe())});
  base::FilePath user_data;
  base::FilePath default_user_data;
  base::PathService::Get(chrome::DIR_USER_DATA, &user_data);
  if (chrome::GetDefaultUserDataDirectory(&default_user_data) &&
      user_data != default_user_data) {
    base::StrAppend(&command,
                    {" ", QuoteForShell("--user-data-dir=" +
                                        user_data.AsUTF8Unsafe())});
  }
  return command;
}
#endif  // BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)

}  // namespace

ClaudeSettingsHandler::ClaudeSettingsHandler(
    Profile* profile,
    mojo::PendingReceiver<mojom::SettingsHandler> receiver)
    : profile_(profile), receiver_(this, std::move(receiver)) {}

ClaudeSettingsHandler::~ClaudeSettingsHandler() {
  if (folder_dialog_) {
    folder_dialog_->ListenerDestroyed();
  }
}

void ClaudeSettingsHandler::SetWebContents(content::WebContents* web_contents) {
  web_contents_ = web_contents ? web_contents->GetWeakPtr() : nullptr;
}

void ClaudeSettingsHandler::SetLoaderFactoryForTesting(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  loader_factory_ = std::move(factory);
}

void ClaudeSettingsHandler::GetState(GetStateCallback callback) {
  const PrefService* prefs = profile_->GetPrefs();
  auto state = mojom::SettingsState::New();
  state->backend = prefs->GetInteger(prefs::kBackend) ==
                           static_cast<int>(Backend::kClaudeCode)
                       ? mojom::Backend::kClaudeCode
                       : mojom::Backend::kApiKey;
  state->claude_code_path = prefs->GetString(prefs::kClaudeCodePath);
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)
  state->claude_code_tools = true;
#endif
  state->has_key = ClaudeCredentials::HasApiKey(profile_);
  state->key_hint = ClaudeCredentials::GetApiKeyHint(profile_);
  state->base_url = prefs->GetString(prefs::kBaseUrl);
  state->auth_style = ToMojo(EndpointFromPrefs(*prefs).auth_style);
  state->model = prefs->GetString(prefs::kModel);
  state->browser_scope = prefs->GetBoolean(prefs::kBrowserScope);
  state->accept_token_risks = prefs->GetBoolean(prefs::kAcceptTokenRisks);
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)
  state->mcp_supported = true;
  state->mcp_command = McpCommand();
#endif
  state->mcp_enabled = prefs->GetBoolean(prefs::kMcpEnabled);
  state->memory_supported = ClaudeMemory::Get(profile_) != nullptr;
  state->memory_enabled = prefs->GetBoolean(prefs::kMemoryEnabled);
  state->memory_save = prefs->GetBoolean(prefs::kMemorySave);
  switch (GetFilesConfig(profile_).access) {
    case prefs::FilesAccess::kOff:
      state->files_access = mojom::FilesAccess::kOff;
      break;
    case prefs::FilesAccess::kSave:
      state->files_access = mojom::FilesAccess::kSave;
      break;
    case prefs::FilesAccess::kFolder:
      state->files_access = mojom::FilesAccess::kFolder;
      break;
  }
  state->files_folder = prefs->GetString(prefs::kFilesFolder);
  state->default_save_folder = DefaultSaveFolder(profile_).AsUTF8Unsafe();
  state->claude_code_own_tools = prefs->GetBoolean(prefs::kClaudeCodeOwnTools);
  state->devtools = prefs->GetBoolean(prefs::kDevTools);
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

void ClaudeSettingsHandler::SetBackend(mojom::Backend backend) {
  profile_->GetPrefs()->SetInteger(
      prefs::kBackend, static_cast<int>(backend == mojom::Backend::kClaudeCode
                                            ? Backend::kClaudeCode
                                            : Backend::kApiKey));
}

void ClaudeSettingsHandler::SetClaudeCodePath(const std::string& path) {
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(path, base::TRIM_ALL);
  if (trimmed.empty()) {
    profile_->GetPrefs()->ClearPref(prefs::kClaudeCodePath);
  } else {
    profile_->GetPrefs()->SetString(prefs::kClaudeCodePath,
                                    std::string(trimmed));
  }
}

void ClaudeSettingsHandler::SetBrowserScope(bool enabled) {
  profile_->GetPrefs()->SetBoolean(prefs::kBrowserScope, enabled);
}

void ClaudeSettingsHandler::SetAcceptTokenRisks(bool accepted) {
  profile_->GetPrefs()->SetBoolean(prefs::kAcceptTokenRisks, accepted);
}

void ClaudeSettingsHandler::SetMcpEnabled(bool enabled) {
  profile_->GetPrefs()->SetBoolean(prefs::kMcpEnabled, enabled);
}

void ClaudeSettingsHandler::ForgetMcpClient(const std::string& client) {
  ScopedListPrefUpdate update(profile_->GetPrefs(), prefs::kMcpAllowedClients);
  update->EraseValue(base::Value(client));
}

void ClaudeSettingsHandler::SetMemoryEnabled(bool enabled) {
  profile_->GetPrefs()->SetBoolean(prefs::kMemoryEnabled, enabled);
}

void ClaudeSettingsHandler::SetMemorySave(bool enabled) {
  profile_->GetPrefs()->SetBoolean(prefs::kMemorySave, enabled);
}

void ClaudeSettingsHandler::SetDevTools(bool enabled) {
  profile_->GetPrefs()->SetBoolean(prefs::kDevTools, enabled);
}

void ClaudeSettingsHandler::SetFilesAccess(mojom::FilesAccess access) {
  prefs::FilesAccess value = prefs::FilesAccess::kOff;
  if (access == mojom::FilesAccess::kSave) {
    value = prefs::FilesAccess::kSave;
  } else if (access == mojom::FilesAccess::kFolder) {
    value = prefs::FilesAccess::kFolder;
  }
  profile_->GetPrefs()->SetInteger(prefs::kFilesAccess,
                                   static_cast<int>(value));
}

void ClaudeSettingsHandler::ChooseFilesFolder(
    ChooseFilesFolderCallback callback) {
  if (folder_dialog_ || !web_contents_) {
    // One at a time; none without the page's window.
    std::move(callback).Run(std::string());
    return;
  }
  folder_callback_ = std::move(callback);
  folder_dialog_ = ui::SelectFileDialog::Create(
      this, std::make_unique<ChromeSelectFilePolicy>(web_contents_.get()));
  const FilesConfig config = GetFilesConfig(profile_);
  folder_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_FOLDER, u"Choose a folder for Claude",
      config.folder.empty() ? DefaultSaveFolder(profile_) : config.folder,
      /*file_types=*/nullptr, /*file_type_index=*/0,
      /*default_extension=*/base::FilePath::StringType(),
      web_contents_->GetTopLevelNativeWindow());
}

void ClaudeSettingsHandler::FileSelected(const ui::SelectedFileInfo& file,
                                         int index) {
  folder_dialog_.reset();
  const std::string folder = file.path().AsUTF8Unsafe();
  profile_->GetPrefs()->SetString(prefs::kFilesFolder, folder);
  if (folder_callback_) {
    std::move(folder_callback_).Run(folder);
  }
}

void ClaudeSettingsHandler::FileSelectionCanceled() {
  folder_dialog_.reset();
  if (folder_callback_) {
    std::move(folder_callback_).Run(std::string());
  }
}

void ClaudeSettingsHandler::ClearFilesFolder() {
  profile_->GetPrefs()->ClearPref(prefs::kFilesFolder);
}

void ClaudeSettingsHandler::SetClaudeCodeOwnTools(bool enabled) {
  profile_->GetPrefs()->SetBoolean(prefs::kClaudeCodeOwnTools, enabled);
}

void ClaudeSettingsHandler::GetMemories(GetMemoriesCallback callback) {
  ClaudeMemory* memory = ClaudeMemory::Get(profile_);
  if (!memory) {
    std::move(callback).Run({});
    return;
  }
  memory->WhenLoaded(base::BindOnce(
      [](base::WeakPtr<ClaudeMemory> memory, GetMemoriesCallback callback) {
        std::vector<mojom::MemoryPtr> memories;
        if (memory) {
          for (const MemoryEntry& entry : memory->entries()) {
            memories.push_back(mojom::Memory::New(entry.id, entry.text));
          }
        }
        std::move(callback).Run(std::move(memories));
      },
      memory->GetWeakPtr(), std::move(callback)));
}

void ClaudeSettingsHandler::AddMemory(const std::string& text,
                                      AddMemoryCallback callback) {
  ClaudeMemory* memory = ClaudeMemory::Get(profile_);
  if (!memory) {
    std::move(callback).Run("Memory isn't available in this profile.");
    return;
  }
  base::expected<MemoryEntry, std::string> added = memory->Add(text);
  std::move(callback).Run(added.has_value() ? std::string() : added.error());
}

void ClaudeSettingsHandler::UpdateMemory(const std::string& id,
                                         const std::string& text,
                                         UpdateMemoryCallback callback) {
  ClaudeMemory* memory = ClaudeMemory::Get(profile_);
  if (!memory) {
    std::move(callback).Run("Memory isn't available in this profile.");
    return;
  }
  base::expected<MemoryEntry, std::string> updated = memory->Update(id, text);
  std::move(callback).Run(updated.has_value() ? std::string()
                                              : updated.error());
}

void ClaudeSettingsHandler::RemoveMemory(const std::string& id) {
  if (ClaudeMemory* memory = ClaudeMemory::Get(profile_)) {
    memory->Remove(id);
  }
}

void ClaudeSettingsHandler::RemoveAllMemories() {
  if (ClaudeMemory* memory = ClaudeMemory::Get(profile_)) {
    memory->RemoveAll();
  }
}

void ClaudeSettingsHandler::TestConnection(TestConnectionCallback callback) {
  const PrefService* prefs = profile_->GetPrefs();
  if (prefs->GetInteger(prefs::kBackend) ==
      static_cast<int>(Backend::kClaudeCode)) {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](base::FilePath configured) -> std::optional<ClaudeCodeStatus> {
              std::optional<base::FilePath> program =
                  FindClaudeCode(configured);
              if (!program) {
                return std::nullopt;
              }
              return CheckClaudeCode(*program);
            },
            base::FilePath::FromUTF8Unsafe(
                prefs->GetString(prefs::kClaudeCodePath))),
        base::BindOnce(&ClaudeSettingsHandler::OnClaudeCodeChecked,
                       weak_factory_.GetWeakPtr(), std::move(callback)));
    return;
  }
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
  if (NeedsTokenRisksAccepted(endpoint)) {
    result->error = kTokenRisksMessage;
    std::move(callback).Run(std::move(result));
    return;
  }
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

void ClaudeSettingsHandler::OnClaudeCodeChecked(
    TestConnectionCallback callback,
    std::optional<ClaudeCodeStatus> status) {
  auto result = mojom::ConnectionResult::New();
  if (!status) {
    result->error =
        profile_->GetPrefs()->GetString(prefs::kClaudeCodePath).empty()
            ? "Claude Code isn't installed where Compass looks. Install it "
              "(claude.com/claude-code), or set its path."
            : "There's no Claude Code at that path.";
  } else if (!status->ok) {
    result->error = std::move(status->text);
  } else {
    result->ok = true;
    result->detail = std::move(status->text);
  }
  std::move(callback).Run(std::move(result));
}

}  // namespace compass::claude
