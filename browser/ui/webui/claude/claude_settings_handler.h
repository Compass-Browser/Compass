// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_SETTINGS_HANDLER_H_
#define COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_SETTINGS_HANDLER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_code.h"
#include "compass/browser/ui/webui/claude/claude.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/shell_dialogs/select_file_dialog.h"

class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace compass::claude {

class ClaudeClient;

// The browser side of compass://settings/claude: reads and writes the
// profile's Claude prefs, saves the key encrypted (ClaudeCredentials) and
// tests the connection. See docs/design/claude.md, "Settings".
class ClaudeSettingsHandler : public mojom::SettingsHandler,
                              public ui::SelectFileDialog::Listener {
 public:
  ClaudeSettingsHandler(Profile* profile,
                        mojo::PendingReceiver<mojom::SettingsHandler> receiver);
  ClaudeSettingsHandler(const ClaudeSettingsHandler&) = delete;
  ClaudeSettingsHandler& operator=(const ClaudeSettingsHandler&) = delete;
  ~ClaudeSettingsHandler() override;

  // The settings page's tab: the folder picker's window.
  void SetWebContents(content::WebContents* web_contents);

  // Where TestConnection() sends requests; the profile's network by default.
  void SetLoaderFactoryForTesting(
      scoped_refptr<network::SharedURLLoaderFactory> factory);

  // mojom::SettingsHandler:
  void GetState(GetStateCallback callback) override;
  void SetApiKey(const std::string& key, SetApiKeyCallback callback) override;
  void SetEndpoint(const std::string& base_url,
                   mojom::AuthStyle auth_style,
                   SetEndpointCallback callback) override;
  void SetModel(const std::string& model) override;
  void SetBackend(mojom::Backend backend) override;
  void SetClaudeCodePath(const std::string& path) override;
  void SetBrowserScope(bool enabled) override;
  void SetAcceptTokenRisks(bool accepted) override;
  void SetMcpEnabled(bool enabled) override;
  void ForgetMcpClient(const std::string& client) override;
  void SetMemoryEnabled(bool enabled) override;
  void SetMemorySave(bool enabled) override;
  void SetDevTools(bool enabled) override;
  void GetMemories(GetMemoriesCallback callback) override;
  void AddMemory(const std::string& text, AddMemoryCallback callback) override;
  void UpdateMemory(const std::string& id,
                    const std::string& text,
                    UpdateMemoryCallback callback) override;
  void RemoveMemory(const std::string& id) override;
  void RemoveAllMemories() override;
  void TestConnection(TestConnectionCallback callback) override;
  void SetFilesAccess(mojom::FilesAccess access) override;
  void ChooseFilesFolder(ChooseFilesFolderCallback callback) override;
  void ClearFilesFolder() override;
  void SetClaudeCodeOwnTools(bool enabled) override;

  // ui::SelectFileDialog::Listener:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void FileSelectionCanceled() override;

 private:
  void OnKeyForTest(TestConnectionCallback callback,
                    std::optional<std::string> key);
  void OnModels(TestConnectionCallback callback,
                base::expected<std::vector<ModelInfo>, std::string> models);
  void OnClaudeCodeChecked(TestConnectionCallback callback,
                           std::optional<ClaudeCodeStatus> status);

  const raw_ptr<Profile> profile_;
  base::WeakPtr<content::WebContents> web_contents_;
  scoped_refptr<ui::SelectFileDialog> folder_dialog_;
  ChooseFilesFolderCallback folder_callback_;
  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  std::unique_ptr<ClaudeClient> client_;
  mojo::Receiver<mojom::SettingsHandler> receiver_;
  base::WeakPtrFactory<ClaudeSettingsHandler> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_SETTINGS_HANDLER_H_
