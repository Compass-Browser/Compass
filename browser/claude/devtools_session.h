// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_DEVTOOLS_SESSION_H_
#define COMPASS_BROWSER_CLAUDE_DEVTOOLS_SESSION_H_

#include <map>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host_client.h"

namespace content {
class DevToolsAgentHost;
class WebContents;
}  // namespace content

namespace compass::claude {

// An internal DevTools protocol client attached to one tab: sends commands,
// matches their replies, and passes events on. Internal clients show no
// "being debugged" bar, and the page can't tell. It refuses the browser's
// own pages (WebUI), so DevTools detaches it when the tab navigates to one;
// the next command attaches again. See docs/design/claude.md, "ClaudeTools".
class DevToolsSession : public content::DevToolsAgentHostClient {
 public:
  using Result = base::expected<base::DictValue, std::string>;
  using ResultCallback = base::OnceCallback<void(Result)>;
  // An event's method ("Runtime.consoleAPICalled") and params.
  using EventCallback =
      base::RepeatingCallback<void(const std::string&, const base::DictValue&)>;

  // |on_attach| runs after each attach, to enable the domains the caller
  // needs (their events come to |on_event|).
  DevToolsSession(content::WebContents* contents,
                  EventCallback on_event,
                  base::RepeatingClosure on_attach);
  DevToolsSession(const DevToolsSession&) = delete;
  DevToolsSession& operator=(const DevToolsSession&) = delete;
  ~DevToolsSession() override;

  // Sends |method| with |params|; |callback| gets the result, or the
  // protocol's error message.
  void Send(const std::string& method,
            base::DictValue params,
            ResultCallback callback);

  // Attaches now, if it isn't: events start to come. Returns false if the
  // tab refuses (a browser page) or is gone.
  bool Attach();
  bool attached() const { return !!host_; }

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;
  bool MayAttachToURL(const GURL& url, bool is_webui) override;
  bool MayReadLocalFiles() override;
  bool MayWriteLocalFiles() override;
  std::string GetTypeForMetrics() override;

 private:
  void FailPending(const std::string& error);

  base::WeakPtr<content::WebContents> contents_;
  EventCallback on_event_;
  base::RepeatingClosure on_attach_;
  scoped_refptr<content::DevToolsAgentHost> host_;
  int next_id_ = 1;
  std::map<int, ResultCallback> pending_;
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_DEVTOOLS_SESSION_H_
