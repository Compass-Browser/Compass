// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_request.h"

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
  // The shared prompt, cached, then the context.
  const base::ListValue& system = *body.FindList("system");
  ASSERT_EQ(system.size(), 2u);
  EXPECT_EQ(*system[0].GetDict().FindString("text"), assets::kSystemPrompt);
  EXPECT_TRUE(system[0].GetDict().FindDict("cache_control"));
  EXPECT_FALSE(system[1].GetDict().FindDict("cache_control"));
  EXPECT_NE(system[1].GetDict().FindString("text")->find("permission is off"),
            std::string::npos);
  // The fallback, on the Claude API.
  EXPECT_EQ(*body.FindString("fallbacks"), "default");
  EXPECT_EQ(request.betas,
            std::vector<std::string>{"server-side-fallback-2026-07-01"});
  // Every tool, the browser ones too (they refuse while the permission is
  // off), the last one cached.
  const base::ListValue& tools = *body.FindList("tools");
  ASSERT_FALSE(tools.empty());
  EXPECT_EQ(*tools[0].GetDict().FindString("name"), "read_page");
  EXPECT_EQ(*tools.back().GetDict().FindString("name"), "close_tab");
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
  EXPECT_FALSE(
      BuildChatRequest(OneMessage(), options).body.contains("fallbacks"));

  options.claude_api = true;
  options.model = "claude-haiku-5-5";
  ChatRequest request = BuildChatRequest(OneMessage(), options);
  EXPECT_FALSE(request.body.contains("fallbacks"));
  EXPECT_TRUE(request.betas.empty());
}

TEST(ClaudeRequestTest, WindowTabsAreInTheContext) {
  ChatOptions options;
  options.model = "claude-opus-5-5";
  options.window_tabs = "- Docs — https://docs.test/\n";
  const std::string context = *(*BuildChatRequest(OneMessage(), options)
                                     .body.FindList("system"))[1]
                                   .GetDict()
                                   .FindString("text");
  EXPECT_NE(context.find("- Docs — https://docs.test/"), std::string::npos);
  EXPECT_NE(context.find("can't read them"), std::string::npos);
}

TEST(ClaudeRequestTest, BrowserScopeIsInTheContext) {
  ChatOptions options;
  options.model = "claude-opus-5-5";
  options.browser_scope = true;
  ChatRequest request = BuildChatRequest(OneMessage(), options);
  EXPECT_NE((*request.body.FindList("system"))[1]
                .GetDict()
                .FindString("text")
                ->find("whole browser"),
            std::string::npos);
}

}  // namespace
}  // namespace compass::claude
