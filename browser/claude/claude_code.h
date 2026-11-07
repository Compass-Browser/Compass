// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_CODE_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_CODE_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"

// Claude Code mode: the chat runs on the user's own Claude Code (`claude`),
// signed in with their Claude subscription, instead of the Claude API with
// a key. Claude Code runs the conversation and its tool loop; Compass's
// tools reach it as an MCP server (compass-mcp --chat). See
// docs/design/claude.md, "Claude Code mode (D8)".
namespace compass::claude {

// The chat's backend (prefs::kBackend).
enum class Backend {
  kApiKey = 0,
  kClaudeCode = 1,
};

// Where `claude` is: |configured| if it's set (the user's path), else the
// first of the places its installers use that holds it. Blocks (file
// checks). Null if none.
std::optional<base::FilePath> FindClaudeCode(const base::FilePath& configured);

// The places FindClaudeCode() looks, in order, for this user.
std::vector<base::FilePath> ClaudeCodeCandidates();

struct ClaudeCodeOptions {
  ClaudeCodeOptions();
  ClaudeCodeOptions(const ClaudeCodeOptions&);
  ClaudeCodeOptions& operator=(const ClaudeCodeOptions&);
  ~ClaudeCodeOptions();

  std::string model;
  std::string system_prompt;
  // compass-mcp's path, its user data directory, and the chat's token
  // (McpBridge::RegisterChat()). An empty path: no tools (no MCP bridge on
  // this system).
  base::FilePath mcp_relay;
  base::FilePath user_data_dir;
  std::string chat_token;
  // A session to continue (after Stop, or another model); empty for a new
  // conversation.
  std::string resume;
};

// The arguments after the program: print mode, stream-json both ways with
// partial messages, the model, Compass's system prompt, none of Claude
// Code's own tools (files, shell, web) and none of the user's MCP servers
// or settings, Compass's MCP server allowed without asking, 40 turns at
// most.
std::vector<std::string> ClaudeCodeArgs(const ClaudeCodeOptions& options);

// The MCP config --mcp-config gets: compass-mcp with --chat.
std::string ClaudeCodeMcpConfig(const ClaudeCodeOptions& options);

// A user message for Claude Code's stdin (stream-json): |content|, the
// Messages API's content blocks, without tool_result blocks (Claude Code
// keeps its own tool results). One line, with its newline.
std::string ClaudeCodeUserLine(base::ListValue content);

// Tools come from the MCP server "compass": Claude Code names them
// mcp__compass__<name>. The panel shows Compass's names.
inline constexpr char kToolPrefix[] = "mcp__compass__";

// What a line of Claude Code's stdout says.
struct ClaudeCodeEvent {
  enum class Type {
    // Nothing for the chat (status lines, rate limits, whole messages that
    // stream events already gave).
    kIgnore,
    // system/init: the session's id.
    kInit,
    // A Messages API stream event: |name| and its JSON |data|, tool names
    // without kToolPrefix.
    kStreamEvent,
    // A tool's result, from Claude Code's user message: |tool_use_id| and
    // |data|, {"content": [blocks], "is_error": bool}.
    kToolResult,
    // The reply to the message ended: |error| empty if it completed.
    kResult,
  };

  ClaudeCodeEvent();
  ClaudeCodeEvent(ClaudeCodeEvent&&);
  ClaudeCodeEvent& operator=(ClaudeCodeEvent&&);
  ~ClaudeCodeEvent();

  Type type = Type::kIgnore;
  std::string session_id;
  std::string name;
  std::string data;
  std::string tool_use_id;
  std::string error;
};

// The events in a line of stdout (a user message can carry several tool
// results). Not JSON: none (a warning on stderr, which shares the pipe).
std::vector<ClaudeCodeEvent> ParseClaudeCodeLine(std::string_view line);

// What settings' Test connection says of Claude Code, from the output of
// `claude --version` and `claude auth status` (JSON).
struct ClaudeCodeStatus {
  bool ok = false;
  std::string text;
};
ClaudeCodeStatus DescribeClaudeCodeStatus(std::string_view version,
                                          std::string_view auth_status);

// Runs `program --version` and `program auth status` and describes them.
// Blocks.
ClaudeCodeStatus CheckClaudeCode(const base::FilePath& program);

// What a process that ended before its reply did says to the user, from
// its exit code and the last lines it wrote that weren't JSON.
std::string DescribeClaudeCodeExit(int exit_code, std::string_view output);

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_CODE_H_
