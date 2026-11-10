// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_PREFS_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_PREFS_H_

class PrefRegistrySimple;

// The Claude settings of a profile (docs/design/claude.md, "Settings"). None
// of them sync: a key belongs to the device it was typed on. (The memories
// themselves sync, with the spaces.)
namespace compass::claude::prefs {

// The API key, encrypted with the OS's safe storage (os_crypt_async), in
// base64. Empty when none is set. Read and written by ClaudeCredentials only.
inline constexpr char kApiKey[] = "compass.claude.api_key";
// What settings show of the key (KeyHint()).
inline constexpr char kApiKeyHint[] = "compass.claude.api_key_hint";
// The base URL, as ParseBaseUrl() normalized it; empty for the Claude API.
inline constexpr char kBaseUrl[] = "compass.claude.base_url";
// How the key is sent (AuthStyle).
inline constexpr char kAuthStyle[] = "compass.claude.auth_style";
// What the chat runs on (claude_code.h Backend): 0, the Claude API (or the
// gateway) with the key; 1, the user's Claude Code, signed in with their
// Claude subscription (docs/design/claude.md, D8).
inline constexpr char kBackend[] = "compass.claude.backend";
// Claude Code's path; empty to look in the usual places (FindClaudeCode()).
inline constexpr char kClaudeCodePath[] = "compass.claude.claude_code_path";
// The model the chat uses.
inline constexpr char kModel[] = "compass.claude.model";
// "Let Claude use the whole browser": other tabs and windows, not only the
// current tab.
inline constexpr char kBrowserScope[] = "compass.claude.browser_scope";
// "Accept the risks of subscription tokens": a token from `claude
// setup-token` may be used as the key (IsOAuthToken()). Off, such a key is
// refused before anything is sent.
inline constexpr char kAcceptTokenRisks[] = "compass.claude.accept_token_risks";
// "Allow MCP clients": Claude Code and other MCP clients may use Claude's
// tools on this profile, through compass-mcp (docs/design/claude.md, D6).
inline constexpr char kMcpEnabled[] = "compass.claude.mcp_enabled";
// The MCP clients the user always allows, by the name they declare.
inline constexpr char kMcpAllowedClients[] =
    "compass.claude.mcp_allowed_clients";

// "Use memory": what Claude saved about the user goes into each new chat
// (docs/design/claude.md, "Memory"). Off, the memory tools aren't sent
// either; the memories stay.
inline constexpr char kMemoryEnabled[] = "compass.claude.memory_enabled";
// "Let Claude save memories": the memory tools are sent (with kMemoryEnabled).
inline constexpr char kMemorySave[] = "compass.claude.memory_save";

// Called by compass::RegisterCompassProfilePrefs().
void RegisterProfilePrefs(PrefRegistrySimple* registry);

}  // namespace compass::claude::prefs

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_PREFS_H_
