// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/dev_mode/devtools_frontend_hook.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "compass/browser/dev_mode/network_log.h"
#include "compass/browser/dev_mode/network_log_buffer.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/web_contents.h"

namespace compass::dev_mode {

namespace {

// Messages longer than this are never ones the hook looks at: its commands
// and replies are small, and Target.attachedToTarget is too.
constexpr size_t kMaxLookedAt = 64 * 1024;

constexpr char kGetResponseBody[] = "Network.getResponseBody";
constexpr char kGetRequestPostData[] = "Network.getRequestPostData";

std::optional<base::DictValue> Parse(std::string_view message) {
  return base::JSONReader::ReadDict(message, base::JSON_PARSE_RFC);
}

std::string ToJson(const base::DictValue& dict) {
  std::string json;
  base::JSONWriter::Write(dict, &json);
  return json;
}

const std::string* RequestIdOf(const base::DictValue& params) {
  const std::string* id = params.FindString("requestId");
  return id ? id : params.FindString("transportId");
}

// The hook's delegate for a DevTools window: its bindings' callbacks, and
// the page its agent host is for.
class BindingsDelegate : public DevToolsFrontendHook::Delegate {
 public:
  explicit BindingsDelegate(DevToolsFrontendHook::Frontend frontend)
      : frontend_(std::move(frontend)) {}

  void set_agent_host(content::DevToolsAgentHost* host) { host_ = host; }
  content::DevToolsAgentHost* agent_host() const { return host_.get(); }
  void ShowPanel(const std::string& panel) { frontend_.show_panel.Run(panel); }

  // DevToolsFrontendHook::Delegate:
  const NetworkLogBuffer* GetBuffer() override {
    NetworkLog* log = GetNetworkLog();
    return log ? &log->buffer() : nullptr;
  }
  void SendToLog(const std::string& method,
                 base::DictValue params,
                 base::OnceCallback<void(DevToolsFrontendHook::Result)>
                     callback) override {
    NetworkLog* log = GetNetworkLog();
    if (!log) {
      std::move(callback).Run(
          base::unexpected("The page's recorded requests are gone."));
      return;
    }
    log->Send(method, std::move(params), std::move(callback));
  }
  std::string GetPageTargetId() override {
    content::WebContents* contents = GetWebContents();
    if (!contents) {
      return std::string();
    }
    scoped_refptr<content::DevToolsAgentHost> page =
        content::DevToolsAgentHost::GetOrCreateFor(contents);
    return page ? page->GetId() : std::string();
  }
  void DispatchToFrontend(const std::string& message) override {
    frontend_.dispatch.Run(message);
  }

 private:
  content::WebContents* GetWebContents() {
    return host_ ? host_->GetWebContents() : nullptr;
  }
  NetworkLog* GetNetworkLog() {
    content::WebContents* contents = GetWebContents();
    DevModeTabHelper* helper =
        contents ? DevModeTabHelper::FromWebContents(contents) : nullptr;
    return helper ? helper->network_log() : nullptr;
  }

  DevToolsFrontendHook::Frontend frontend_;
  scoped_refptr<content::DevToolsAgentHost> host_;
};

using Registry = std::map<const void*, std::unique_ptr<DevToolsFrontendHook>>;

Registry& GetRegistry() {
  static base::NoDestructor<Registry> registry;
  return *registry;
}

}  // namespace

DevToolsFrontendHook::Frontend::Frontend() = default;
DevToolsFrontendHook::Frontend::Frontend(Frontend&&) = default;
DevToolsFrontendHook::Frontend& DevToolsFrontendHook::Frontend::operator=(
    Frontend&&) = default;
DevToolsFrontendHook::Frontend::~Frontend() = default;

DevToolsFrontendHook::DevToolsFrontendHook(std::unique_ptr<Delegate> delegate)
    : delegate_(std::move(delegate)) {}

DevToolsFrontendHook::~DevToolsFrontendHook() = default;

bool DevToolsFrontendHook::OnMessageFromFrontend(std::string_view message) {
  if (message.size() > kMaxLookedAt) {
    return false;
  }
  const bool enable = message.find("\"Network.enable\"") != std::string::npos;
  const bool body =
      !replayed_.empty() &&
      (message.find(kGetResponseBody) != std::string::npos ||
       message.find(kGetRequestPostData) != std::string::npos);
  if (!enable && !body) {
    return false;
  }
  std::optional<base::DictValue> dict = Parse(message);
  if (!dict) {
    return false;
  }
  const std::string* method = dict->FindString("method");
  const std::optional<int> id = dict->FindInt("id");
  const std::string* session = dict->FindString("sessionId");
  if (!method || !id || !session || page_session_id_.empty() ||
      *session != page_session_id_) {
    return false;
  }
  if (*method == "Network.enable") {
    pending_enables_[*id] = *session;
    return false;
  }
  if (*method != kGetResponseBody && *method != kGetRequestPostData) {
    return false;
  }
  base::DictValue* params = dict->FindDict("params");
  const std::string* request_id = params ? RequestIdOf(*params) : nullptr;
  if (!request_id || !replayed_.contains(*request_id)) {
    return false;
  }
  // The frontend's session never saw this request; the log's did.
  delegate_->SendToLog(*method, std::move(*params),
                       base::BindOnce(&DevToolsFrontendHook::OnLogReply,
                                      weak_factory_.GetWeakPtr(), *id,
                                      *session));
  return true;
}

void DevToolsFrontendHook::OnMessageFromAgent(std::string_view message) {
  if (replaying_ || message.size() > kMaxLookedAt) {
    return;
  }
  if (message.find("\"Target.attachedToTarget\"") != std::string::npos) {
    std::optional<base::DictValue> dict = Parse(message);
    const base::DictValue* params = dict ? dict->FindDict("params") : nullptr;
    const std::string* session =
        params ? params->FindString("sessionId") : nullptr;
    const std::string* target_id =
        params ? params->FindStringByDottedPath("targetInfo.targetId")
               : nullptr;
    if (session && target_id && *target_id == delegate_->GetPageTargetId()) {
      // A new session with the page (DevTools opened, or reloaded itself).
      page_session_id_ = *session;
      pending_enables_.clear();
      replayed_.clear();
    }
    return;
  }
  // Replies start with their id.
  if (pending_enables_.empty() ||
      message.substr(0, 16).find("\"id\":") == std::string::npos) {
    return;
  }
  std::optional<base::DictValue> dict = Parse(message);
  const std::optional<int> id = dict ? dict->FindInt("id") : std::nullopt;
  if (!id) {
    return;
  }
  auto it = pending_enables_.find(*id);
  if (it == pending_enables_.end()) {
    return;
  }
  const std::string session = std::move(it->second);
  pending_enables_.erase(it);
  if (!dict->FindDict("error")) {
    Replay(session);
  }
}

void DevToolsFrontendHook::Replay(const std::string& session_id) {
  const NetworkLogBuffer* buffer = delegate_->GetBuffer();
  if (!buffer || buffer->empty()) {
    return;
  }
  // Right after the reply: the page's own events for the session come
  // after these, so a request still loading carries on from here.
  replaying_ = true;
  buffer->ForEach([&](const std::string& method,
                      const base::DictValue& params) {
    if (const std::string* request_id = RequestIdOf(params)) {
      replayed_.insert(*request_id);
    }
    delegate_->DispatchToFrontend(ToJson(base::DictValue()
                                             .Set("method", method)
                                             .Set("params", params.Clone())
                                             .Set("sessionId", session_id)));
  });
  replaying_ = false;
}

void DevToolsFrontendHook::OnLogReply(int id,
                                      const std::string& session_id,
                                      Result result) {
  base::DictValue reply;
  reply.Set("id", id);
  reply.Set("sessionId", session_id);
  if (result.has_value()) {
    reply.Set("result", std::move(result.value()));
  } else {
    reply.Set("error", base::DictValue()
                           .Set("code", -32000)
                           .Set("message", result.error()));
  }
  delegate_->DispatchToFrontend(ToJson(reply));
}

// static
bool DevToolsFrontendHook::HandleMessageFromFrontend(
    const void* bindings,
    content::DevToolsAgentHost* agent_host,
    std::string_view message,
    base::FunctionRef<Frontend()> make_frontend) {
  Registry& registry = GetRegistry();
  auto it = registry.find(bindings);
  if (it == registry.end()) {
    it = registry
             .emplace(bindings, std::make_unique<DevToolsFrontendHook>(
                                    std::make_unique<BindingsDelegate>(
                                        make_frontend())))
             .first;
  }
  DevToolsFrontendHook* hook = it->second.get();
  static_cast<BindingsDelegate*>(hook->delegate())->set_agent_host(agent_host);
  return hook->OnMessageFromFrontend(message);
}

// static
void DevToolsFrontendHook::HandleMessageFromAgent(const void* bindings,
                                                  std::string_view message) {
  Registry& registry = GetRegistry();
  auto it = registry.find(bindings);
  if (it != registry.end()) {
    it->second->OnMessageFromAgent(message);
  }
}

// static
void DevToolsFrontendHook::RemoveFor(const void* bindings) {
  GetRegistry().erase(bindings);
}

bool ShowDevToolsPanel(content::WebContents* inspected,
                       std::string_view panel) {
  scoped_refptr<content::DevToolsAgentHost> tab =
      content::DevToolsAgentHost::GetOrCreateForTab(inspected);
  for (auto& [bindings, hook] : GetRegistry()) {
    auto* delegate = static_cast<BindingsDelegate*>(hook->delegate());
    if (delegate->agent_host() == tab.get()) {
      delegate->ShowPanel(std::string(panel));
      return true;
    }
  }
  return false;
}

}  // namespace compass::dev_mode
