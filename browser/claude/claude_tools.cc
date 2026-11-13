// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_tools.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/no_destructor.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/types/expected_macros.h"
#include "build/build_config.h"
#include "compass/browser/claude/claude_activity.h"
#include "compass/browser/claude/claude_assets.h"
#include "compass/browser/claude/claude_files.h"
#include "compass/browser/claude/devtools_session.h"
#include "compass/browser/claude/key_events.h"
#include "compass/browser/claude/page_reader.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "third_party/re2/src/re2/re2.h"
#include "ui/base/page_transition_types.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"

namespace compass::claude {

namespace {

// A tool that takes longer is stopped (a hung page, a dropped reply).
constexpr base::TimeDelta kToolTimeout = base::Seconds(30);
constexpr base::TimeDelta kNavigateTimeout = base::Seconds(15);
// The tab is let go after this long without tools.
constexpr base::TimeDelta kIdle = base::Minutes(1);
// The tint stays this long after a tool, for the next step.
constexpr base::TimeDelta kActingLinger = base::Seconds(2);
// After a click or key, before reporting: the page reacts.
constexpr base::TimeDelta kSettle = base::Milliseconds(300);
// Screenshots' longest side, in pixels: legible, and about 1,200 tokens.
constexpr double kScreenshotMaxEdge = 1280;
constexpr size_t kConsoleSize = 500;
// run_js results longer than this are cut.
constexpr size_t kMaxJsChars = 20000;

std::string Describe(const base::DictValue& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

std::string Json(const base::Value& value) {
  std::string json;
  base::JSONWriter::WriteWithOptions(
      value, base::JSONWriter::OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION, &json);
  return json;
}

// The page agent's description of the page: title, URL, viewport.
std::string PageHeader(const base::DictValue& page) {
  std::string header = base::StrCat({"Title: ", Describe(page, "title"),
                                     "\nURL: ", Describe(page, "url")});
  if (const base::DictValue* viewport = page.FindDict("viewport")) {
    base::StrAppend(
        &header,
        {base::StringPrintf(
            "\nViewport: %dx%d, scrolled to %d,%d of %dx%d",
            viewport->FindInt("width").value_or(0),
            viewport->FindInt("height").value_or(0),
            viewport->FindInt("scrollX").value_or(0),
            viewport->FindInt("scrollY").value_or(0),
            viewport->FindInt("scrollWidth").value_or(0),
            viewport->FindInt("scrollHeight").value_or(0))});
  }
  return header;
}

int ButtonModifiers(const std::string* modifiers) {
  if (!modifiers) {
    return 0;
  }
  // "cmd+shift": ParseKeys knows the names; a dummy key carries them.
  auto parsed = ParseKeys(*modifiers + "+a", /*mac=*/false);
  return parsed.has_value() ? parsed->front().modifiers : 0;
}

std::string ConsoleLevel(const std::string& type) {
  if (type == "warning" || type == "error" || type == "info" ||
      type == "debug") {
    return type;
  }
  if (type == "assert") {
    return "error";
  }
  if (type == "verbose") {
    return "debug";
  }
  return "log";
}

// A console argument as text: its value, or its description.
std::string ArgText(const base::DictValue& arg) {
  if (const base::Value* value = arg.Find("value")) {
    return value->is_string() ? value->GetString() : Json(*value);
  }
  if (const std::string* description = arg.FindString("description")) {
    return *description;
  }
  return Describe(arg, "type");
}

std::string Location(const std::string& url, std::optional<int> line) {
  if (url.empty()) {
    return std::string();
  }
  return line ? base::StrCat({url, ":", base::NumberToString(*line + 1)})
              : url;
}

constexpr char kNotPermitted[] =
    "Not permitted: the user hasn't turned on \"Let Claude use the whole "
    "browser\" (Compass settings, Claude). Without it, Claude can only use "
    "the tab the chat is about. Tell the user if the task needs more.";

// tools.json's entries for this platform, parsed once.
// Where this build runs, as tools.json's "platforms" name it.
#if BUILDFLAG(IS_ANDROID)
constexpr char kPlatform[] = "android";
#else
constexpr char kPlatform[] = "desktop";
#endif

const base::ListValue& ToolEntries() {
  static const base::NoDestructor<base::ListValue> entries([] {
    base::ListValue entries;
    std::optional<base::DictValue> file =
        base::JSONReader::ReadDict(assets::kTools, base::JSON_PARSE_RFC);
    CHECK(file);
    for (base::Value& tool : *file->FindList("tools")) {
      const base::ListValue* platforms = tool.GetDict().FindList("platforms");
      if (platforms &&
          !std::ranges::contains(*platforms, base::Value(kPlatform))) {
        continue;
      }
      entries.Append(std::move(tool));
    }
    return entries;
  }());
  return *entries;
}

std::string ScopeOf(const std::string& name) {
  for (const base::Value& tool : ToolEntries()) {
    const base::DictValue& dict = tool.GetDict();
    const std::string* tool_name = dict.FindStringByDottedPath("definition.name");
    if (tool_name && *tool_name == name) {
      return Describe(dict, "scope");
    }
  }
  return std::string();
}

bool IsBrowserTool(const std::string& name) {
  return ScopeOf(name) == "browser";
}

bool IsMemoryTool(const std::string& name) {
  return ScopeOf(name) == "memory";
}

bool IsFilesTool(const std::string& name) {
  return ScopeOf(name) == "files";
}

bool IsDevToolsTool(const std::string& name) {
  return ScopeOf(name) == "devtools";
}

constexpr char kDevToolsOff[] =
    "Not permitted: the user hasn't turned on \"Let Claude use developer "
    "tools\" (Compass settings, Claude). Tell the user if the task needs "
    "it.";

constexpr char kFilesOff[] =
    "Not permitted: the user hasn't turned on file access (Compass settings, "
    "Claude, Files). Tell the user if the task needs it.";
constexpr char kNoFolder[] =
    "Not permitted: the user hasn't chosen a folder for Claude yet (Compass "
    "settings, Claude, Files). Tell the user if the task needs it.";
constexpr char kSaveOnly[] =
    "Not permitted: Claude may only save new files (save_file). With \"Use "
    "a folder\" in Compass settings (Claude, Files), it can also list, read "
    "and write files in one folder. Tell the user if the task needs it.";
constexpr char kNotAllowed[] = "The user didn't allow it.";
// How much of a file the approval shows.
constexpr size_t kApprovalDetailChars = 2000;

std::string ApprovalDetail(std::string_view content) {
  std::u16string text = base::UTF8ToUTF16(content);
  if (text.size() <= kApprovalDetailChars) {
    return std::string(content);
  }
  text.resize(kApprovalDetailChars);
  return base::StrCat({base::UTF16ToUTF8(text), "\n…"});
}

std::string FolderName(const base::FilePath& folder) {
  return folder.BaseName().AsUTF8Unsafe();
}

}  // namespace

TabsDelegate::Result::Result() = default;
TabsDelegate::Result::Result(Result&&) = default;
TabsDelegate::Result& TabsDelegate::Result::operator=(Result&&) = default;
TabsDelegate::Result::~Result() = default;

MemoryChange::MemoryChange() = default;
MemoryChange::MemoryChange(const MemoryChange&) = default;
MemoryChange& MemoryChange::operator=(const MemoryChange&) = default;
MemoryChange::~MemoryChange() = default;

ToolResult::ToolResult() = default;
ToolResult::ToolResult(ToolResult&&) = default;
ToolResult& ToolResult::operator=(ToolResult&&) = default;
ToolResult::~ToolResult() = default;

// static
ToolResult ToolResult::Text(std::string text) {
  ToolResult result;
  result.content.Append(
      base::DictValue().Set("type", "text").Set("text", std::move(text)));
  return result;
}

// static
ToolResult ToolResult::Error(std::string text) {
  ToolResult result = Text(std::move(text));
  result.is_error = true;
  return result;
}

base::DictValue ToolResult::ToDict() && {
  return base::DictValue()
      .Set("content", std::move(content))
      .Set("is_error", is_error);
}

ClaudeTools::ClaudeTools(std::unique_ptr<TabsDelegate> tabs)
    : tabs_(std::move(tabs)) {}

ClaudeTools::~ClaudeTools() {
  // A pending callback goes back to its caller, which may be going away
  // too: run it before the session it may use is gone.
  if (callback_) {
    std::move(callback_).Run(ToolResult::Error("Stopped."));
  }
  StopActing();
}

void ClaudeTools::SetActing(content::WebContents* contents) {
  acting_linger_.Stop();
  if (acting_.get() == contents) {
    return;
  }
  StopActing();
  if (contents) {
    ClaudeActivity::Begin(contents);
    acting_ = contents->GetWeakPtr();
  }
}

void ClaudeTools::StopActing() {
  acting_linger_.Stop();
  if (acting_) {
    ClaudeActivity::End(acting_.get());
  }
  acting_.reset();
}

// static
base::ListValue ClaudeTools::Definitions(bool memory,
                                         bool files,
                                         bool devtools) {
  base::ListValue definitions;
  for (const base::Value& tool : ToolEntries()) {
    const std::string scope = Describe(tool.GetDict(), "scope");
    if ((!memory && scope == "memory") || (!files && scope == "files") ||
        (!devtools && scope == "devtools")) {
      continue;
    }
    definitions.Append(tool.GetDict().FindDict("definition")->Clone());
  }
  if (!definitions.empty()) {
    // The tools are the same for every request: cached with the prompt.
    definitions.back().GetDict().Set(
        "cache_control", base::DictValue().Set("type", "ephemeral"));
  }
  return definitions;
}

void ClaudeTools::SetFiles(FilesConfigGetter config, Approver approve) {
  files_config_ = std::move(config);
  approve_ = std::move(approve);
}

void ClaudeTools::SetDevTools(base::RepeatingCallback<bool()> allowed) {
  devtools_allowed_ = std::move(allowed);
}

bool ClaudeTools::CheckDevTools() {
  if (!devtools_allowed_) {
    Finish(ToolResult::Error("The developer tools aren't available here."));
    return false;
  }
  if (!devtools_allowed_.Run()) {
    Finish(ToolResult::Error(kDevToolsOff));
    return false;
  }
  return true;
}

void ClaudeTools::SetMemory(ClaudeMemory* memory,
                            base::RepeatingCallback<bool()> may_save) {
  memory_ = memory ? memory->GetWeakPtr() : nullptr;
  may_save_memory_ = std::move(may_save);
}

ToolResult ClaudeTools::RunMemoryTool(const std::string& name,
                                      const base::DictValue& input) {
  if (!memory_) {
    return ToolResult::Error("Memory isn't available here.");
  }
  if (may_save_memory_ && !may_save_memory_.Run()) {
    return ToolResult::Error(
        "The user turned off \"Let Claude save memories\" in Compass's "
        "Claude settings.");
  }
  const std::string text = Describe(input, "text");
  const std::string id = Describe(input, "id");
  MemoryChange change;
  if (name == "remember") {
    const size_t before = memory_->entries().size();
    base::expected<MemoryEntry, std::string> entry = memory_->Add(text);
    if (!entry.has_value()) {
      return ToolResult::Error(entry.error());
    }
    const std::string short_id = entry->id.substr(0, 8);
    if (memory_->entries().size() == before) {
      return ToolResult::Text(
          base::StrCat({"Already remembered, as [", short_id, "]."}));
    }
    change.kind = "saved";
    change.id = entry->id;
    change.text = entry->text;
    ToolResult result =
        ToolResult::Text(base::StrCat({"Saved, as [", short_id, "]."}));
    result.memory_change = std::move(change);
    return result;
  }
  const MemoryEntry* existing = memory_->Find(id);
  if (!existing) {
    return ToolResult::Error(base::StrCat(
        {"No memory has the id ", id,
         ". It may have been deleted since the chat started."}));
  }
  change.before = *existing;
  change.id = existing->id;
  if (name == "update_memory") {
    base::expected<MemoryEntry, std::string> entry =
        memory_->Update(change.id, text);
    if (!entry.has_value()) {
      return ToolResult::Error(entry.error());
    }
    change.kind = "updated";
    change.text = entry->text;
    ToolResult result = ToolResult::Text("Updated.");
    result.memory_change = std::move(change);
    return result;
  }
  memory_->Remove(change.id);
  change.kind = "forgot";
  change.text = change.before->text;
  ToolResult result = ToolResult::Text("Forgotten.");
  result.memory_change = std::move(change);
  return result;
}

void ClaudeTools::SetTarget(content::WebContents* contents) {
  if (contents == web_contents()) {
    return;
  }
  Stop();
  task_tab_ = contents ? contents->GetWeakPtr() : nullptr;
  Observe(contents);
  console_.clear();
  console_read_seq_ = 0;
  network_.clear();
  network_read_seq_ = 0;
  screenshot_scale_ = 1;
}

void ClaudeTools::Retarget(content::WebContents* contents) {
  if (contents == web_contents()) {
    return;
  }
  ResetDevTools();
  capture_.RunAndReset();
  Observe(contents);
  console_.clear();
  console_read_seq_ = 0;
  network_.clear();
  network_read_seq_ = 0;
  screenshot_scale_ = 1;
  if (acting_) {
    // The tint follows Claude to the new tab.
    SetActing(contents);
  }
  if (contents) {
    capture_ = contents->IncrementCapturerCount(
        gfx::Size(), /*stay_hidden=*/true, /*stay_awake=*/false,
        /*is_activity=*/false);
  }
}

bool ClaudeTools::CheckBrowserScope() {
  if (!tabs_) {
    Finish(ToolResult::Error("The browser tools aren't available here."));
    return false;
  }
  if (!tabs_->BrowserScopeAllowed()) {
    Finish(ToolResult::Error(kNotPermitted));
    return false;
  }
  return true;
}

void ClaudeTools::Stop() {
  run_weak_factory_.InvalidateWeakPtrs();
  timeout_.Stop();
  delay_.Stop();
  on_loaded_.Reset();
  if (callback_) {
    std::move(callback_).Run(ToolResult::Error("Stopped by the user."));
  }
  StopActing();
  Release();
}

void ClaudeTools::Release() {
  idle_.Stop();
  // Letting go of the session also undoes what emulate changed.
  ResetDevTools();
  capture_.RunAndReset();
}

void ClaudeTools::ResetDevTools() {
  devtools_.reset();
  network_enabled_ = false;
}

void ClaudeTools::Run(const std::string& name,
                      base::DictValue input,
                      Callback callback) {
  if (callback_) {
    Stop();
  }
  callback_ = std::move(callback);
  if (IsMemoryTool(name)) {
    // No tab, no permission: the profile's memory.
    std::move(callback_).Run(RunMemoryTool(name, input));
    return;
  }
  if (IsFilesTool(name)) {
    // No tab: files, with the setting and the user's approval. No timeout
    // either: the user may take a while to decide.
    RunFileTool(name, std::move(input));
    return;
  }
  if (IsDevToolsTool(name) && !CheckDevTools()) {
    return;
  }
  content::WebContents* contents = web_contents();
  const bool browser_tool = IsBrowserTool(name);
  if (browser_tool) {
    if (!CheckBrowserScope()) {
      return;
    }
  } else {
    if (!contents) {
      Finish(ToolResult::Error(
          "The tab Claude was using is closed. With \"Let Claude use the "
          "whole browser\" on, select_tab picks another."));
      return;
    }
    // Another tab than the task's (select_tab, open_tab) needs the
    // permission, now: it may have been turned off since.
    if (contents != task_tab_.get() &&
        !(tabs_ && tabs_->BrowserScopeAllowed())) {
      Finish(ToolResult::Error(kNotPermitted));
      return;
    }
    if (name != "navigate" && !CanUsePage(contents->GetLastCommittedURL())) {
      Finish(ToolResult::Error(
          "Claude can't use this page: it's one of the browser's own pages "
          "(settings, passwords, extensions) or the Web Store. Navigate to a "
          "web page first."));
      return;
    }
    SetActing(contents);
    // Keeps the tab rendering while Claude works on it in the background.
    if (!capture_) {
      capture_ = contents->IncrementCapturerCount(
          gfx::Size(), /*stay_hidden=*/true, /*stay_awake=*/false,
          /*is_activity=*/false);
    }
  }
  idle_.Stop();
  timeout_.Start(FROM_HERE,
                 name == "wait" || name == "navigate" || name == "open_tab"
                     ? kToolTimeout + kNavigateTimeout
                     : kToolTimeout,
                 base::BindOnce(&ClaudeTools::Finish,
                                run_weak_factory_.GetWeakPtr(),
                                ToolResult::Error(
                                    "The page didn't answer in time.")));
  url_before_ = contents ? contents->GetLastCommittedURL().spec() : "";

  using Handler = void (ClaudeTools::*)(base::DictValue);
  static constexpr std::pair<const char*, Handler> kHandlers[] = {
      {"read_page", &ClaudeTools::ReadPage},
      {"get_page_text", &ClaudeTools::GetPageText},
      {"find", &ClaudeTools::Find},
      {"screenshot", &ClaudeTools::Screenshot},
      {"read_console", &ClaudeTools::ReadConsole},
      {"run_js", &ClaudeTools::RunJs},
      {"click", &ClaudeTools::Click},
      {"hover", &ClaudeTools::Hover},
      {"type", &ClaudeTools::Type},
      {"key", &ClaudeTools::Key},
      {"scroll", &ClaudeTools::Scroll},
      {"form_input", &ClaudeTools::FormInput},
      {"navigate", &ClaudeTools::Navigate},
      {"wait", &ClaudeTools::Wait},
      {"list_tabs", &ClaudeTools::ListTabs},
      {"open_tab", &ClaudeTools::OpenTab},
      {"select_tab", &ClaudeTools::SelectTab},
      {"close_tab", &ClaudeTools::CloseTab},
      {"network_requests", &ClaudeTools::NetworkRequests},
      {"network_response", &ClaudeTools::NetworkResponse},
  };
  for (const auto& [tool, handler] : kHandlers) {
    if (name == tool) {
      if (!browser_tool) {
        // The console is gathered from the first tool on: attach now.
        devtools().Attach();
      }
      (this->*handler)(std::move(input));
      return;
    }
  }
  if (browser_tool) {
    RunBrowserTool(name, std::move(input));
    return;
  }
  Finish(ToolResult::Error(
      base::StrCat({"Tool ", name, " isn't available here."})));
}

TabsDelegate::Context ClaudeTools::MakeContext() {
  TabsDelegate::Context context;
  context.target = web_contents();
  context.opened = &opened_;
  return context;
}

void ClaudeTools::RunBrowserTool(const std::string& name,
                                 base::DictValue input) {
  tabs_->RunTool(name, std::move(input), MakeContext(),
                 base::BindOnce(&ClaudeTools::OnBrowserToolDone,
                                run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::OnBrowserToolDone(TabsDelegate::Result result) {
  if (result.act_on) {
    Retarget(result.act_on.get());
  }
  Finish(std::move(result.result));
}

void ClaudeTools::RunFileTool(const std::string& name, base::DictValue input) {
  if (!files_config_) {
    Finish(ToolResult::Error("File tools aren't available here."));
    return;
  }
  FilesConfig config = files_config_.Run();
  if (config.access == prefs::FilesAccess::kOff) {
    Finish(ToolResult::Error(kFilesOff));
    return;
  }
  if (config.folder.empty()) {
    Finish(ToolResult::Error(kNoFolder));
    return;
  }
  if (name != "save_file" && config.access != prefs::FilesAccess::kFolder) {
    Finish(ToolResult::Error(kSaveOnly));
    return;
  }
  if (!file_runner_) {
    file_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
         base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
  }
  const std::string path = Describe(input, "path");
  const std::string content = Describe(input, "content");
  if (name == "list_files") {
    file_runner_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(
            [](base::FilePath folder, std::string path) {
              return files::ListFolder(folder, path);
            },
            config.folder, path),
        base::BindOnce(&ClaudeTools::FinishWithText,
                       run_weak_factory_.GetWeakPtr()));
    return;
  }
  if (name == "read_file") {
    const int offset = std::max(0, input.FindInt("offset").value_or(0));
    file_runner_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(
            [](base::FilePath folder, std::string path, size_t offset) {
              return files::ReadTextFile(folder, path, offset);
            },
            config.folder, path, static_cast<size_t>(offset)),
        base::BindOnce(&ClaudeTools::FinishWithText,
                       run_weak_factory_.GetWeakPtr()));
    return;
  }
  if (std::optional<std::string> error = files::CheckContent(content)) {
    Finish(ToolResult::Error(*error));
    return;
  }
  if (name == "save_file") {
    const std::string file_name = Describe(input, "name");
    if (std::optional<std::string> error = files::CheckFileName(file_name)) {
      Finish(ToolResult::Error(*error));
      return;
    }
    ApprovalRequest request;
    request.tool = name;
    request.title = base::StrCat({"Save \u201c", file_name, "\u201d in ",
                                  FolderName(config.folder)});
    request.detail = ApprovalDetail(content);
    ApproveThenWrite(
        std::move(request),
        base::BindOnce(
            [](base::FilePath folder, std::string name, std::string content)
                -> base::expected<std::string, std::string> {
              ASSIGN_OR_RETURN(base::FilePath saved,
                               files::SaveNewFile(folder, name, content));
              return base::ok(
                  base::StrCat({"Saved as ", saved.AsUTF8Unsafe(), "."}));
            },
            config.folder, file_name, content));
    return;
  }
  // write_file: is it a new file or a replaced one? The approval says.
  base::expected<base::FilePath, std::string> full =
      files::JoinInFolder(config.folder, path);
  if (!full.has_value() || path.empty()) {
    Finish(ToolResult::Error(full.has_value() ? "write_file needs a path."
                                              : full.error()));
    return;
  }
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath folder,
             base::FilePath full) -> base::expected<bool, std::string> {
            RETURN_IF_ERROR(files::CheckStaysInFolder(folder, full));
            return files::FileExists(full);
          },
          config.folder, *full),
      base::BindOnce(&ClaudeTools::OnWriteChecked,
                     run_weak_factory_.GetWeakPtr(), config, path, content));
}

void ClaudeTools::OnWriteChecked(FilesConfig config,
                                 std::string path,
                                 std::string content,
                                 base::expected<bool, std::string> exists) {
  if (!exists.has_value()) {
    Finish(ToolResult::Error(exists.error()));
    return;
  }
  ApprovalRequest request;
  request.tool = "write_file";
  request.title = base::StrCat({*exists ? "Replace \u201c" : "Create \u201c",
                                files::DisplayPath(path), "\u201d in ",
                                FolderName(config.folder)});
  request.detail = ApprovalDetail(content);
  ApproveThenWrite(
      std::move(request),
      base::BindOnce(
          [](base::FilePath folder, std::string path, std::string content)
              -> base::expected<std::string, std::string> {
            ASSIGN_OR_RETURN(bool replaced,
                             files::WriteTextFile(folder, path, content));
            return base::ok(base::StrCat({replaced ? "Replaced " : "Created ",
                                          files::DisplayPath(path), "."}));
          },
          config.folder, path, content));
}

void ClaudeTools::ApproveThenWrite(
    ApprovalRequest request,
    base::OnceCallback<base::expected<std::string, std::string>()> write) {
  if (!approve_) {
    Finish(ToolResult::Error("Compass can't ask the user here."));
    return;
  }
  approve_.Run(std::move(request),
               base::BindOnce(&ClaudeTools::OnApproval,
                              run_weak_factory_.GetWeakPtr(),
                              std::move(write)));
}

void ClaudeTools::ApproveThen(ApprovalRequest request,
                              base::OnceClosure then) {
  if (!approve_) {
    Finish(ToolResult::Error("Compass can't ask the user here."));
    return;
  }
  // No timeout while the user decides; Stop() still ends it.
  timeout_.Stop();
  approve_.Run(std::move(request),
               base::BindOnce(
                   [](base::WeakPtr<ClaudeTools> self, base::OnceClosure then,
                      bool allowed) {
                     if (!self) {
                       return;
                     }
                     if (!allowed) {
                       self->Finish(ToolResult::Error(kNotAllowed));
                       return;
                     }
                     std::move(then).Run();
                   },
                   run_weak_factory_.GetWeakPtr(), std::move(then)));
}

void ClaudeTools::OnApproval(
    base::OnceCallback<base::expected<std::string, std::string>()> write,
    bool allowed) {
  if (!allowed) {
    Finish(ToolResult::Error(kNotAllowed));
    return;
  }
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, std::move(write),
      base::BindOnce(&ClaudeTools::FinishWithText,
                     run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::FinishWithText(
    base::expected<std::string, std::string> text) {
  Finish(text.has_value() ? ToolResult::Text(std::move(*text))
                          : ToolResult::Error(std::move(text.error())));
}

void ClaudeTools::Finish(ToolResult result) {
  timeout_.Stop();
  delay_.Stop();
  on_loaded_.Reset();
  run_weak_factory_.InvalidateWeakPtrs();
  idle_.Start(FROM_HERE, kIdle,
              base::BindOnce(&ClaudeTools::Release, base::Unretained(this)));
  if (acting_) {
    acting_linger_.Start(FROM_HERE, kActingLinger,
                         base::BindOnce(&ClaudeTools::StopActing,
                                        base::Unretained(this)));
  }
  if (callback_) {
    std::move(callback_).Run(std::move(result));
  }
}

DevToolsSession& ClaudeTools::devtools() {
  if (!devtools_) {
    devtools_ = std::make_unique<DevToolsSession>(
        web_contents(),
        base::BindRepeating(&ClaudeTools::OnEvent,
                            weak_factory_.GetWeakPtr()),
        base::BindRepeating(&ClaudeTools::OnAttach,
                            weak_factory_.GetWeakPtr()));
  }
  return *devtools_;
}

void ClaudeTools::OnAttach() {
  // Runtime replays the console messages V8 kept from before; Log adds the
  // browser's own (network errors, blocked content). The overlay draws the
  // click indicator.
  for (const char* method :
       {"Runtime.enable", "Log.enable", "DOM.enable", "Overlay.enable"}) {
    devtools_->Send(method, base::DictValue(), base::DoNothing());
  }
  // With the developer tools on, the tab's requests are recorded from the
  // first tool on, as the console is.
  network_enabled_ = false;
  if (devtools_allowed_ && devtools_allowed_.Run()) {
    EnsureNetwork(base::DoNothing());
  }
}

void ClaudeTools::OnEvent(const std::string& method,
                          const base::DictValue& params) {
  if (method.starts_with("Network.")) {
    OnNetworkEvent(method, params);
    return;
  }
  if (method == "Runtime.consoleAPICalled") {
    std::vector<std::string> args;
    if (const base::ListValue* list = params.FindList("args")) {
      for (const base::Value& arg : *list) {
        if (arg.is_dict()) {
          args.push_back(ArgText(arg.GetDict()));
        }
      }
    }
    std::string source;
    if (const base::ListValue* frames =
            params.FindListByDottedPath("stackTrace.callFrames");
        frames && !frames->empty() && (*frames)[0].is_dict()) {
      const base::DictValue& frame = (*frames)[0].GetDict();
      source = Location(Describe(frame, "url"), frame.FindInt("lineNumber"));
    }
    AddConsole(ConsoleLevel(Describe(params, "type")),
               base::JoinString(args, " "), std::move(source),
               params.FindDouble("timestamp").value_or(0));
  } else if (method == "Runtime.exceptionThrown") {
    const base::DictValue* details = params.FindDict("exceptionDetails");
    if (!details) {
      return;
    }
    std::string text;
    if (const std::string* description =
            details->FindStringByDottedPath("exception.description")) {
      text = *description;
    } else {
      text = Describe(*details, "text");
    }
    AddConsole("error", base::StrCat({"Uncaught ", text}),
               Location(Describe(*details, "url"),
                        details->FindInt("lineNumber")),
               params.FindDouble("timestamp").value_or(0));
  } else if (method == "Log.entryAdded") {
    const base::DictValue* entry = params.FindDict("entry");
    if (!entry) {
      return;
    }
    const std::string level = Describe(*entry, "level");
    AddConsole(level == "verbose" ? "debug" : level, Describe(*entry, "text"),
               Location(Describe(*entry, "url"), entry->FindInt("lineNumber")),
               entry->FindDouble("timestamp").value_or(0));
  }
}

void ClaudeTools::AddConsole(std::string level,
                             std::string text,
                             std::string source,
                             double time) {
  if (console_.size() == kConsoleSize) {
    console_.pop_front();
  }
  console_.push_back({next_console_seq_++, std::move(level), std::move(text),
                      std::move(source), time});
}

void ClaudeTools::Agent(const std::string& method,
                        base::DictValue opts,
                        base::OnceCallback<void(Result)> callback) {
  RunPageAgent(web_contents(), method, std::move(opts), std::move(callback));
}

void ClaudeTools::Send(const std::string& method,
                       base::DictValue params,
                       base::OnceCallback<void(Result)> callback) {
  devtools().Send(method, std::move(params), std::move(callback));
}

void ClaudeTools::SendAll(
    std::vector<std::pair<std::string, base::DictValue>> commands,
    base::OnceCallback<void(Result)> callback) {
  if (commands.empty()) {
    std::move(callback).Run(base::DictValue());
    return;
  }
  auto [method, params] = std::move(commands.front());
  commands.erase(commands.begin());
  Send(method, std::move(params),
       base::BindOnce(
           [](base::WeakPtr<ClaudeTools> self,
              std::vector<std::pair<std::string, base::DictValue>> rest,
              base::OnceCallback<void(Result)> callback, Result result) {
             if (!self) {
               return;
             }
             if (!result.has_value() || rest.empty()) {
               std::move(callback).Run(std::move(result));
               return;
             }
             self->SendAll(std::move(rest), std::move(callback));
           },
           run_weak_factory_.GetWeakPtr(), std::move(commands),
           std::move(callback)));
}

void ClaudeTools::Locate(
    const base::DictValue& input,
    base::OnceCallback<void(base::expected<Point, std::string>)> callback) {
  if (const std::string* ref = input.FindString("ref")) {
    Agent("rect", base::DictValue().Set("ref", *ref),
          base::BindOnce(
              [](base::OnceCallback<void(base::expected<Point, std::string>)>
                     callback,
                 Result rect) {
                if (!rect.has_value()) {
                  std::move(callback).Run(base::unexpected(rect.error()));
                  return;
                }
                Point point;
                point.x = rect->FindDouble("x").value_or(0);
                point.y = rect->FindDouble("y").value_or(0);
                point.element = Describe(*rect, "element");
                point.covered = Describe(*rect, "covered");
                std::move(callback).Run(std::move(point));
              },
              std::move(callback)));
    return;
  }
  const base::ListValue* coordinate = input.FindList("coordinate");
  auto is_number = [](const base::Value& value) {
    return value.is_int() || value.is_double();
  };
  if (!coordinate || coordinate->size() != 2 ||
      !is_number((*coordinate)[0]) || !is_number((*coordinate)[1])) {
    std::move(callback).Run(
        base::unexpected("Give a ref, or a coordinate [x, y]."));
    return;
  }
  Point point;
  // Screenshot pixels to CSS pixels.
  point.x = (*coordinate)[0].GetDouble() / screenshot_scale_;
  point.y = (*coordinate)[1].GetDouble() / screenshot_scale_;
  Agent("elementAt", base::DictValue().Set("x", point.x).Set("y", point.y),
        base::BindOnce(
            [](base::OnceCallback<void(base::expected<Point, std::string>)>
                   callback,
               Point point, Result at) {
              if (at.has_value()) {
                point.element = Describe(*at, "element");
              }
              std::move(callback).Run(std::move(point));
            },
            std::move(callback), point));
}

void ClaudeTools::ReadPage(base::DictValue input) {
  Agent("readPage", std::move(input),
        base::BindOnce(
            [](base::WeakPtr<ClaudeTools> self, Result page) {
              if (!self) {
                return;
              }
              if (!page.has_value()) {
                self->Finish(ToolResult::Error(page.error()));
                return;
              }
              std::string text = PageHeader(*page);
              if (const std::string* focused = page->FindString("focused")) {
                base::StrAppend(&text, {"\nFocused: ", *focused});
              }
              base::StrAppend(&text, {"\n\n", Describe(*page, "tree")});
              self->Finish(ToolResult::Text(std::move(text)));
            },
            run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::GetPageText(base::DictValue input) {
  Agent("pageText", std::move(input),
        base::BindOnce(
            [](base::WeakPtr<ClaudeTools> self, Result page) {
              if (!self) {
                return;
              }
              if (!page.has_value()) {
                self->Finish(ToolResult::Error(page.error()));
                return;
              }
              self->Finish(ToolResult::Text(base::StrCat(
                  {PageHeader(*page), "\n\n", Describe(*page, "text")})));
            },
            run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::Find(base::DictValue input) {
  Agent("find", std::move(input),
        base::BindOnce(
            [](base::WeakPtr<ClaudeTools> self, Result found) {
              if (!self) {
                return;
              }
              if (!found.has_value()) {
                self->Finish(ToolResult::Error(found.error()));
                return;
              }
              std::string text;
              const base::ListValue* matches = found->FindList("matches");
              if (!matches || matches->empty()) {
                self->Finish(ToolResult::Text("No matches."));
                return;
              }
              for (const base::Value& match : *matches) {
                const base::DictValue& m = match.GetDict();
                base::StrAppend(&text, {Describe(m, "ref"), " ",
                                        Describe(m, "role"), " \"",
                                        Describe(m, "name"), "\""});
                if (const std::string* state = m.FindString("state");
                    state && !state->empty()) {
                  base::StrAppend(&text, {" (", *state, ")"});
                }
                text += "\n";
              }
              if (found->FindBool("more").value_or(false)) {
                text += "… more: raise max_results or narrow the query.\n";
              }
              self->Finish(ToolResult::Text(std::move(text)));
            },
            run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::Screenshot(base::DictValue input) {
  auto measure = base::BindOnce(
      [](base::WeakPtr<ClaudeTools> self, Result scrolled) {
        if (!self) {
          return;
        }
        if (!scrolled.has_value()) {
          self->Finish(ToolResult::Error(scrolled.error()));
          return;
        }
        self->Send("Page.getLayoutMetrics", base::DictValue(),
                   base::BindOnce(&ClaudeTools::OnLayoutMetrics,
                                  self->run_weak_factory_.GetWeakPtr()));
      },
      run_weak_factory_.GetWeakPtr());
  if (const std::string* ref = input.FindString("ref")) {
    Agent("scrollIntoView", base::DictValue().Set("ref", *ref),
          std::move(measure));
  } else {
    std::move(measure).Run(base::DictValue());
  }
}

void ClaudeTools::OnLayoutMetrics(Result metrics) {
  if (!metrics.has_value()) {
    Finish(ToolResult::Error(metrics.error()));
    return;
  }
  const base::DictValue* viewport = metrics->FindDict("cssVisualViewport");
  if (!viewport) {
    Finish(ToolResult::Error("The page has no viewport."));
    return;
  }
  const double width = viewport->FindDouble("clientWidth").value_or(0);
  const double height = viewport->FindDouble("clientHeight").value_or(0);
  if (width < 1 || height < 1) {
    Finish(ToolResult::Error("The page has no size (is the tab hidden?)."));
    return;
  }
  const double scale =
      std::min(1.0, kScreenshotMaxEdge / std::max(width, height));
  const int out_width = std::floor(width * scale);
  const int out_height = std::floor(height * scale);
  base::DictValue clip;
  clip.Set("x", viewport->FindDouble("pageX").value_or(0));
  clip.Set("y", viewport->FindDouble("pageY").value_or(0));
  clip.Set("width", width);
  clip.Set("height", height);
  clip.Set("scale", scale);
  Send("Page.captureScreenshot",
       base::DictValue()
           .Set("format", "jpeg")
           .Set("quality", 80)
           .Set("clip", std::move(clip))
           .Set("fromSurface", true),
       base::BindOnce(
           [](base::WeakPtr<ClaudeTools> self, double scale, int width,
              int height, Result shot) {
             if (!self) {
               return;
             }
             const std::string* data =
                 shot.has_value() ? shot->FindString("data") : nullptr;
             if (!data) {
               self->Finish(ToolResult::Error(
                   shot.has_value() ? "No screenshot." : shot.error()));
               return;
             }
             self->screenshot_scale_ = scale;
             ToolResult result;
             result.content.Append(
                 base::DictValue()
                     .Set("type", "image")
                     .Set("source", base::DictValue()
                                        .Set("type", "base64")
                                        .Set("media_type", "image/jpeg")
                                        .Set("data", *data)));
             result.content.Append(
                 base::DictValue()
                     .Set("type", "text")
                     .Set("text",
                          base::StringPrintf(
                              "Screenshot of the visible page, %dx%d. Use "
                              "this image's pixels for coordinates.",
                              width, height)));
             self->Finish(std::move(result));
           },
           run_weak_factory_.GetWeakPtr(), scale, out_width, out_height));
}

void ClaudeTools::ReadConsole(base::DictValue input) {
  const bool only_new = input.FindBool("only_new").value_or(true);
  const int limit = std::clamp(input.FindInt("limit").value_or(100), 1, 500);
  std::vector<std::string> levels;
  if (const base::ListValue* list = input.FindList("levels")) {
    for (const base::Value& level : *list) {
      if (level.is_string()) {
        levels.push_back(level.GetString());
      }
    }
  }
  std::unique_ptr<re2::RE2> pattern;
  if (const std::string* text = input.FindString("pattern")) {
    pattern = std::make_unique<re2::RE2>(*text, re2::RE2::Quiet);
    if (!pattern->ok()) {
      Finish(ToolResult::Error(
          base::StrCat({"Bad pattern: ", pattern->error()})));
      return;
    }
  }
  // Runtime.enable replays what came before asynchronously: a moment for it
  // to arrive on the first read.
  delay_.Start(
      FROM_HERE, kSettle,
      base::BindOnce(
          [](ClaudeTools* self, bool only_new, int limit,
             std::vector<std::string> levels,
             std::unique_ptr<re2::RE2> pattern) {
            std::vector<const ConsoleEntry*> matching;
            for (const ConsoleEntry& entry : self->console_) {
              if (only_new && entry.seq <= self->console_read_seq_) {
                continue;
              }
              if (!levels.empty() &&
                  std::ranges::find(levels, entry.level) == levels.end()) {
                continue;
              }
              if (pattern && !re2::RE2::PartialMatch(entry.text, *pattern)) {
                continue;
              }
              matching.push_back(&entry);
            }
            if (!self->console_.empty()) {
              self->console_read_seq_ = self->console_.back().seq;
            }
            if (matching.empty()) {
              self->Finish(ToolResult::Text(
                  only_new ? "No new console messages." :
                             "No console messages."));
              return;
            }
            std::string text;
            const size_t skipped =
                matching.size() > static_cast<size_t>(limit)
                    ? matching.size() - limit
                    : 0;
            if (skipped) {
              base::StrAppend(&text, {base::NumberToString(skipped),
                                      " older messages not shown.\n"});
            }
            for (size_t i = skipped; i < matching.size(); ++i) {
              const ConsoleEntry& entry = *matching[i];
              base::StrAppend(&text, {"[", entry.level, "] ", entry.text});
              if (!entry.source.empty()) {
                base::StrAppend(&text, {"  (", entry.source, ")"});
              }
              text += "\n";
            }
            self->Finish(ToolResult::Text(std::move(text)));
          },
          base::Unretained(this), only_new, limit, std::move(levels),
          std::move(pattern)));
}

void ClaudeTools::RunJs(base::DictValue input) {
  const std::string* code = input.FindString("code");
  if (!code) {
    Finish(ToolResult::Error("No code."));
    return;
  }
  Send("Runtime.evaluate",
       base::DictValue()
           .Set("expression", *code)
           .Set("awaitPromise", true)
           .Set("returnByValue", true)
           .Set("userGesture", true)
           // Top-level await, and let/const that can be declared again.
           .Set("replMode", true)
           .Set("timeout", 20000),
       base::BindOnce(
           [](base::WeakPtr<ClaudeTools> self, Result evaluated) {
             if (!self) {
               return;
             }
             if (!evaluated.has_value()) {
               self->Finish(ToolResult::Error(evaluated.error()));
               return;
             }
             if (const base::DictValue* exception =
                     evaluated->FindDict("exceptionDetails")) {
               const std::string* description =
                   exception->FindStringByDottedPath("exception.description");
               self->Finish(ToolResult::Error(
                   description ? *description
                               : Describe(*exception, "text")));
               return;
             }
             const base::DictValue* result = evaluated->FindDict("result");
             std::string text;
             if (!result) {
               text = "undefined";
             } else if (const base::Value* value = result->Find("value")) {
               text = Json(*value);
             } else if (const std::string* description =
                            result->FindString("description")) {
               text = *description;
             } else {
               text = Describe(*result, "type");
             }
             if (text.size() > kMaxJsChars) {
               text.resize(kMaxJsChars);
               text += "… (cut)";
             }
             self->Finish(ToolResult::Text(std::move(text)));
           },
           run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::Click(base::DictValue input) {
  const std::string button = input.FindString("button")
                                 ? *input.FindString("button")
                                 : std::string("left");
  const int count = std::clamp(input.FindInt("click_count").value_or(1), 1, 3);
  const int modifiers = ButtonModifiers(input.FindString("modifiers"));
  Locate(
      input,
      base::BindOnce(
          [](base::WeakPtr<ClaudeTools> self, std::string button, int count,
             int modifiers, base::expected<Point, std::string> point) {
            if (!self) {
              return;
            }
            if (!point.has_value()) {
              self->Finish(ToolResult::Error(point.error()));
              return;
            }
            auto mouse = [&](const char* type, int clicks) {
              return base::DictValue()
                  .Set("type", type)
                  .Set("x", point->x)
                  .Set("y", point->y)
                  .Set("button", type == std::string("mouseMoved")
                                     ? std::string("none")
                                     : button)
                  .Set("clickCount", clicks)
                  .Set("modifiers", modifiers);
            };
            std::vector<std::pair<std::string, base::DictValue>> commands;
            commands.emplace_back("Input.dispatchMouseEvent",
                                  mouse("mouseMoved", 0));
            for (int i = 1; i <= count; ++i) {
              commands.emplace_back("Input.dispatchMouseEvent",
                                    mouse("mousePressed", i));
              commands.emplace_back("Input.dispatchMouseEvent",
                                    mouse("mouseReleased", i));
            }
            self->ShowIndicator(point->x, point->y);
            std::string done = base::StringPrintf(
                "Clicked %s at %.0f,%.0f.",
                point->element.empty() ? "the page" : point->element.c_str(),
                point->x * self->screenshot_scale_,
                point->y * self->screenshot_scale_);
            if (!point->covered.empty()) {
              base::StrAppend(&done, {" It was covered by ", point->covered,
                                      ", which got the click."});
            }
            self->SendAll(
                std::move(commands),
                base::BindOnce(
                    [](base::WeakPtr<ClaudeTools> self, std::string done,
                       Result sent) {
                      if (!self) {
                        return;
                      }
                      if (!sent.has_value()) {
                        self->Finish(ToolResult::Error(sent.error()));
                        return;
                      }
                      self->ReportAfterAction(std::move(done));
                    },
                    self->run_weak_factory_.GetWeakPtr(), std::move(done)));
          },
          run_weak_factory_.GetWeakPtr(), button, count, modifiers));
}

void ClaudeTools::Hover(base::DictValue input) {
  Locate(input,
         base::BindOnce(
             [](base::WeakPtr<ClaudeTools> self,
                base::expected<Point, std::string> point) {
               if (!self) {
                 return;
               }
               if (!point.has_value()) {
                 self->Finish(ToolResult::Error(point.error()));
                 return;
               }
               std::string done = base::StrCat(
                   {"The pointer is over ",
                    point->element.empty() ? "the page" : point->element,
                    "."});
               self->Send("Input.dispatchMouseEvent",
                          base::DictValue()
                              .Set("type", "mouseMoved")
                              .Set("x", point->x)
                              .Set("y", point->y),
                          base::BindOnce(
                              [](base::WeakPtr<ClaudeTools> self,
                                 std::string done, Result sent) {
                                if (!self) {
                                  return;
                                }
                                if (!sent.has_value()) {
                                  self->Finish(ToolResult::Error(sent.error()));
                                  return;
                                }
                                self->ReportAfterAction(std::move(done));
                              },
                              self->run_weak_factory_.GetWeakPtr(),
                              std::move(done)));
             },
             run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::Type(base::DictValue input) {
  const std::string* text = input.FindString("text");
  if (!text) {
    Finish(ToolResult::Error("No text."));
    return;
  }
  auto insert = base::BindOnce(
      [](base::WeakPtr<ClaudeTools> self, std::string text, Result focused) {
        if (!self) {
          return;
        }
        if (!focused.has_value()) {
          self->Finish(ToolResult::Error(focused.error()));
          return;
        }
        const std::string target = Describe(*focused, "focused");
        std::string done = base::StrCat(
            {"Typed ", base::NumberToString(text.size()), " characters",
             target.empty() ? std::string() : " into " + target, "."});
        self->Send("Input.insertText", base::DictValue().Set("text", text),
                   base::BindOnce(
                       [](base::WeakPtr<ClaudeTools> self, std::string done,
                          Result sent) {
                         if (!self) {
                           return;
                         }
                         if (!sent.has_value()) {
                           self->Finish(ToolResult::Error(sent.error()));
                           return;
                         }
                         self->ReportAfterAction(std::move(done));
                       },
                       self->run_weak_factory_.GetWeakPtr(),
                       std::move(done)));
      },
      run_weak_factory_.GetWeakPtr(), *text);
  if (const std::string* ref = input.FindString("ref")) {
    Agent("focus", base::DictValue().Set("ref", *ref), std::move(insert));
  } else {
    std::move(insert).Run(base::DictValue());
  }
}

void ClaudeTools::Key(base::DictValue input) {
  const std::string* keys = input.FindString("keys");
  if (!keys) {
    Finish(ToolResult::Error("No keys."));
    return;
  }
  auto presses = ParseKeys(*keys, BUILDFLAG(IS_MAC));
  if (!presses.has_value()) {
    Finish(ToolResult::Error(presses.error()));
    return;
  }
  const int repeat = std::clamp(input.FindInt("repeat").value_or(1), 1, 50);
  std::vector<std::pair<std::string, base::DictValue>> commands;
  for (int i = 0; i < repeat; ++i) {
    for (const KeyPress& press : *presses) {
      commands.emplace_back("Input.dispatchKeyEvent", KeyDownParams(press));
      commands.emplace_back("Input.dispatchKeyEvent", KeyUpParams(press));
    }
  }
  std::string done = base::StrCat({"Pressed ", *keys});
  if (repeat > 1) {
    base::StrAppend(&done, {" ", base::NumberToString(repeat), " times"});
  }
  done += ".";
  SendAll(std::move(commands),
          base::BindOnce(
              [](base::WeakPtr<ClaudeTools> self, std::string done,
                 Result sent) {
                if (!self) {
                  return;
                }
                if (!sent.has_value()) {
                  self->Finish(ToolResult::Error(sent.error()));
                  return;
                }
                self->ReportAfterAction(std::move(done));
              },
              run_weak_factory_.GetWeakPtr(), std::move(done)));
}

void ClaudeTools::Scroll(base::DictValue input) {
  // The agent takes CSS pixels.
  if (base::ListValue* coordinate = input.FindList("coordinate");
      coordinate && coordinate->size() == 2) {
    base::ListValue css;
    for (const base::Value& value : *coordinate) {
      css.Append(value.is_int() || value.is_double()
                     ? value.GetDouble() / screenshot_scale_
                     : 0.0);
    }
    input.Set("coordinate", std::move(css));
  }
  Agent("scroll", std::move(input),
        base::BindOnce(
            [](base::WeakPtr<ClaudeTools> self, Result scrolled) {
              if (!self) {
                return;
              }
              if (!scrolled.has_value()) {
                self->Finish(ToolResult::Error(scrolled.error()));
                return;
              }
              const int by = scrolled->FindInt("scrolled").value_or(0);
              const std::string container = Describe(*scrolled, "container");
              self->Finish(ToolResult::Text(
                  by == 0 ? base::StrCat({"Nothing scrolled: ", container,
                                          " is at its end."})
                          : base::StringPrintf(
                                "Scrolled %s by %d px, to %d of %d.",
                                container.c_str(), by,
                                scrolled->FindInt("position").value_or(0),
                                scrolled->FindInt("max").value_or(0))));
            },
            run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::FormInput(base::DictValue input) {
  Agent("formInput", std::move(input),
        base::BindOnce(
            [](base::WeakPtr<ClaudeTools> self, Result set) {
              if (!self) {
                return;
              }
              if (!set.has_value()) {
                self->Finish(ToolResult::Error(set.error()));
                return;
              }
              const base::Value* value = set->Find("value");
              self->ReportAfterAction(base::StrCat(
                  {"Set to ",
                   value ? (value->is_string() ? value->GetString()
                                               : Json(*value))
                         : std::string("?"),
                   "."}));
            },
            run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::Navigate(base::DictValue input) {
  const std::string* where = input.FindString("url");
  if (!where) {
    Finish(ToolResult::Error("No url."));
    return;
  }
  content::NavigationController& controller =
      web_contents()->GetController();
  if (*where == "back" || *where == "forward") {
    const bool back = *where == "back";
    if (back ? !controller.CanGoBack() : !controller.CanGoForward()) {
      Finish(ToolResult::Error(
          base::StrCat({"There's no page to go ", *where, " to."})));
      return;
    }
    back ? controller.GoBack() : controller.GoForward();
  } else if (*where == "reload") {
    controller.Reload(content::ReloadType::NORMAL, /*check_for_repost=*/false);
  } else {
    const GURL url(*where);
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || !CanUsePage(url)) {
      Finish(ToolResult::Error(
          "Claude can only load http and https web pages."));
      return;
    }
    content::NavigationController::LoadURLParams params(url);
    params.transition_type = ui::PageTransitionFromInt(
        ui::PAGE_TRANSITION_LINK | ui::PAGE_TRANSITION_FROM_API);
    controller.LoadURLWithParams(params);
  }
  auto loaded = base::BindOnce(
      [](base::WeakPtr<ClaudeTools> self) {
        if (!self) {
          return;
        }
        content::WebContents* contents = self->web_contents();
        self->Finish(ToolResult::Text(base::StrCat(
            {"Loaded ", contents->GetLastCommittedURL().spec(), " (\"",
             base::UTF16ToUTF8(contents->GetTitle()), "\")."})));
      },
      run_weak_factory_.GetWeakPtr());
  // Done at the load's end, or after a while (some pages never stop).
  on_loaded_ = std::move(loaded);
  delay_.Start(FROM_HERE, kNavigateTimeout,
               base::BindOnce(
                   [](base::WeakPtr<ClaudeTools> self) {
                     if (self && self->on_loaded_) {
                       std::move(self->on_loaded_).Run();
                     }
                   },
                   run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::Wait(base::DictValue input) {
  const double seconds =
      std::clamp(input.FindDouble("seconds").value_or(1), 0.1, 10.0);
  delay_.Start(FROM_HERE, base::Seconds(seconds),
               base::BindOnce(&ClaudeTools::Finish,
                              run_weak_factory_.GetWeakPtr(),
                              ToolResult::Text("Waited.")));
}

void ClaudeTools::ListTabs(base::DictValue input) {
  Finish(ToolResult::Text(tabs_->ListTabs(MakeContext())));
}

void ClaudeTools::OpenTab(base::DictValue input) {
  const std::string* spec = input.FindString("url");
  const GURL url(spec ? *spec : std::string());
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || !CanUsePage(url)) {
    Finish(ToolResult::Error("Claude can only open http and https web pages."));
    return;
  }
  content::WebContents* contents =
      tabs_->OpenTab(url, input.FindBool("active").value_or(false));
  if (!contents) {
    Finish(ToolResult::Error("The tab couldn't be opened."));
    return;
  }
  const std::string id = tabs_->IdOf(contents);
  opened_.insert(id);
  Retarget(contents);
  on_loaded_ = base::BindOnce(
      [](base::WeakPtr<ClaudeTools> self, std::string id) {
        if (!self) {
          return;
        }
        content::WebContents* contents = self->web_contents();
        if (!contents) {
          self->Finish(ToolResult::Error("The new tab closed."));
          return;
        }
        self->Finish(ToolResult::Text(base::StrCat(
            {"Opened tab ", id, ": ", contents->GetLastCommittedURL().spec(),
             " (\"", base::UTF16ToUTF8(contents->GetTitle()),
             "\"). The page tools act on it now; select_tab goes back."})));
      },
      run_weak_factory_.GetWeakPtr(), id);
  delay_.Start(FROM_HERE, kNavigateTimeout,
               base::BindOnce(
                   [](base::WeakPtr<ClaudeTools> self) {
                     if (self && self->on_loaded_) {
                       std::move(self->on_loaded_).Run();
                     }
                   },
                   run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::SelectTab(base::DictValue input) {
  const std::string* id = input.FindString("tab_id");
  base::expected<content::WebContents*, std::string> tab =
      id ? tabs_->LoadTab(*id)
         : base::unexpected(std::string("Give a tab_id from list_tabs."));
  if (!tab.has_value()) {
    Finish(ToolResult::Error(tab.error()));
    return;
  }
  content::WebContents* contents = *tab;
  Retarget(contents);
  std::string text = base::StrCat(
      {"The page tools act on tab ", *id, " now: ",
       base::UTF16ToUTF8(contents->GetTitle()), " — ",
       contents->GetVisibleURL().spec(), "."});
  if (!CanUsePage(contents->GetVisibleURL())) {
    text += " It's a browser page, which Claude can't use.";
  }
  Finish(ToolResult::Text(std::move(text)));
}

void ClaudeTools::CloseTab(base::DictValue input) {
  const std::string* id = input.FindString("tab_id");
  if (id) {
    opened_.erase(*id);
    // Claude steps off a tab before it goes: back to the chat's own.
    content::WebContents* live = tabs_->FindLiveTab(*id);
    if (live && live == web_contents()) {
      Retarget(task_tab_.get() != live ? task_tab_.get() : nullptr);
    }
  }
  RunBrowserTool("close_tab", std::move(input));
}

void ClaudeTools::ReportAfterAction(std::string done) {
  delay_.Start(
      FROM_HERE, kSettle,
      base::BindOnce(
          [](base::WeakPtr<ClaudeTools> self, std::string done) {
            if (!self) {
              return;
            }
            content::WebContents* contents = self->web_contents();
            const std::string url = contents->GetLastCommittedURL().spec();
            if (url != self->url_before_) {
              base::StrAppend(&done, {" The page is now ", url, "."});
            } else if (contents->IsLoading()) {
              done += " The page is loading.";
            }
            self->Finish(ToolResult::Text(std::move(done)));
          },
          run_weak_factory_.GetWeakPtr(), std::move(done)));
}

void ClaudeTools::ShowIndicator(double x, double y) {
  // Drawn by DevTools' overlay, over the page but not in it: the page's DOM
  // and scripts never see it.
  constexpr double kSize = 18;
  auto color = [](double alpha) {
    // Claude's orange.
    return base::DictValue()
        .Set("r", 217)
        .Set("g", 119)
        .Set("b", 87)
        .Set("a", alpha);
  };
  Send("Overlay.highlightRect",
       base::DictValue()
           .Set("x", static_cast<int>(x - kSize / 2))
           .Set("y", static_cast<int>(y - kSize / 2))
           .Set("width", static_cast<int>(kSize))
           .Set("height", static_cast<int>(kSize))
           .Set("color", color(0.45))
           .Set("outlineColor", color(1)),
       base::DoNothing());
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<ClaudeTools> self) {
            if (self && self->devtools_) {
              self->devtools_->Send("Overlay.hideHighlight",
                                    base::DictValue(), base::DoNothing());
            }
          },
          weak_factory_.GetWeakPtr()),
      base::Milliseconds(700));
}

void ClaudeTools::DidStopLoading() {
  if (on_loaded_) {
    std::move(on_loaded_).Run();
  }
}

void ClaudeTools::WebContentsDestroyed() {
  StopActing();
  capture_.ReplaceClosure(base::OnceClosure());
  ResetDevTools();
  Observe(nullptr);
  if (callback_) {
    Finish(ToolResult::Error("The tab Claude was using closed."));
  }
}

}  // namespace compass::claude
