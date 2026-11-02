// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_TOOLS_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_TOOLS_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"
#include "url/gurl.h"

namespace content {
class WebContents;
}  // namespace content

namespace compass::claude {

class DevToolsSession;

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
};

// One open tab, as list_tabs shows it.
struct TabInfo {
  TabInfo();
  TabInfo(const TabInfo&);
  TabInfo& operator=(const TabInfo&);
  ~TabInfo();

  std::string id;
  std::string title;
  std::string url;
  // "Window 1", or "Not shown" for tabs of spaces no window shows.
  std::string window;
  std::string space;
  // The front tab of its window.
  bool active = false;
  base::WeakPtr<content::WebContents> contents;
};

// The browser around ClaudeTools, for the browser tools: the profile's tabs
// in every window and space. Lives in the UI layer (ClaudeTabs), which knows
// windows and spaces; ClaudeTools only asks.
class TabsDelegate {
 public:
  virtual ~TabsDelegate() = default;

  // "Let Claude use the whole browser", now.
  virtual bool BrowserScopeAllowed() = 0;
  // The profile's open tabs, front window first.
  virtual std::vector<TabInfo> ListTabs() = 0;
  // The tab with list_tabs id |id|, in this profile, or null.
  virtual content::WebContents* FindTab(const std::string& id) = 0;
  // Opens |url| in a new tab of the chat's window (its space), in the
  // background unless |active|. Null if it can't.
  virtual content::WebContents* OpenTab(const GURL& url, bool active) = 0;
  virtual std::string IdOf(content::WebContents* contents) = 0;
  virtual void CloseTab(content::WebContents* contents) = 0;
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

  // The definitions to send to the API: every tool. The browser tools are
  // always sent and answer "not permitted" while "Let Claude use the whole
  // browser" is off, so a conversation stays valid when it's switched.
  // The last one is marked for caching.
  static base::ListValue Definitions();

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
  // A short pause after an action, then what changed (the URL).
  void ReportAfterAction(std::string done);
  void ShowIndicator(double x, double y);

  std::unique_ptr<TabsDelegate> tabs_;
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
