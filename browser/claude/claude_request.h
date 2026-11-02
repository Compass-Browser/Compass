// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_REQUEST_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_REQUEST_H_

#include <string>
#include <vector>

#include "base/time/time.h"
#include "base/values.h"

namespace compass::claude {

// What a chat request needs beside the conversation.
struct ChatOptions {
  std::string model;
  // The requests go to the Claude API itself, not a gateway: the
  // server-side refusal fallback is only there.
  bool claude_api = true;
  // "Let Claude use the whole browser".
  bool browser_scope = false;
  // The chat window's tabs, a line each ("- title — url"), for context.
  std::string window_tabs;
  // Send the tools (ClaudeTools::Definitions()).
  bool tools = true;
  base::Time now;
};

struct ChatRequest {
  ChatRequest();
  ChatRequest(ChatRequest&&);
  ChatRequest& operator=(ChatRequest&&);
  ~ChatRequest();

  // The Messages API request body; ClaudeClient adds "stream".
  base::DictValue body;
  // anthropic-beta values.
  std::vector<std::string> betas;
};

// The request for the next reply to |messages| (the Messages API's
// "messages", as the chat page keeps them). The browser, not the page, sets
// the model, the system prompt (common/claude/system_prompt.md) and the
// rest, so the page can't change Claude's instructions. See
// docs/design/claude.md, "Request defaults".
ChatRequest BuildChatRequest(base::ListValue messages,
                             const ChatOptions& options);

// Whether |model| takes the server-side refusal fallback
// (fallbacks: "default").
bool SupportsDefaultFallback(const std::string& model);

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_REQUEST_H_
