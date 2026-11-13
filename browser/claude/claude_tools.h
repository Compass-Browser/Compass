// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_TOOLS_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_TOOLS_H_

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "compass/browser/claude/claude_memory.h"
#include "compass/browser/claude/claude_prefs.h"
#include "content/public/browser/web_contents_observer.h"
#include "url/gurl.h"

namespace content {
class WebContents;
}  // namespace content

namespace compass::claude {

class DevToolsSession;

// The file tools' setting at a call (docs/design/claude.md, "Files").
struct FilesConfig {
  prefs::FilesAccess access = prefs::FilesAccess::kOff;
  // "Save files": the folder, Downloads when none is chosen. "Use a folder":
  // the folder, empty when none is chosen.
  base::FilePath folder;
};

// What a tool asks the user before it acts.
struct ApprovalRequest {
  // The tool's name: "Allow for this chat" is per tool.
  std::string tool;
  // "Save “notes.md” in Downloads".
  std::string title;
  // What it writes (the start of it), or runs.
  std::string detail;
};

// What a memory tool changed, so the chat can undo it.
struct MemoryChange {
  MemoryChange();
  MemoryChange(const MemoryChange&);
  MemoryChange& operator=(const MemoryChange&);
  ~MemoryChange();

  // "saved", "updated" or "forgot".
  std::string kind;
  std::string id;
  // The memory's text now (forgot: what it was).
  std::string text;
  // The memory before the change; none if it's new.
  std::optional<MemoryEntry> before;
};

// What a tool returns: the tool_result's content blocks (text, image).
struct ToolResult {
  ToolResult();
  ToolResult(ToolResult&&);
  ToolResult& operator=(ToolResult&&);
  ~ToolResult();

  static ToolResult Text(std::string text);
  static ToolResult Error(std::string text);

  // {"content": [...], "is_error": bool}.
  base::DictValue ToDict() &&;

  base::ListValue content;
  bool is_error = false;
  // A memory tool's change; not part of the tool_result.
  std::optional<MemoryChange> memory_change;
};

// The browser around ClaudeTools, for the browser tools (list_tabs,
// activate_tab, move_tab, …): the profile's sidebar, windows and data. Lives
// in the UI layer (ClaudeTabs), which knows windows, spaces and services;
// ClaudeTools checks the permission and keeps the tab it acts on.
// docs/design/claude.md, D5 and D7.
class TabsDelegate {
 public:
  // What the tools know of the session.
  struct Context {
    // The tab the page tools act on, or null.
    raw_ptr<content::WebContents> target = nullptr;
    // The ids of the tabs Claude opened.
    raw_ptr<const std::set<std::string>> opened = nullptr;
  };
  struct Result {
    Result();
    Result(Result&&);
    Result& operator=(Result&&);
    ~Result();

    ToolResult result;
    // The page tools act on this tab from now on (activate_tab).
    base::WeakPtr<content::WebContents> act_on;
  };

  virtual ~TabsDelegate() = default;

  // "Let Claude use the whole browser", now.
  virtual bool BrowserScopeAllowed() = 0;
  // list_tabs's text: the sidebar of every space, and other windows' tabs.
  virtual std::string ListTabs(const Context& context) = 0;
  // The live tab with list_tabs id |id|, or null (unloaded, or no such tab).
  virtual content::WebContents* FindLiveTab(const std::string& id) = 0;
  // The tab |id|, loading an unloaded entry in the background if a window
  // shows its space; or why not.
  virtual base::expected<content::WebContents*, std::string> LoadTab(
      const std::string& id) = 0;
  // Opens |url| in a new tab of the chat's window (its space), in the
  // background unless |active|. Null if it can't.
  virtual content::WebContents* OpenTab(const GURL& url, bool active) = 0;
  // |contents|' list_tabs id.
  virtual std::string IdOf(content::WebContents* contents) = 0;
  // The other browser tools: close_tab, activate_tab, move_tab, rename_tab,
  // create_folder, mute_tab, split_tabs, switch_space, create_space,
  // search_history, list_downloads, send_tab_to_device.
  virtual void RunTool(const std::string& name,
                       base::DictValue input,
                       const Context& context,
                       base::OnceCallback<void(Result)> done) = 0;
};

// Runs Claude's tools (common/claude/tools.json) on a target tab. It knows
// nothing of the chat: the chat panel and, later, the MCP bridge are two
// callers. The page agent (common/claude/page_agent.js) reads the page from
// an isolated world; clicks, keys, screenshots, the console and run_js go
// through an internal DevTools client (DevToolsSession), so the user's own
// mouse and keyboard are never used. Tools run one at a time. While it
// works on a tab, the tab keeps rendering in the background; after a
// minute without tools it lets go. See docs/design/claude.md, "ClaudeTools".
class ClaudeTools : public content::WebContentsObserver {
 public:
  using Callback = base::OnceCallback<void(ToolResult)>;

  // |tabs| may be null: the browser tools then refuse.
  explicit ClaudeTools(std::unique_ptr<TabsDelegate> tabs = nullptr);
  ClaudeTools(const ClaudeTools&) = delete;
  ClaudeTools& operator=(const ClaudeTools&) = delete;
  ~ClaudeTools() override;

  // The definitions to send to the API: every tool, the memory tools only
  // with |memory|. The browser tools are always sent and answer "not
  // permitted" while "Let Claude use the whole browser" is off, so a
  // conversation stays valid when it's switched. The last one is marked for
  // caching.
  // |files|: the file tools (always sent where they exist, as the browser
  // tools; left out for MCP clients that have their own).
  // |devtools|: the developer tools, sent only while "Let Claude use
  // developer tools" is on (docs/design/claude.md, "Developer tools").
  static base::ListValue Definitions(bool memory = false,
                                     bool files = true,
                                     bool devtools = false);

  // The memory the memory tools change, and whether they may now ("Let
  // Claude save memories"). Without it, they refuse.
  void SetMemory(ClaudeMemory* memory,
                 base::RepeatingCallback<bool()> may_save);

  // The file tools' setting, read at each call, and how they ask the user
  // before a write: |approve| runs its callback with true to go on. Without
  // them, the file tools refuse.
  using FilesConfigGetter = base::RepeatingCallback<FilesConfig()>;
  using Approver = base::RepeatingCallback<void(ApprovalRequest,
                                                base::OnceCallback<void(bool)>)>;
  void SetFiles(FilesConfigGetter config, Approver approve);

  // Whether the developer tools may run now ("Let Claude use developer
  // tools"). Without it, they refuse. edit_page_storage asks through the
  // approver SetFiles() gave.
  void SetDevTools(base::RepeatingCallback<bool()> allowed);

  // The task's tab: the page tools act on it until select_tab or open_tab
  // picks another. Setting another one stops what runs.
  void SetTarget(content::WebContents* contents);
  content::WebContents* target() { return web_contents(); }

  // Runs tool |name| with |input|. A Run() while another runs stops it.
  void Run(const std::string& name, base::DictValue input, Callback callback);
  // Stops what runs (its callback gets "Stopped") and lets go of the tab.
  void Stop();

  // content::WebContentsObserver:
  void DidStopLoading() override;
  void WebContentsDestroyed() override;

 private:
  struct ConsoleEntry {
    uint64_t seq = 0;
    std::string level;
    std::string text;
    std::string source;
    double time = 0;
  };
  using Result = base::expected<base::DictValue, std::string>;
  // A request network_requests lists (Network.* events).
  struct NetworkEntry {
    NetworkEntry();
    NetworkEntry(NetworkEntry&&);
    NetworkEntry& operator=(NetworkEntry&&);
    ~NetworkEntry();

    uint64_t seq = 0;
    // DevTools' id; a redirect keeps it, so the latest entry with it wins.
    std::string request_id;
    std::string method;
    std::string url;
    std::string type;
    std::string mime_type;
    int status = 0;
    std::string status_text;
    // Why it failed (net::ERR_…), when it did.
    std::string error;
    bool finished = false;
    bool from_cache = false;
    // DevTools' monotonic timestamps, in seconds.
    double start = 0;
    double end = 0;
    // Bytes on the wire.
    double size = 0;
    base::DictValue request_headers;
    base::DictValue response_headers;
    std::string post_data;
    bool has_post_data = false;
  };

  void Finish(ToolResult result);
  // The page tools act on |contents| from now on, without stopping the
  // running tool (open_tab, select_tab).
  void Retarget(content::WebContents* contents);
  // Whether the browser tools may run; an error result otherwise.
  bool CheckBrowserScope();
  // The sidebar's tint (ClaudeActivity): on |contents| while tools run
  // there, and a moment after, so it doesn't flicker between steps.
  void SetActing(content::WebContents* contents);
  void StopActing();
  void Release();
  DevToolsSession& devtools();
  void OnAttach();
  void OnEvent(const std::string& method, const base::DictValue& params);
  void AddConsole(std::string level,
                  std::string text,
                  std::string source,
                  double time);
  void OnNetworkEvent(const std::string& method,
                      const base::DictValue& params);
  NetworkEntry* FindNetworkEntry(const std::string& request_id);
  // Lets go of the DevTools session and what lived in it.
  void ResetDevTools();
  // Whether the developer tools may run; an error result otherwise.
  bool CheckDevTools();
  // Records the tab's requests from now on (Network.enable), then |next|.
  void EnsureNetwork(base::OnceClosure next);

  // The page agent, on the target.
  void Agent(const std::string& method,
             base::DictValue opts,
             base::OnceCallback<void(Result)> callback);
  // DevTools commands, one after the other; |callback| gets the last
  // result, or the first error.
  void Send(const std::string& method,
            base::DictValue params,
            base::OnceCallback<void(Result)> callback);
  void SendAll(std::vector<std::pair<std::string, base::DictValue>> commands,
               base::OnceCallback<void(Result)> callback);

  // Where a tool's ref or coordinate is, in viewport CSS pixels; with a
  // description of the element there.
  struct Point {
    double x = 0;
    double y = 0;
    std::string element;
    std::string covered;
  };
  void Locate(const base::DictValue& input,
              base::OnceCallback<void(base::expected<Point, std::string>)>
                  callback);

  void ReadPage(base::DictValue input);
  void GetPageText(base::DictValue input);
  void Find(base::DictValue input);
  void Screenshot(base::DictValue input);
  void OnLayoutMetrics(Result metrics);
  void ReadConsole(base::DictValue input);
  void RunJs(base::DictValue input);
  void Click(base::DictValue input);
  void Hover(base::DictValue input);
  void Type(base::DictValue input);
  void Key(base::DictValue input);
  void Scroll(base::DictValue input);
  void FormInput(base::DictValue input);
  void Navigate(base::DictValue input);
  void Wait(base::DictValue input);
  void ListTabs(base::DictValue input);
  void OpenTab(base::DictValue input);
  void SelectTab(base::DictValue input);
  void CloseTab(base::DictValue input);
  // The developer tools (claude_devtools_tools.cc).
  void NetworkRequests(base::DictValue input);
  void NetworkResponse(base::DictValue input);
  // inspect_element and page_storage run DevTools commands one after the
  // other, gathering what they answer.
  struct InspectState;
  struct StorageState;
  void InspectElement(base::DictValue input);
  void InspectStep(std::unique_ptr<InspectState> state, int step, Result result);
  void PageStorage(base::DictValue input);
  void StorageStep(std::unique_ptr<StorageState> state, int step, Result result);
  void EditPageStorage(base::DictValue input);
  void RunStorageEdit(base::DictValue input);
  void OnCssEvent(const std::string& method, const base::DictValue& params);
  // remember, update_memory, forget.
  ToolResult RunMemoryTool(const std::string& name,
                           const base::DictValue& input);
  // save_file, list_files, read_file, write_file.
  void RunFileTool(const std::string& name, base::DictValue input);
  // Asks the user about |request|, then runs |write| on the file sequence.
  void ApproveThenWrite(
      ApprovalRequest request,
      base::OnceCallback<base::expected<std::string, std::string>()> write);
  void OnApproval(
      base::OnceCallback<base::expected<std::string, std::string>()> write,
      bool allowed);
  void OnWriteChecked(FilesConfig config,
                      std::string path,
                      std::string content,
                      base::expected<bool, std::string> exists);
  void FinishWithText(base::expected<std::string, std::string> text);
  // Asks the user about |request|, then runs |then| (a change the user must
  // allow, like edit_page_storage's).
  void ApproveThen(ApprovalRequest request, base::OnceClosure then);
  // The browser tools ClaudeTabs runs.
  void RunBrowserTool(const std::string& name, base::DictValue input);
  void OnBrowserToolDone(TabsDelegate::Result result);
  TabsDelegate::Context MakeContext();
  // A short pause after an action, then what changed (the URL).
  void ReportAfterAction(std::string done);
  void ShowIndicator(double x, double y);

  std::unique_ptr<TabsDelegate> tabs_;
  base::WeakPtr<ClaudeMemory> memory_;
  base::RepeatingCallback<bool()> may_save_memory_;
  FilesConfigGetter files_config_;
  Approver approve_;
  base::RepeatingCallback<bool()> devtools_allowed_;
  // Where file work runs (blocking).
  scoped_refptr<base::SequencedTaskRunner> file_runner_;
  // The tab the user's message was about (SetTarget()).
  base::WeakPtr<content::WebContents> task_tab_;
  // Tabs Claude opened: the ones close_tab may close.
  std::set<std::string> opened_;
  std::unique_ptr<DevToolsSession> devtools_;
  base::ScopedClosureRunner capture_;
  Callback callback_;
  std::string url_before_;
  // A screenshot pixel is 1 / |screenshot_scale_| CSS pixels.
  double screenshot_scale_ = 1;
  std::deque<ConsoleEntry> console_;
  uint64_t next_console_seq_ = 1;
  uint64_t console_read_seq_ = 0;
  // The requests since the developer tools first ran on the tab.
  std::deque<NetworkEntry> network_;
  uint64_t next_network_seq_ = 1;
  uint64_t network_read_seq_ = 0;
  // Network.enable was sent in this session.
  bool network_enabled_ = false;
  // CSS.enable was, and the style sheets it told of, by id: where a rule
  // comes from.
  struct StyleSheet {
    std::string url;
    int start_line = 0;
    bool is_inline = false;
  };
  bool css_enabled_ = false;
  std::map<std::string, StyleSheet> style_sheets_;
  base::OnceClosure on_loaded_;
  base::OneShotTimer timeout_;
  base::OneShotTimer idle_;
  base::OneShotTimer delay_;
  base::WeakPtr<content::WebContents> acting_;
  base::OneShotTimer acting_linger_;
  // For each tool's steps; invalidated by Stop().
  base::WeakPtrFactory<ClaudeTools> run_weak_factory_{this};
  base::WeakPtrFactory<ClaudeTools> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_TOOLS_H_
