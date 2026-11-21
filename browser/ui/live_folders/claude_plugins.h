// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_LIVE_FOLDERS_CLAUDE_PLUGINS_H_
#define COMPASS_BROWSER_UI_LIVE_FOLDERS_CLAUDE_PLUGINS_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "compass/browser/claude/claude_tools.h"

class Profile;

namespace compass {

// Claude's plugin tools' side in the UI layer (docs/design/claude.md,
// "Plugins"): the profile's live folder service. Claude's plugins are local
// plugins, folders in <user data>/Compass Plugins named by their id
// ("claude.…"); a test runs from a scratch folder in it, .claude-test, that
// the plugin scan never reads.
class ClaudePlugins : public claude::PluginsDelegate {
 public:
  explicit ClaudePlugins(Profile* profile);
  ClaudePlugins(const ClaudePlugins&) = delete;
  ClaudePlugins& operator=(const ClaudePlugins&) = delete;
  ~ClaudePlugins() override;

  // claude::PluginsDelegate:
  base::ListValue List() override;
  void Get(const std::string& id, DictCallback callback) override;
  base::expected<base::DictValue, std::string> Check(
      const std::string& manifest_json) override;
  void Test(const std::string& manifest_json,
            const std::string& main_js,
            bool preview,
            content::WebContents* tab,
            base::DictValue config,
            bool session,
            DictCallback callback) override;
  void Save(const std::string& manifest_json,
            const std::string& main_js,
            const std::string& icon_svg,
            bool grant_session,
            ChangeCallback callback) override;
  void Delete(const std::string& id, ChangeCallback callback) override;
  void Undo(const claude::PluginChange& change,
            base::OnceCallback<void(bool)> callback) override;

 private:
  base::WeakPtr<Profile> profile_;
  base::WeakPtrFactory<ClaudePlugins> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_LIVE_FOLDERS_CLAUDE_PLUGINS_H_
