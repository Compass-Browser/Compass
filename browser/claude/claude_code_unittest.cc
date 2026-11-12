// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_code.h"

#include <algorithm>

#include "base/json/json_reader.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

using Type = ClaudeCodeEvent::Type;

base::DictValue Json(const std::string& text) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(text, base::JSON_PARSE_RFC);
  EXPECT_TRUE(dict) << text;
  return dict ? std::move(*dict) : base::DictValue();
}

bool Has(const std::vector<std::string>& args,
         const std::string& flag,
         const std::string& value) {
  auto it = std::ranges::find(args, flag);
  return it != args.end() && it + 1 != args.end() && *(it + 1) == value;
}

// Lines as Claude Code 2.1.319 wrote them (trimmed).
TEST(ClaudeCodeTest, ParsesInit) {
  auto events = ParseClaudeCodeLine(
      R"({"type":"system","subtype":"init","cwd":"/tmp","session_id":)"
      R"("ada87fe3-739f-46f8-accc-7b122676e6e7","tools":[]})");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, Type::kInit);
  EXPECT_EQ(events[0].session_id, "ada87fe3-739f-46f8-accc-7b122676e6e7");

  EXPECT_TRUE(ParseClaudeCodeLine(
                  R"({"type":"system","subtype":"status","status":)"
                  R"("requesting","session_id":"x"})")
                  .empty());
}

TEST(ClaudeCodeTest, PassesStreamEvents) {
  auto events = ParseClaudeCodeLine(
      R"({"type":"stream_event","event":{"type":"content_block_delta",)"
      R"("index":0,"delta":{"type":"text_delta","text":"Hello"}},)"
      R"("session_id":"s","parent_tool_use_id":null})");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, Type::kStreamEvent);
  EXPECT_EQ(events[0].name, "content_block_delta");
  base::DictValue data = Json(events[0].data);
  EXPECT_EQ(*data.FindStringByDottedPath("delta.text"), "Hello");
}

TEST(ClaudeCodeTest, StripsToolPrefix) {
  auto events = ParseClaudeCodeLine(
      R"({"type":"stream_event","event":{"type":"content_block_start",)"
      R"("index":1,"content_block":{"type":"tool_use","id":"toolu_1",)"
      R"("name":"mcp__compass__get_page_text","input":{}}},)"
      R"("parent_tool_use_id":null})");
  ASSERT_EQ(events.size(), 1u);
  base::DictValue data = Json(events[0].data);
  EXPECT_EQ(*data.FindStringByDottedPath("content_block.name"),
            "get_page_text");
}

TEST(ClaudeCodeTest, IgnoresPingsSubagentsAndWholeMessages) {
  EXPECT_TRUE(ParseClaudeCodeLine(
                  R"({"type":"stream_event","event":{"type":"ping"}})")
                  .empty());
  EXPECT_TRUE(ParseClaudeCodeLine(
                  R"({"type":"stream_event","event":{"type":"message_stop"},)"
                  R"("parent_tool_use_id":"toolu_9"})")
                  .empty());
  EXPECT_TRUE(ParseClaudeCodeLine(
                  R"({"type":"assistant","message":{"content":[]}})")
                  .empty());
  EXPECT_TRUE(ParseClaudeCodeLine(
                  R"({"type":"rate_limit_event","rate_limit_info":{}})")
                  .empty());
  EXPECT_TRUE(ParseClaudeCodeLine("Warning: something on stderr").empty());
}

TEST(ClaudeCodeTest, ToolResults) {
  auto events = ParseClaudeCodeLine(
      R"({"type":"user","message":{"role":"user","content":[)"
      R"({"tool_use_id":"toolu_1","type":"tool_result","content":[)"
      R"({"type":"text","text":"The page says: purple elephants."}]},)"
      R"({"tool_use_id":"toolu_2","type":"tool_result","content":"Nope",)"
      R"("is_error":true}]},"parent_tool_use_id":null})");
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].type, Type::kToolResult);
  EXPECT_EQ(events[0].tool_use_id, "toolu_1");
  base::DictValue first = Json(events[0].data);
  EXPECT_EQ(*first.FindBool("is_error"), false);
  EXPECT_EQ(*(*first.FindList("content"))[0].GetDict().FindString("text"),
            "The page says: purple elephants.");
  base::DictValue second = Json(events[1].data);
  EXPECT_EQ(*second.FindBool("is_error"), true);
  EXPECT_EQ(*(*second.FindList("content"))[0].GetDict().FindString("text"),
            "Nope");
}

TEST(ClaudeCodeTest, Results) {
  auto done = ParseClaudeCodeLine(
      R"({"type":"result","subtype":"success","is_error":false,)"
      R"("result":"Hi"})");
  ASSERT_EQ(done.size(), 1u);
  EXPECT_EQ(done[0].type, Type::kResult);
  EXPECT_TRUE(done[0].error.empty());

  auto turns = ParseClaudeCodeLine(
      R"({"type":"result","subtype":"error_max_turns","is_error":true,)"
      R"json("errors":["Reached maximum number of turns (40)"]})json");
  ASSERT_EQ(turns.size(), 1u);
  EXPECT_NE(turns[0].error.find("go on"), std::string::npos);

  auto signed_out = ParseClaudeCodeLine(
      R"({"type":"result","subtype":"success","is_error":true,)"
      R"("result":"Not logged in · Please run /login"})");
  ASSERT_EQ(signed_out.size(), 1u);
  EXPECT_EQ(signed_out[0].error, "Not logged in · Please run /login");
}

TEST(ClaudeCodeTest, Args) {
  ClaudeCodeOptions options;
  options.model = "claude-opus-5-5";
  options.system_prompt = "Be kind.";
  std::vector<std::string> args = ClaudeCodeArgs(options);
  EXPECT_EQ(args[0], "-p");
  EXPECT_TRUE(Has(args, "--input-format", "stream-json"));
  EXPECT_TRUE(Has(args, "--output-format", "stream-json"));
  EXPECT_TRUE(Has(args, "--model", "claude-opus-5-5"));
  EXPECT_TRUE(Has(args, "--system-prompt", "Be kind."));
  EXPECT_TRUE(Has(args, "--tools", ""));
  EXPECT_TRUE(Has(args, "--setting-sources", ""));
  EXPECT_TRUE(Has(args, "--max-turns", "40"));
  EXPECT_NE(std::ranges::find(args, "--strict-mcp-config"), args.end());
  // No relay (no MCP bridge on this system): no tools at all.
  EXPECT_EQ(std::ranges::find(args, "--mcp-config"), args.end());
  EXPECT_EQ(std::ranges::find(args, "--resume"), args.end());

  options.mcp_relay = base::FilePath(FILE_PATH_LITERAL("/Compass/compass-mcp"));
  options.user_data_dir = base::FilePath(FILE_PATH_LITERAL("/Data"));
  options.chat_token = "abc123";
  options.resume = "session-1";
  args = ClaudeCodeArgs(options);
  EXPECT_TRUE(Has(args, "--allowedTools", "mcp__compass"));
  EXPECT_TRUE(Has(args, "--resume", "session-1"));
  auto config = std::ranges::find(args, "--mcp-config");
  ASSERT_NE(config, args.end());
  base::DictValue mcp = Json(*(config + 1));
  const base::DictValue* compass = mcp.FindDictByDottedPath("mcpServers.compass");
  ASSERT_TRUE(compass);
  EXPECT_EQ(*compass->FindString("command"), "/Compass/compass-mcp");
  const base::ListValue& relay_args = *compass->FindList("args");
  ASSERT_EQ(relay_args.size(), 2u);
  EXPECT_EQ(relay_args[0].GetString(), "--user-data-dir=/Data");
  EXPECT_EQ(relay_args[1].GetString(), "--chat=abc123");
}

// With its own tools: the list (no subagents) and the questions on stdio.
TEST(ClaudeCodeTest, OwnToolsArgs) {
  ClaudeCodeOptions options;
  options.own_tools = true;
  std::vector<std::string> args = ClaudeCodeArgs(options);
  EXPECT_TRUE(Has(args, "--tools", kClaudeCodeOwnTools));
  EXPECT_TRUE(Has(args, "--permission-prompt-tool", "stdio"));
  EXPECT_FALSE(Has(args, "--tools", ""));
  EXPECT_EQ(std::string(kClaudeCodeOwnTools).find("Agent"), std::string::npos);
  EXPECT_EQ(std::string(kClaudeCodeOwnTools).find("Task"), std::string::npos);
}

TEST(ClaudeCodeTest, PermissionRequests) {
  auto events = ParseClaudeCodeLine(
      R"({"type":"control_request","request_id":"r-1","request":)"
      R"({"subtype":"can_use_tool","tool_name":"Write","input":)"
      R"({"file_path":"/w/a.md","content":"hi"},"tool_use_id":"toolu_9"}})");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, ClaudeCodeEvent::Type::kPermission);
  EXPECT_EQ(events[0].request_id, "r-1");
  EXPECT_EQ(events[0].name, "Write");
  EXPECT_EQ(*Json(events[0].data).FindString("content"), "hi");
  // Other control requests aren't questions.
  EXPECT_TRUE(ParseClaudeCodeLine(
                  R"({"type":"control_request","request_id":"r-2",)"
                  R"("request":{"subtype":"interrupt"}})")
                  .empty());

  base::DictValue allow = Json(ClaudeCodePermissionLine(
      "r-1", /*allow=*/true, R"({"file_path":"/w/a.md","content":"hi"})"));
  EXPECT_EQ(*allow.FindString("type"), "control_response");
  EXPECT_EQ(*allow.FindStringByDottedPath("response.subtype"), "success");
  EXPECT_EQ(*allow.FindStringByDottedPath("response.request_id"), "r-1");
  EXPECT_EQ(*allow.FindStringByDottedPath("response.response.behavior"),
            "allow");
  EXPECT_EQ(*allow.FindStringByDottedPath(
                "response.response.updatedInput.content"),
            "hi");
  base::DictValue deny = Json(ClaudeCodePermissionLine("r-1", false, "{}"));
  EXPECT_EQ(*deny.FindStringByDottedPath("response.response.behavior"),
            "deny");
  EXPECT_TRUE(deny.FindStringByDottedPath("response.response.message"));
  EXPECT_TRUE(ClaudeCodePermissionLine("r-1", false, "{}").ends_with("\n"));
}

TEST(ClaudeCodeTest, DescribesPermissions) {
  ClaudeCodeApproval command = DescribeClaudeCodePermission(
      "Bash", R"({"command":"rm -rf build","description":"Clean up"})");
  EXPECT_EQ(command.title, "Run a command");
  EXPECT_EQ(command.detail, "Clean up\n\nrm -rf build");
  ClaudeCodeApproval write = DescribeClaudeCodePermission(
      "Write", R"({"file_path":"/w/a.md","content":"hi"})");
  EXPECT_EQ(write.title, "Write \u201c/w/a.md\u201d");
  EXPECT_EQ(write.detail, "hi");
  ClaudeCodeApproval fetch = DescribeClaudeCodePermission(
      "WebFetch", R"({"url":"https://example.com","prompt":"Read it"})");
  EXPECT_EQ(fetch.title, "Fetch https://example.com");
  ClaudeCodeApproval other =
      DescribeClaudeCodePermission("NotebookEdit", R"({"a":1})");
  EXPECT_EQ(other.title, "Use NotebookEdit");
  const std::string long_content(5000, 'x');
  ClaudeCodeApproval long_write = DescribeClaudeCodePermission(
      "Write", R"({"file_path":"a","content":")" + long_content + "\"}");
  EXPECT_LT(long_write.detail.size(), 2100u);
}

TEST(ClaudeCodeTest, UserLine) {
  base::ListValue content;
  content.Append(base::DictValue()
                     .Set("type", "tool_result")
                     .Set("tool_use_id", "toolu_1"));
  content.Append(base::DictValue().Set("type", "text").Set("text", "Hi"));
  std::string line = ClaudeCodeUserLine(std::move(content));
  ASSERT_EQ(line.back(), '\n');
  line.pop_back();
  EXPECT_EQ(line.find('\n'), std::string::npos);
  base::DictValue message = Json(line);
  EXPECT_EQ(*message.FindString("type"), "user");
  const base::ListValue& blocks =
      *message.FindListByDottedPath("message.content");
  ASSERT_EQ(blocks.size(), 1u);
  EXPECT_EQ(*blocks[0].GetDict().FindString("text"), "Hi");

  // Only tool results: Claude Code is asked to go on.
  base::ListValue results;
  results.Append(base::DictValue().Set("type", "tool_result"));
  base::DictValue go_on = Json(ClaudeCodeUserLine(std::move(results)));
  EXPECT_EQ(*(*go_on.FindListByDottedPath("message.content"))[0]
                 .GetDict()
                 .FindString("text"),
            "Go on.");
}

TEST(ClaudeCodeTest, DescribesStatus) {
  ClaudeCodeStatus status = DescribeClaudeCodeStatus(
      "2.1.319 (Claude Code)\n",
      R"({"loggedIn":true,"authMethod":"claude.ai","subscriptionType":)"
      R"("max","email":"ada@example.com"})");
  EXPECT_TRUE(status.ok);
  EXPECT_EQ(status.text,
            "Claude Code 2.1.319, signed in with your Claude max plan "
            "(ada@example.com).");

  status = DescribeClaudeCodeStatus("2.1.319 (Claude Code)",
                                    R"({"loggedIn":false})");
  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.text.find("isn't signed in"), std::string::npos);

  EXPECT_FALSE(DescribeClaudeCodeStatus("", "").ok);
}

TEST(ClaudeCodeTest, DescribesExit) {
  EXPECT_EQ(DescribeClaudeCodeExit(1, ""), "Claude Code stopped (exit code 1).");
  EXPECT_EQ(DescribeClaudeCodeExit(1, "  error: unknown option\n"),
            "Claude Code stopped: error: unknown option");
}

}  // namespace
}  // namespace compass::claude
