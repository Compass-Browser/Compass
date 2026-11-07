// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_MCP_SESSION_H_
#define COMPASS_BROWSER_UI_CLAUDE_MCP_SESSION_H_

#include <deque>
#include <string>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "compass/browser/claude/claude_tools.h"

class Profile;

namespace compass::claude {

// The user's answer to "<client> wants to control Compass".
enum class McpApproval { kDeny, kAllowOnce, kAlwaysAllow };

// One MCP client's connection, on the UI thread: the MCP protocol over
// JSON-RPC (initialize, ping, tools/list, tools/call) for one profile.
// Tools run one at a time through its own ClaudeTools, on the active tab of
// the profile's last active window, until select_tab or open_tab picks
// another. The first tools/call asks the user, unless they always allow
// this client. See docs/design/claude.md, D6.
class McpSession {
 public:
  using SendCallback = base::RepeatingCallback<void(std::string)>;
  using AskCallback =
      base::RepeatingCallback<void(Profile*,
                                   const std::string& client,
                                   base::OnceCallback<void(McpApproval)>)>;

  // Runs a tool for a chat of the Claude panel (Claude Code mode), on that
  // chat's own ClaudeTools.
  using ChatToolRunner = base::RepeatingCallback<
      void(const std::string& name,
           base::DictValue arguments,
           base::OnceCallback<void(ToolResult)> done)>;

  McpSession(Profile* profile, SendCallback send, AskCallback ask);
  // A session for the Claude Code process a chat started (McpBridge::
  // RegisterChat()): its calls run as the chat's, with no question asked,
  // since the user started it from the panel.
  McpSession(Profile* profile, SendCallback send, ChatToolRunner chat);
  McpSession(const McpSession&) = delete;
  McpSession& operator=(const McpSession&) = delete;
  ~McpSession();

  Profile* profile() { return profile_; }

  // A JSON-RPC message from the client.
  void OnMessage(const std::string& line);

  // The protocol versions it speaks, newest first.
  static const char* const kProtocolVersions[];

 private:
  enum class Approval { kUnknown, kAsking, kAllowed, kDenied };

  void Reply(const base::Value& id, base::Value result);
  void ReplyError(const base::Value& id, int code, const std::string& message);
  void Initialize(const base::Value& id, const base::DictValue& params);
  void ListTools(const base::Value& id);
  void RunNext();
  void OnApproval(McpApproval approval);
  void OnToolDone(base::Value id, std::string name, ToolResult result);

  const raw_ptr<Profile> profile_;
  SendCallback send_;
  AskCallback ask_;
  ChatToolRunner chat_;
  std::string client_name_ = "An MCP client";
  Approval approval_ = Approval::kUnknown;
  // tools/call requests waiting their turn: id, name, arguments.
  struct Call {
    base::Value id;
    std::string name;
    base::DictValue arguments;
  };
  std::deque<Call> calls_;
  bool running_ = false;
  // select_tab or open_tab picked the tab; until then, the user's active
  // tab, at each call.
  bool picked_tab_ = false;
  ClaudeTools tools_;
  base::WeakPtrFactory<McpSession> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_CLAUDE_MCP_SESSION_H_
