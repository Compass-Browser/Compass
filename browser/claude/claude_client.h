// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_CLIENT_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_CLIENT_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/sse_parser.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"

class GURL;

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
namespace mojom {
class URLResponseHead;
}  // namespace mojom
}  // namespace network

namespace compass::claude {

// Talks to the Claude API (or the gateway the user set) from the browser
// process, where the key stays: the list of models, and streamed Messages
// API requests. One request of each kind at a time; a new one replaces the
// one running. See docs/design/claude.md, "Browser-side services".
class ClaudeClient : public network::SimpleURLLoaderStreamConsumer {
 public:
  using ModelsCallback = base::OnceCallback<void(
      base::expected<std::vector<ModelInfo>, std::string>)>;
  // An event of a streamed reply ("message_start", "content_block_delta",
  // ...; "ping" too), in order. It may delete the client.
  using EventCallback = base::RepeatingCallback<void(const SseParser::Event&)>;
  // The stream ended: no error once "message_stop" came, otherwise what went
  // wrong, for the user.
  using DoneCallback =
      base::OnceCallback<void(std::optional<std::string> error)>;

  ClaudeClient(scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
               Endpoint endpoint);
  ClaudeClient(const ClaudeClient&) = delete;
  ClaudeClient& operator=(const ClaudeClient&) = delete;
  ~ClaudeClient() override;

  // GET /v1/models: the models the key can use. Also how settings test the
  // connection.
  void ListModels(ModelsCallback callback);

  // POST /v1/messages with |request|, a Messages API request body, streamed
  // ("stream" is set). |on_event| gets each event; |on_done| runs once at
  // the end, unless Cancel() came first.
  // |betas| go in anthropic-beta.
  void StreamMessage(base::DictValue request,
                     std::vector<std::string> betas,
                     EventCallback on_event,
                     DoneCallback on_done);
  // Stops the stream, without running its DoneCallback.
  void Cancel();
  bool IsStreaming() const { return !!stream_loader_; }

  // network::SimpleURLLoaderStreamConsumer:
  void OnDataReceived(std::string_view data,
                      base::OnceClosure resume) override;
  void OnComplete(bool success) override;
  void OnRetry(base::OnceClosure start_retry) override;

 private:
  std::unique_ptr<network::SimpleURLLoader> MakeLoader(
      const GURL& url,
      std::string_view method,
      const std::vector<std::string>& betas = {});
  void OnModels(ModelsCallback callback, std::optional<std::string> body);
  void OnStreamStarted(const GURL& final_url,
                       const network::mojom::URLResponseHead& head);
  void FinishStream(std::optional<std::string> error);

  const scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const Endpoint endpoint_;

  std::unique_ptr<network::SimpleURLLoader> models_loader_;

  std::unique_ptr<network::SimpleURLLoader> stream_loader_;
  EventCallback on_event_;
  DoneCallback on_done_;
  SseParser parser_;
  int stream_status_ = 0;
  // An error reply's body, to say what went wrong.
  std::string error_body_;
  bool saw_message_stop_ = false;

  base::WeakPtrFactory<ClaudeClient> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_CLIENT_H_
