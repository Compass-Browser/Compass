// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_request.h"

#include <algorithm>

#include <string>

#include "base/values.h"
#include "compass/browser/claude/claude_assets.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

base::ListValue OneMessage() {
  return base::ListValue().Append(
      base::DictValue().Set("role", "user").Set("content", "Hi"));
}

TEST(ClaudeRequestTest, Defaults) {
  ChatOptions options;
  options.model = "claude-opus-5-5";
  options.now = base::Time::Now();
  ChatRequest request = BuildChatRequest(OneMessage(), options);
  const base::DictValue& body = request.body;
  EXPECT_EQ(*body.FindString("model"), "claude-opus-5-5");
  EXPECT_EQ(*body.FindInt("max_tokens"), 64000);
  EXPECT_EQ(*body.FindStringByDottedPath("thinking.type"), "adaptive");
  EXPECT_EQ(*body.FindStringByDottedPath("thinking.display"), "summarized");
  EXPECT_EQ(*body.FindStringByDottedPath("cache_control.type"), "ephemeral");
  ASSERT_EQ(body.FindList("messages")->size(), 1u);
  // The shared prompt and how Compass's additions read, cached, and nothing
  // that changes: thinking is bound to it.
  const base::ListValue& system = *body.FindList("system");
  ASSERT_EQ(system.size(), 2u);
  EXPECT_EQ(*system[0].GetDict().FindString("text"), assets::kSystemPrompt);
  EXPECT_FALSE(system[0].GetDict().FindDict("cache_control"));
  EXPECT_TRUE(system[1].GetDict().FindDict("cache_control"));
  EXPECT_NE(system[1].GetDict().FindString("text")->find("<context>"),
            std::string::npos);
  ChatOptions later = options;
  later.now = options.now + base::Days(3);
  later.browser_scope = true;
  later.window_tabs = "- Docs — https://docs.test/\n";
  EXPECT_EQ(*BuildChatRequest(OneMessage(), later).body.FindList("system"),
            system);
  // On the Claude API: stale thinking dropped, not refused; the fallback.
  EXPECT_EQ(*body.FindStringByDottedPath(
                "thinking.block_binding.prefix_mismatch_behavior"),
            "drop_block");
  EXPECT_EQ(*body.FindString("fallbacks"), "default");
  EXPECT_EQ(request.betas,
            (std::vector<std::string>{"thinking-binding-controls-2026-08-01",
                                      "server-side-fallback-2026-07-01"}));
  // Every tool, the browser ones too (they refuse while the permission is
  // off), the last one cached.
  const base::ListValue& tools = *body.FindList("tools");
  ASSERT_FALSE(tools.empty());
  EXPECT_EQ(*tools[0].GetDict().FindString("name"), "read_page");
  EXPECT_TRUE(std::ranges::any_of(tools, [](const base::Value& tool) {
    return *tool.GetDict().FindString("name") == "close_tab";
  }));
  EXPECT_TRUE(tools.back().GetDict().FindDict("cache_control"));
  for (const base::Value& tool : tools) {
    EXPECT_TRUE(tool.GetDict().FindDict("input_schema"));
    EXPECT_FALSE(tool.GetDict().contains("scope"));
  }
  // No sampling parameters, which the API version refuses.
  EXPECT_FALSE(body.contains("temperature"));
}

TEST(ClaudeRequestTest, NoFallbackOnGatewaysOrOtherModels) {
  ChatOptions options;
  options.model = "claude-opus-5-5";
  options.claude_api = false;
  ChatRequest gateway = BuildChatRequest(OneMessage(), options);
  EXPECT_FALSE(gateway.body.contains("fallbacks"));
  EXPECT_FALSE(gateway.body.FindDictByDottedPath("thinking.block_binding"));
  EXPECT_TRUE(gateway.betas.empty());

  options.claude_api = true;
  options.model = "claude-haiku-5-5";
  ChatRequest request = BuildChatRequest(OneMessage(), options);
  EXPECT_FALSE(request.body.contains("fallbacks"));
  EXPECT_EQ(request.betas, std::vector<std::string>{
                               "thinking-binding-controls-2026-08-01"});
}

TEST(ClaudeRequestTest, MessageContext) {
  ChatOptions options;
  options.now = base::Time::Now();
  options.window_tabs = "- Docs — https://docs.test/\n";
  const std::string context = BuildMessageContext(options);
  EXPECT_TRUE(context.starts_with("<context>\nToday is "));
  EXPECT_TRUE(context.ends_with("</context>"));
  EXPECT_NE(context.find("- Docs — https://docs.test/"), std::string::npos);
  EXPECT_NE(context.find("permission is off"), std::string::npos);
  EXPECT_NE(context.find("can't read them"), std::string::npos);

  options.browser_scope = true;
  options.window_tabs.clear();
  const std::string whole = BuildMessageContext(options);
  EXPECT_NE(whole.find("whole browser"), std::string::npos);
  EXPECT_EQ(whole.find("Tabs open"), std::string::npos);
}

}  // namespace
}  // namespace compass::claude
