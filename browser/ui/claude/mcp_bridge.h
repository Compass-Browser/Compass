// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_MCP_BRIDGE_H_
#define COMPASS_BROWSER_UI_CLAUDE_MCP_BRIDGE_H_

#include <map>
#include <memory>
#include <set>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "compass/browser/ui/claude/mcp_session.h"

class Profile;

namespace compass::claude {

class McpListener;

// Lets MCP clients on this computer (Claude Code, Claude Desktop, …) use
// Claude's tools in Compass: they run compass-mcp, which relays MCP's stdio
// to <user data dir>/compass-mcp.sock. It listens while a profile has
// "Allow MCP clients" on (McpProfileService tells it). A connection serves
// the profile of the last active window among those, one McpSession each.
// See docs/design/claude.md, D6.
class McpBridge {
 public:
  static constexpr char kSocketName[] = "compass-mcp.sock";

  // Created on first use; lives until the browser exits.
  static McpBridge* Get();
  static McpBridge* GetIfExists();

  McpBridge(const McpBridge&) = delete;
  McpBridge& operator=(const McpBridge&) = delete;

  // |profile|'s "Allow MCP clients". Off closes its connections.
  void SetProfileEnabled(Profile* profile, bool enabled);

  bool listening() const { return listening_; }
  base::FilePath socket_path() const;

  void SetSocketPathForTesting(base::FilePath path);
  // Answers the "wants to control Compass" question instead of the user.
  void SetAskForTesting(McpSession::AskCallback ask);
  size_t session_count_for_testing() const { return sessions_.size(); }

 private:
  McpBridge();
  ~McpBridge();

  void UpdateListener();
  void OnListening(bool listening);
  void OnConnected(int id);
  void OnLine(int id, std::string line);
  void OnClosed(int id);
  void Send(int id, std::string line);
  // The profile a new connection serves, or null.
  Profile* PickProfile();
  void Ask(Profile* profile,
           const std::string& client,
           base::OnceCallback<void(McpApproval)> done);

  base::FilePath socket_path_for_testing_;
  McpSession::AskCallback ask_for_testing_;
  std::set<raw_ptr<Profile>> profiles_;
  base::SequenceBound<McpListener> listener_;
  bool listening_ = false;
  std::map<int, std::unique_ptr<McpSession>> sessions_;
  base::WeakPtrFactory<McpBridge> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_CLAUDE_MCP_BRIDGE_H_
