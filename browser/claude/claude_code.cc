// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_code.h"

#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"

namespace compass::claude {

namespace {

// As the panel's own loop (app.ts MAX_TOOL_ROUNDS).
constexpr int kMaxTurns = 40;

#if BUILDFLAG(IS_WIN)
constexpr base::FilePath::CharType kClaudeExe[] = FILE_PATH_LITERAL("claude.exe");
#else
constexpr base::FilePath::CharType kClaudeExe[] = FILE_PATH_LITERAL("claude");
#endif

std::string ToJson(const base::Value& value) {
  std::string json;
  base::JSONWriter::Write(value, &json);
  return json;
}

// A tool_result's content as blocks: Claude Code gives a string or blocks.
base::ListValue ResultBlocks(base::Value* content) {
  base::ListValue blocks;
  if (!content) {
    return blocks;
  }
  if (content->is_string()) {
    blocks.Append(base::DictValue()
                      .Set("type", "text")
                      .Set("text", std::move(content->GetString())));
  } else if (content->is_list()) {
    for (base::Value& block : content->GetList()) {
      if (block.is_dict()) {
        blocks.Append(std::move(block));
      }
    }
  }
  return blocks;
}

std::string ResultError(const base::DictValue& line) {
  const std::string* subtype = line.FindString("subtype");
  if (subtype && *subtype == "error_max_turns") {
    return base::StrCat({"Claude stopped after ",
                         base::NumberToString(kMaxTurns),
                         " steps. Send \"go on\" to let it continue."});
  }
  std::vector<std::string> errors;
  if (const base::ListValue* list = line.FindList("errors")) {
    for (const base::Value& error : *list) {
      if (error.is_string() && !error.GetString().empty()) {
        errors.push_back(error.GetString());
      }
    }
  }
  if (!errors.empty()) {
    return base::JoinString(errors, "\n");
  }
  // An API error (signed out, overloaded) comes as the result's text.
  if (const std::string* result = line.FindString("result");
      result && !result->empty()) {
    return *result;
  }
  return base::StrCat(
      {"Claude Code stopped (", subtype ? *subtype : "error", ")."});
}

}  // namespace

ClaudeCodeOptions::ClaudeCodeOptions() = default;
ClaudeCodeOptions::ClaudeCodeOptions(const ClaudeCodeOptions&) = default;
ClaudeCodeOptions& ClaudeCodeOptions::operator=(const ClaudeCodeOptions&) =
    default;
ClaudeCodeOptions::~ClaudeCodeOptions() = default;

ClaudeCodeEvent::ClaudeCodeEvent() = default;
ClaudeCodeEvent::ClaudeCodeEvent(ClaudeCodeEvent&&) = default;
ClaudeCodeEvent& ClaudeCodeEvent::operator=(ClaudeCodeEvent&&) = default;
ClaudeCodeEvent::~ClaudeCodeEvent() = default;

std::vector<base::FilePath> ClaudeCodeCandidates() {
  base::FilePath home;
  base::PathService::Get(base::DIR_HOME, &home);
  // The native installer's, then the older local install's, then package
  // managers'. A GUI app doesn't get the shell's PATH, so they're named.
  std::vector<base::FilePath> candidates = {
      home.Append(FILE_PATH_LITERAL(".local"))
          .Append(FILE_PATH_LITERAL("bin"))
          .Append(kClaudeExe),
      home.Append(FILE_PATH_LITERAL(".claude"))
          .Append(FILE_PATH_LITERAL("local"))
          .Append(kClaudeExe),
  };
#if BUILDFLAG(IS_MAC)
  candidates.emplace_back("/opt/homebrew/bin/claude");
  candidates.emplace_back("/usr/local/bin/claude");
#elif BUILDFLAG(IS_LINUX)
  candidates.emplace_back("/usr/local/bin/claude");
  candidates.emplace_back("/usr/bin/claude");
#endif
  return candidates;
}

std::optional<base::FilePath> FindClaudeCode(
    const base::FilePath& configured) {
  if (!configured.empty()) {
    if (base::PathExists(configured) && !base::DirectoryExists(configured)) {
      return configured;
    }
    return std::nullopt;
  }
  for (const base::FilePath& candidate : ClaudeCodeCandidates()) {
    if (base::PathExists(candidate) && !base::DirectoryExists(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::string ClaudeCodeMcpConfig(const ClaudeCodeOptions& options) {
  base::ListValue args;
  args.Append(base::StrCat(
      {"--user-data-dir=", options.user_data_dir.AsUTF8Unsafe()}));
  args.Append(base::StrCat({"--chat=", options.chat_token}));
  return ToJson(base::Value(base::DictValue().Set(
      "mcpServers",
      base::DictValue().Set(
          "compass", base::DictValue()
                         .Set("command", options.mcp_relay.AsUTF8Unsafe())
                         .Set("args", std::move(args))))));
}

std::vector<std::string> ClaudeCodeArgs(const ClaudeCodeOptions& options) {
  std::vector<std::string> args = {
      "-p",
      "--input-format",
      "stream-json",
      "--output-format",
      "stream-json",
      "--verbose",
      "--include-partial-messages",
      "--max-turns",
      base::NumberToString(kMaxTurns),
      // Compass's prompt, not Claude Code's coding one.
      "--system-prompt",
      options.system_prompt,
      // None of Claude Code's own tools: no files, shell or web.
      "--tools",
      "",
      // Not the user's settings, hooks or MCP servers either.
      "--setting-sources",
      "",
      "--strict-mcp-config",
  };
  if (!options.model.empty()) {
    args.insert(args.end(), {"--model", options.model});
  }
  if (!options.mcp_relay.empty()) {
    args.insert(args.end(), {"--mcp-config", ClaudeCodeMcpConfig(options),
                             // Compass checks each call itself (ClaudeTools).
                             "--allowedTools", "mcp__compass"});
  }
  if (!options.resume.empty()) {
    args.insert(args.end(), {"--resume", options.resume});
  }
  return args;
}

std::string ClaudeCodeUserLine(base::ListValue content) {
  base::ListValue blocks;
  for (base::Value& block : content) {
    const std::string* type =
        block.is_dict() ? block.GetDict().FindString("type") : nullptr;
    if (type && *type != "tool_result") {
      blocks.Append(std::move(block));
    }
  }
  if (blocks.empty()) {
    // Only results of tools Claude Code no longer waits for (Retry after a
    // failed step): it goes on from where it was.
    blocks.Append(
        base::DictValue().Set("type", "text").Set("text", "Go on."));
  }
  return ToJson(base::Value(
             base::DictValue()
                 .Set("type", "user")
                 .Set("message", base::DictValue()
                                     .Set("role", "user")
                                     .Set("content", std::move(blocks))))) +
         "\n";
}

std::vector<ClaudeCodeEvent> ParseClaudeCodeLine(std::string_view line) {
  std::vector<ClaudeCodeEvent> events;
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(line, base::JSON_PARSE_RFC);
  if (!dict) {
    return events;
  }
  const std::string* type = dict->FindString("type");
  if (!type) {
    return events;
  }
  if (*type == "system") {
    const std::string* subtype = dict->FindString("subtype");
    const std::string* session = dict->FindString("session_id");
    if (subtype && *subtype == "init" && session) {
      ClaudeCodeEvent& event = events.emplace_back();
      event.type = ClaudeCodeEvent::Type::kInit;
      event.session_id = *session;
    }
    return events;
  }
  if (*type == "stream_event") {
    // A subagent's events (parent_tool_use_id) aren't the reply's; Claude
    // Code's tools are off, so none should come.
    if (const base::Value* parent = dict->Find("parent_tool_use_id");
        parent && !parent->is_none()) {
      return events;
    }
    base::DictValue* event_dict = dict->FindDict("event");
    const std::string* name =
        event_dict ? event_dict->FindString("type") : nullptr;
    if (!name || *name == "ping") {
      return events;
    }
    if (*name == "content_block_start") {
      if (std::string* tool =
              event_dict->FindStringByDottedPath("content_block.name")) {
        if (tool->starts_with(kToolPrefix)) {
          tool->erase(0, sizeof(kToolPrefix) - 1);
        }
      }
    }
    ClaudeCodeEvent& event = events.emplace_back();
    event.type = ClaudeCodeEvent::Type::kStreamEvent;
    event.name = *name;
    event.data = ToJson(base::Value(std::move(*event_dict)));
    return events;
  }
  if (*type == "user") {
    base::ListValue* content = dict->FindListByDottedPath("message.content");
    if (!content) {
      return events;
    }
    for (base::Value& block : *content) {
      if (!block.is_dict()) {
        continue;
      }
      base::DictValue& result = block.GetDict();
      const std::string* block_type = result.FindString("type");
      const std::string* id = result.FindString("tool_use_id");
      if (!block_type || *block_type != "tool_result" || !id) {
        continue;
      }
      ClaudeCodeEvent& event = events.emplace_back();
      event.type = ClaudeCodeEvent::Type::kToolResult;
      event.tool_use_id = *id;
      event.data = ToJson(base::Value(
          base::DictValue()
              .Set("content", ResultBlocks(result.Find("content")))
              .Set("is_error", result.FindBool("is_error").value_or(false))));
    }
    return events;
  }
  if (*type == "result") {
    ClaudeCodeEvent& event = events.emplace_back();
    event.type = ClaudeCodeEvent::Type::kResult;
    if (dict->FindBool("is_error").value_or(false)) {
      event.error = ResultError(*dict);
    }
    return events;
  }
  return events;
}

ClaudeCodeStatus DescribeClaudeCodeStatus(std::string_view version,
                                          std::string_view auth_status) {
  ClaudeCodeStatus status;
  // "2.1.319 (Claude Code)"
  std::string_view number = base::TrimWhitespaceASCII(version, base::TRIM_ALL);
  number = number.substr(0, number.find(' '));
  std::optional<base::DictValue> auth =
      base::JSONReader::ReadDict(auth_status, base::JSON_PARSE_RFC);
  if (number.empty() || !auth) {
    status.text = "Claude Code didn't answer as expected. Update it (claude "
                  "update) and test again.";
    return status;
  }
  if (!auth->FindBool("loggedIn").value_or(false)) {
    status.text = base::StrCat(
        {"Claude Code ", number,
         " isn't signed in. Run claude in Terminal, sign in with your "
         "Claude account, then test again."});
    return status;
  }
  status.ok = true;
  const std::string* method = auth->FindString("authMethod");
  const std::string* plan = auth->FindString("subscriptionType");
  const std::string* email = auth->FindString("email");
  std::string how;
  if (method && *method == "claude.ai") {
    how = plan && !plan->empty()
              ? base::StrCat({"signed in with your Claude ", *plan, " plan"})
              : "signed in with your Claude account";
  } else {
    how = "signed in";
  }
  status.text = base::StrCat({"Claude Code ", number, ", ", how,
                              email && !email->empty()
                                  ? base::StrCat({" (", *email, ")"})
                                  : std::string(),
                              "."});
  return status;
}

ClaudeCodeStatus CheckClaudeCode(const base::FilePath& program) {
  base::CommandLine version_command(program);
  version_command.AppendArg("--version");
  std::string version;
  base::GetAppOutputAndError(version_command, &version);
  base::CommandLine auth_command(program);
  auth_command.AppendArg("auth");
  auth_command.AppendArg("status");
  std::string auth;
  // Non-zero when signed out; the JSON says so too.
  base::GetAppOutput(auth_command, &auth);
  return DescribeClaudeCodeStatus(version, auth);
}

std::string DescribeClaudeCodeExit(int exit_code, std::string_view output) {
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(output, base::TRIM_ALL);
  if (trimmed.empty()) {
    return base::StrCat({"Claude Code stopped (exit code ",
                         base::NumberToString(exit_code), ")."});
  }
  return base::StrCat({"Claude Code stopped: ", trimmed});
}

}  // namespace compass::claude
