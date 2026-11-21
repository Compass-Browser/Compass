// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_DEV_MODE_DEVTOOLS_FRONTEND_HOOK_H_
#define COMPASS_BROWSER_DEV_MODE_DEVTOOLS_FRONTEND_HOOK_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "base/values.h"

namespace content {
class DevToolsAgentHost;
class WebContents;
}  // namespace content

namespace compass::dev_mode {

class NetworkLogBuffer;

// Sits between a DevTools window's frontend and the page it inspects
// (docs/design/dev-mode.md, "Requests before DevTools"). When the frontend
// turns on the Network domain for the page, it gets the requests the
// page's NetworkLog recorded before, as if they had just happened; asking
// for one of those requests' bodies is answered from the log's session,
// which saw them. Everything else passes untouched, and costs a substring
// search.
class DevToolsFrontendHook {
 public:
  using Result = base::expected<base::DictValue, std::string>;

  // The window's bindings and the page, for the hook.
  class Delegate {
   public:
    virtual ~Delegate() = default;
    // The page's recorded requests, while it is in development mode.
    virtual const NetworkLogBuffer* GetBuffer() = 0;
    // Sends a command on the log's session.
    virtual void SendToLog(const std::string& method,
                           base::DictValue params,
                           base::OnceCallback<void(Result)> callback) = 0;
    // The page's main frame target, as Target.attachedToTarget names it.
    virtual std::string GetPageTargetId() = 0;
    // Hands |message| to the frontend, as from the page.
    virtual void DispatchToFrontend(const std::string& message) = 0;
  };

  explicit DevToolsFrontendHook(std::unique_ptr<Delegate> delegate);
  DevToolsFrontendHook(const DevToolsFrontendHook&) = delete;
  DevToolsFrontendHook& operator=(const DevToolsFrontendHook&) = delete;
  ~DevToolsFrontendHook();

  // A protocol message from the frontend. True if the hook answers it, and
  // it mustn't go to the page.
  bool OnMessageFromFrontend(std::string_view message);
  // A protocol message from the page, after the frontend has it.
  void OnMessageFromAgent(std::string_view message);

  Delegate* delegate() { return delegate_.get(); }

  // For upstream's DevToolsUIBindings (see
  // patches/chrome-browser-devtools-devtools_ui_bindings.cc.patch), whose
  // address is |bindings|: one hook per bindings, made on the frontend's
  // first message from what |make_frontend| gives.
  struct Frontend {
    Frontend();
    Frontend(Frontend&&);
    Frontend& operator=(Frontend&&);
    ~Frontend();

    // Dispatches a protocol message to the frontend, as from the page.
    base::RepeatingCallback<void(const std::string&)> dispatch;
    // Switches the frontend to a panel ("console", "network").
    base::RepeatingCallback<void(const std::string&)> show_panel;
  };
  static bool HandleMessageFromFrontend(
      const void* bindings,
      content::DevToolsAgentHost* agent_host,
      std::string_view message,
      base::FunctionRef<Frontend()> make_frontend);
  static void HandleMessageFromAgent(const void* bindings,
                                     std::string_view message);
  static void RemoveFor(const void* bindings);

 private:
  void Replay(const std::string& session_id);
  void OnLogReply(int id, const std::string& session_id, Result result);

  std::unique_ptr<Delegate> delegate_;
  // The frontend's session with the page's main frame.
  std::string page_session_id_;
  // Network.enable commands for that session, until their replies.
  std::map<int, std::string> pending_enables_;
  // The requests the frontend got from the log.
  std::set<std::string, std::less<>> replayed_;
  bool replaying_ = false;
  base::WeakPtrFactory<DevToolsFrontendHook> weak_factory_{this};
};

// Switches the DevTools window inspecting |inspected| to |panel|. False if
// none is open.
bool ShowDevToolsPanel(content::WebContents* inspected, std::string_view panel);

}  // namespace compass::dev_mode

#endif  // COMPASS_BROWSER_DEV_MODE_DEVTOOLS_FRONTEND_HOOK_H_
