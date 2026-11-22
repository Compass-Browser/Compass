// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_MCP_SESSION_H_
#define COMPASS_BROWSER_UI_CLAUDE_MCP_SESSION_H_

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "compass/browser/claude/claude_tools.h"

class BrowserWindowInterface;
class Profile;

namespace compass::claude {

class McpTabs;

// The user's answer to "<client> wants to control Compass", or kNoWindow
// when the profile has no window to ask in.
enum class McpApproval { kDeny, kAllowOnce, kAlwaysAllow, kNoWindow };

// One MCP client's connection, on the UI thread: the MCP protocol over
// JSON-RPC (initialize, ping, tools/list, tools/call) for every profile that
// allows MCP clients. Tools run one at a time through its own ClaudeTools,
// on the active tab of the last active window among those profiles, until
// select_tab or open_tab picks another. Each call acts in one profile
// (McpTabs::ResolveCall()); the first call in a profile asks the user there,
// unless they always allow this client in it. See docs/design/claude.md, D6.
class McpSession {
 public:
  using SendCallback = base::RepeatingCallback<void(std::string)>;
  // The profiles that allow MCP clients now.
  using ProfilesGetter = base::RepeatingCallback<std::vector<Profile*>()>;
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

  McpSession(ProfilesGetter profiles, SendCallback send, AskCallback ask);
  // A session for the Claude Code process a chat started (McpBridge::
  // RegisterChat()): its calls run as the chat's, with no question asked,
  // since the user started it from the panel.
  McpSession(Profile* profile, SendCallback send, ChatToolRunner chat);
  McpSession(const McpSession&) = delete;
  McpSession& operator=(const McpSession&) = delete;
  ~McpSession();

  // |profile| stopped allowing MCP clients, or is going: the session stops
  // acting in it.
  void RemoveProfile(Profile* profile);

  // A JSON-RPC message from the client.
  void OnMessage(const std::string& line);

  // The protocol versions it speaks, newest first.
  static const char* const kProtocolVersions[];

 private:
  enum class Approval { kUnknown, kAllowed, kDenied };

  void Reply(const base::Value& id, base::Value result);
  void ReplyError(const base::Value& id, int code, const std::string& message);
  void Initialize(const base::Value& id, const base::DictValue& params);
  void ListTools(const base::Value& id);
  void RunNext();
  void RunChatCall();
  // The tab the page tools follow while none is picked: the active tab of
  // the front window, or the one the user was on before a question in
  // another profile came to the front.
  content::WebContents* UserTab(const std::vector<Profile*>& profiles);
  // Asks the user whether the client may act in |profile|.
  void AskIn(Profile* profile, const std::vector<Profile*>& profiles);
  void OnApproval(int generation,
                  base::WeakPtr<Profile> profile,
                  McpApproval approval);
  void OnToolDone(base::Value id, std::string name, ToolResult result);
  // A call refused before it runs.
  void Refuse(std::string message);
  // Why list_tabs leaves |profile|'s tabs out, or nullopt.
  std::optional<std::string> HiddenReason(Profile* profile);
  bool AlwaysAllowed(Profile* profile);
  // Whether the client is allowed in at least one profile: until then,
  // messages don't name profiles.
  bool AllowedSomewhere();
  // "Profile "<name>"", or "A profile" while the client is allowed nowhere.
  std::string ProfileLabel(Profile* profile);
  // "<client> to control Compass", and " in "<profile>"" when more than one
  // profile allows MCP clients and the client may know their names.
  std::string ControlIn(Profile* profile);

  // A chat's session: its profile. Null for an MCP client's.
  const raw_ptr<Profile> chat_profile_;
  ProfilesGetter profiles_;
  SendCallback send_;
  AskCallback ask_;
  ChatToolRunner chat_;
  std::string client_name_ = "An MCP client";
  // The user's answer, per profile; none yet is kUnknown.
  std::map<raw_ptr<Profile>, Approval> approvals_;
  // The user is being asked, about the first call's profile.
  bool asking_ = false;
  base::WeakPtr<Profile> asking_profile_;
  // Counts questions, so that the answer to one RemoveProfile() dropped is
  // ignored.
  int ask_generation_ = 0;
  // While a question in another profile is in front (UserTab()): the tab
  // the user was on, and the window the question came on. Compared, never
  // used, once that window is gone.
  base::WeakPtr<content::WebContents> kept_tab_;
  raw_ptr<BrowserWindowInterface, DisableDanglingPtrDetection> asked_window_ =
      nullptr;
  // Within RemoveProfile(): calls answered there don't start the next one.
  bool removing_ = false;
  // The answer for a call RemoveProfile() stops, in place of ClaudeTools'
  // "Stopped by the user".
  std::string stop_reason_;
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
  // Owned by |tools_|, which goes first; null for a chat's session.
  raw_ptr<McpTabs, DisableDanglingPtrDetection> mcp_tabs_ = nullptr;
  ClaudeTools tools_;
  base::WeakPtrFactory<McpSession> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_CLAUDE_MCP_SESSION_H_
