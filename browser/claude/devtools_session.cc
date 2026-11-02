// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/devtools_session.h"

#include <utility>

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace compass::claude {

DevToolsSession::DevToolsSession(content::WebContents* contents,
                                 EventCallback on_event,
                                 base::RepeatingClosure on_attach)
    : contents_(contents->GetWeakPtr()),
      on_event_(std::move(on_event)),
      on_attach_(std::move(on_attach)) {}

DevToolsSession::~DevToolsSession() {
  // Callers' callbacks may point at their owner, which is going away too:
  // dropped, not run.
  pending_.clear();
  if (host_) {
    host_->DetachClient(this);
  }
}

bool DevToolsSession::Attach() {
  if (host_) {
    return true;
  }
  if (!contents_) {
    return false;
  }
  scoped_refptr<content::DevToolsAgentHost> host =
      content::DevToolsAgentHost::GetOrCreateFor(contents_.get());
  if (!host || !host->AttachClient(this)) {
    return false;
  }
  host_ = std::move(host);
  if (on_attach_) {
    on_attach_.Run();
  }
  return true;
}

void DevToolsSession::Send(const std::string& method,
                           base::DictValue params,
                           ResultCallback callback) {
  if (!Attach()) {
    std::move(callback).Run(
        base::unexpected("Claude can't use this tab right now."));
    return;
  }
  const int id = next_id_++;
  std::string json;
  base::JSONWriter::Write(base::DictValue()
                              .Set("id", id)
                              .Set("method", method)
                              .Set("params", std::move(params)),
                          &json);
  pending_.emplace(id, std::move(callback));
  // Dispatching can detach this client (a crash): keep |host_| alive.
  scoped_refptr<content::DevToolsAgentHost> host = host_;
  host->DispatchProtocolMessage(this, base::as_byte_span(json));
}

void DevToolsSession::DispatchProtocolMessage(
    content::DevToolsAgentHost* agent_host,
    base::span<const uint8_t> message) {
  std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
      base::as_string_view(message), base::JSON_PARSE_RFC);
  if (!dict) {
    return;
  }
  if (std::optional<int> id = dict->FindInt("id")) {
    auto it = pending_.find(*id);
    if (it == pending_.end()) {
      return;
    }
    ResultCallback callback = std::move(it->second);
    pending_.erase(it);
    if (const base::DictValue* error = dict->FindDict("error")) {
      const std::string* text = error->FindString("message");
      std::move(callback).Run(
          base::unexpected(text ? *text : std::string("DevTools error")));
      return;
    }
    base::DictValue* result = dict->FindDict("result");
    std::move(callback).Run(result ? std::move(*result) : base::DictValue());
    return;
  }
  const std::string* method = dict->FindString("method");
  const base::DictValue* params = dict->FindDict("params");
  if (!method || !on_event_) {
    return;
  }
  if (params) {
    on_event_.Run(*method, *params);
  } else {
    on_event_.Run(*method, base::DictValue());
  }
}

void DevToolsSession::AgentHostClosed(content::DevToolsAgentHost* agent_host) {
  host_ = nullptr;
  FailPending("The page went away (it navigated, crashed or closed).");
}

bool DevToolsSession::MayAttachToURL(const GURL& url, bool is_webui) {
  return !is_webui;
}

bool DevToolsSession::MayReadLocalFiles() {
  return false;
}

bool DevToolsSession::MayWriteLocalFiles() {
  return false;
}

std::string DevToolsSession::GetTypeForMetrics() {
  return "Other";
}

void DevToolsSession::FailPending(const std::string& error) {
  std::map<int, ResultCallback> pending = std::move(pending_);
  pending_.clear();
  for (auto& [id, callback] : pending) {
    std::move(callback).Run(base::unexpected(error));
  }
}

}  // namespace compass::claude
