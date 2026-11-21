// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_PREFS_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_PREFS_H_

class PrefRegistrySimple;
class PrefService;

// The Claude settings of a profile (docs/design/claude.md, "Settings"). None
// of them sync: a key belongs to the device it was typed on. (The memories
// themselves sync, with the spaces.)
namespace compass::claude::prefs {

// Who answers, and the per-provider names of the settings below (the key, its
// hint, the base URL and the models): ai_provider.h, prefs::kProvider,
// ProviderPref(). Read those through CurrentPref().
//
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

// Files (docs/design/claude.md, "Files"): FilesAccess's value. Off, the file
// tools refuse.
inline constexpr char kFilesAccess[] = "compass.claude.files_access";
// The folder for "Save files" (empty: Downloads) and "Use a folder" (empty:
// none chosen, as off), as a UTF-8 path.
inline constexpr char kFilesFolder[] = "compass.claude.files_folder";
// Claude Code mode with "Use a folder": Claude Code's own tools (files,
// shell, web) in the folder, each change asking the user.
inline constexpr char kClaudeCodeOwnTools[] =
    "compass.claude.claude_code_own_tools";

// "Let Claude use developer tools" (docs/design/claude.md, "Developer
// tools"): the devtools tools (network, styles, storage, performance,
// emulation) on the tab Claude acts on. Off by default; doesn't sync.
inline constexpr char kDevTools[] = "compass.claude.devtools";
// "Let Claude make Boosts" (docs/design/boosts.md): the Boost tools
// (list_boosts, save_boost, delete_boost). Off by default; doesn't sync.
// Boosts the user makes by hand don't depend on it.
inline constexpr char kBoosts[] = "compass.claude.boosts";
// The endpoint (its base URL) that refused the usual cache breakpoints (a
// gateway adding its own), and the CacheLevel that got through with it
// (CacheLevelFor()). Empty for none; another URL doesn't use it.
inline constexpr char kCacheLevelUrl[] = "compass.claude.cache_level_url";
inline constexpr char kCacheLevel[] = "compass.claude.cache_level";

// Claude in the sidebar (docs/design/ai-sidebar.md): each feature has its
// own switch, off by default, and asks Claude (ClaudeQuick) only while it's
// on. "Name Link Folders": Claude names link folders, and folder menus offer
// "Suggest a Name".
inline constexpr char kNameLinkFolders[] = "compass.claude.name_link_folders";
// The model "Name Link Folders" asks; each feature has its own (the chat
// keeps kModel).
inline constexpr char kNameLinkFoldersModel[] =
    "compass.claude.name_link_folders_model";
// "Tidy Today": Today's header and the command bar offer "Tidy", which asks
// Claude to group Today's loose entries into named folders.
inline constexpr char kTidyToday[] = "compass.claude.tidy_today";
// The model "Tidy Today" asks.
inline constexpr char kTidyTodayModel[] = "compass.claude.tidy_today_model";
// "Shorter Tab Titles": the sidebar shows Claude's short titles for long tab
// titles (display only; ShortTitler).
inline constexpr char kShortTabTitles[] = "compass.claude.short_tab_titles";
// The model "Shorter Tab Titles" asks.
inline constexpr char kShortTabTitlesModel[] =
    "compass.claude.short_tab_titles_model";
// "Answer Questions in the Command Bar": a typed line that reads as a
// question gets an "Ask Claude" row at the top of the command bar, whose
// short answer streams in under the field.
inline constexpr char kCommandBarAsk[] = "compass.claude.command_bar_ask";
// The model it asks: Claude Sonnet by default (kDefaultAskModel), quicker
// than the chat's Opus and better at answers than Haiku.
inline constexpr char kCommandBarAskModel[] =
    "compass.claude.command_bar_ask_model";
// kCommandBarAskModel's default.
inline constexpr char kDefaultAskModel[] = "claude-sonnet-5-5";

enum class FilesAccess {
  kOff = 0,
  kSave = 1,
  kFolder = 2,
};

// Called by compass::RegisterCompassProfilePrefs().
void RegisterProfilePrefs(PrefRegistrySimple* registry);

// Moves other providers' settings from the names they had before to
// ProviderPref()'s, and drops what that left in Anthropic's (see
// ProviderPref()). Called by compass::MigrateCompassProfilePrefs().
void MigrateProviderPrefs(PrefService* prefs);

}  // namespace compass::claude::prefs

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_PREFS_H_
