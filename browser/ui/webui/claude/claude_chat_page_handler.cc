// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/claude/claude_chat_page_handler.h"

#include <utility>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/path_service.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_client.h"
#include "compass/browser/claude/claude_code.h"
#include "compass/browser/claude/claude_code_process.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_memory.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/claude/claude_request.h"
#include "compass/browser/ui/claude/claude_files_config.h"
#include "compass/browser/ui/claude/claude_tabs.h"
#include "compass/browser/ui/views/claude/claude_panel.h"
#include "compass/browser/ui/webui/settings/settings_sections.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)
#include "compass/browser/ui/claude/mcp_bridge.h"
#endif

namespace compass::claude {

namespace {

#if BUILDFLAG(IS_WIN)
constexpr base::FilePath::CharType kMcpRelayName[] =
    FILE_PATH_LITERAL("compass-mcp.exe");
#elif BUILDFLAG(IS_POSIX)
constexpr base::FilePath::CharType kMcpRelayName[] =
    FILE_PATH_LITERAL("compass-mcp");
#endif

// About 8,000 tokens of page text: enough for an article, not a whole
// manual.
constexpr int kMaxPageChars = 30000;

// What's kept of Claude Code's output that isn't JSON, to say why it ended.
constexpr size_t kMaxClaudeCodeOutput = 2000;

// Claude Code's folder in the profile: it runs there, so it finds no
// project of the user's (CLAUDE.md, settings).
constexpr base::FilePath::CharType kClaudeCodeDir[] =
    FILE_PATH_LITERAL("Claude Code");

std::optional<base::FilePath> FindAndPrepare(base::FilePath configured,
                                             base::FilePath directory) {
  base::CreateDirectory(directory);
  return FindClaudeCode(configured);
}

}  // namespace

ClaudeChatPageHandler::ClaudeChatPageHandler(
    mojo::PendingReceiver<chat::mojom::PageHandler> receiver,
    mojo::PendingRemote<chat::mojom::Page> page,
    BrowserWindowInterface* browser,
    TopChromeWebUIController* controller)
    : receiver_(this, std::move(receiver)),
      page_(std::move(page)),
      browser_(browser),
      controller_(controller),
      tools_(std::make_unique<ClaudeTabs>(browser->GetProfile(), browser)) {
  tools_.SetMemory(ClaudeMemory::Get(browser_->GetProfile()),
                   base::BindRepeating(
                       [](PrefService* prefs) {
                         return prefs->GetBoolean(prefs::kMemoryEnabled) &&
                                prefs->GetBoolean(prefs::kMemorySave);
                       },
                       browser_->GetProfile()->GetPrefs()));
  tools_.SetFiles(
      base::BindRepeating(&GetFilesConfig,
                          base::Unretained(browser_->GetProfile())),
      base::BindRepeating(&ClaudeChatPageHandler::AskApproval,
                          weak_factory_.GetWeakPtr()));
  tools_.SetDevTools(base::BindRepeating(
      [](PrefService* prefs) { return prefs->GetBoolean(prefs::kDevTools); },
      browser_->GetProfile()->GetPrefs()));
  pref_registrar_.Init(browser_->GetProfile()->GetPrefs());
  const base::RepeatingClosure changed = base::BindRepeating(
      &ClaudeChatPageHandler::OnSettingsChanged, base::Unretained(this));
  pref_registrar_.Add(prefs::kApiKey, changed);
  pref_registrar_.Add(prefs::kModel, changed);
  pref_registrar_.Add(prefs::kBrowserScope, changed);
  pref_registrar_.Add(prefs::kBackend, changed);
  pref_registrar_.Add(prefs::kFilesAccess, changed);
  pref_registrar_.Add(prefs::kFilesFolder, changed);
  pref_registrar_.Add(prefs::kClaudeCodeOwnTools, changed);
  pref_registrar_.Add(prefs::kDevTools, changed);
  active_tab_subscription_ = browser_->RegisterActiveTabDidChange(
      base::BindRepeating(&ClaudeChatPageHandler::OnActiveTabChanged,
                          base::Unretained(this)));
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  Observe(tab ? tab->GetContents() : nullptr);
  if (ClaudePanel* panel = ClaudePanel::From(browser_)) {
    panel->SetPageHandler(this);
  }
}

ClaudeChatPageHandler::PendingApproval::PendingApproval() = default;
ClaudeChatPageHandler::PendingApproval::PendingApproval(PendingApproval&&) =
    default;
ClaudeChatPageHandler::PendingApproval&
ClaudeChatPageHandler::PendingApproval::operator=(PendingApproval&&) = default;
ClaudeChatPageHandler::PendingApproval::~PendingApproval() = default;

ClaudeChatPageHandler::~ClaudeChatPageHandler() {
  EndClaudeCode(/*forget=*/true);
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)
  if (!chat_token_.empty()) {
    McpBridge::Get()->UnregisterChat(chat_token_);
  }
#endif
  if (ClaudePanel* panel = ClaudePanel::From(browser_)) {
    panel->RemovePageHandler(this);
  }
}

void ClaudeChatPageHandler::SetLoaderFactoryForTesting(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  loader_factory_ = std::move(factory);
}

void ClaudeChatPageHandler::ShowUI() {
  if (controller_ && controller_->embedder()) {
    controller_->embedder()->ShowUI();
  }
}

void ClaudeChatPageHandler::GetSetup(GetSetupCallback callback) {
  std::move(callback).Run(MakeSetup());
}

void ClaudeChatPageHandler::GetActivePage(GetActivePageCallback callback) {
  std::move(callback).Run(MakeActivePage());
}

void ClaudeChatPageHandler::ReadActivePage(ReadActivePageCallback callback) {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  if (!tab) {
    std::move(callback).Run(nullptr, "No tab is open.");
    return;
  }
  ReadPageText(tab->GetContents(), kMaxPageChars,
               base::BindOnce(&ClaudeChatPageHandler::OnPageRead,
                              weak_factory_.GetWeakPtr(), std::move(callback)));
}

void ClaudeChatPageHandler::OnPageRead(
    ReadActivePageCallback callback,
    base::expected<PageText, std::string> page) {
  if (!page.has_value()) {
    std::move(callback).Run(nullptr, page.error());
    return;
  }
  std::move(callback).Run(
      chat::mojom::PageText::New(page->url, page->title, page->text,
                                 page->truncated),
      std::string());
}

void ClaudeChatPageHandler::Send(const std::string& messages_json) {
  if (client_) {
    client_->Cancel();
  }
  if (UsesClaudeCode()) {
    ++request_id_;
    std::optional<base::Value> messages =
        base::JSONReader::Read(messages_json, base::JSON_PARSE_RFC);
    if (!messages || !messages->is_list() || messages->GetList().empty()) {
      page_->OnStreamDone("Nothing to send.");
      return;
    }
    SendToClaudeCode(std::move(*messages).TakeList());
    return;
  }
  const int request_id = ++request_id_;
  ClaudeCredentials::GetApiKey(
      browser_->GetProfile(),
      base::BindOnce(&ClaudeChatPageHandler::OnKeyForSend,
                     weak_factory_.GetWeakPtr(), request_id, messages_json));
}

void ClaudeChatPageHandler::OnKeyForSend(int request_id,
                                         std::string messages_json,
                                         std::optional<std::string> key) {
  if (request_id != request_id_) {
    // Stopped, or sent again, meanwhile.
    return;
  }
  if (!key) {
    page_->OnStreamDone("Add your API key in Compass's Claude settings.");
    return;
  }
  std::optional<base::Value> messages =
      base::JSONReader::Read(messages_json, base::JSON_PARSE_RFC);
  if (!messages || !messages->is_list() || messages->GetList().empty()) {
    page_->OnStreamDone("Nothing to send.");
    return;
  }
  Profile* profile = browser_->GetProfile();
  const PrefService* prefs = profile->GetPrefs();
  Endpoint endpoint = EndpointFromPrefs(*prefs);
  endpoint.api_key = std::move(*key);
  if (NeedsTokenRisksAccepted(endpoint)) {
    page_->OnStreamDone(kTokenRisksMessage);
    return;
  }

  if (messages->GetList().size() == 1) {
    // A new chat: "Allow for this chat" ends.
    CancelApprovals(/*new_chat=*/true);
  }
  ChatOptions options;
  options.model = prefs->GetString(prefs::kModel);
  options.claude_api = endpoint.base_url == GURL(kDefaultBaseUrl);
  options.oauth_token = UsesOAuthToken(endpoint);
  options.browser_scope = prefs->GetBoolean(prefs::kBrowserScope);
  options.now = base::Time::Now();
  const ChatMemory& memory =
      FreezeMemory(/*new_chat=*/messages->GetList().size() == 1);
  options.memory = memory.block;
  options.memory_tools = memory.tools;
  // The developer tools join a chat when they're turned on, and stay in it
  // (refusing while off): a conversation's tools only ever grow.
  if (messages->GetList().size() == 1) {
    chat_devtools_ = false;
  }
  options.devtools = prefs->GetBoolean(prefs::kDevTools);
  chat_devtools_ = chat_devtools_ || options.devtools;
  options.devtools_tools = chat_devtools_;
  // A gateway that adds cache breakpoints of its own refused the usual ones
  // before: the level that got through with it. Another URL forgets it.
  if (!prefs->GetString(prefs::kCacheLevelUrl).empty() &&
      CacheLevelFor(*prefs, endpoint.base_url) == CacheLevel::kNormal) {
    RememberCacheLevel(*profile->GetPrefs(), endpoint.base_url,
                       CacheLevel::kNormal);
  }
  options.cache_level = CacheLevelFor(*prefs, endpoint.base_url);
  base_url_ = endpoint.base_url;
  stepped_down_ = false;
  shown_output_ = false;
  // The same messages for every step down.
  retry_options_ = options;
  retry_messages_ = messages->GetList().Clone();
  ChatRequest request =
      BuildChatRequest(std::move(*messages).TakeList(), options);

  scoped_refptr<network::SharedURLLoaderFactory> factory =
      loader_factory_ ? loader_factory_
                      : profile->GetDefaultStoragePartition()
                            ->GetURLLoaderFactoryForBrowserProcess();
  client_ = std::make_unique<ClaudeClient>(std::move(factory),
                                           std::move(endpoint));
  client_->StreamMessage(
      std::move(request.body), std::move(request.betas),
      base::BindRepeating(&ClaudeChatPageHandler::OnStreamEvent,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&ClaudeChatPageHandler::OnStreamDone,
                     weak_factory_.GetWeakPtr(), request_id));
}

const ClaudeChatPageHandler::ChatMemory& ClaudeChatPageHandler::FreezeMemory(
    bool new_chat) {
  if (chat_memory_ && !new_chat) {
    return *chat_memory_;
  }
  Profile* profile = browser_->GetProfile();
  const PrefService* prefs = profile->GetPrefs();
  ChatMemory memory;
  ClaudeMemory* store = ClaudeMemory::Get(profile);
  if (store && prefs->GetBoolean(prefs::kMemoryEnabled)) {
    memory.block = FormatMemoryBlock(store->entries());
    memory.tools = prefs->GetBoolean(prefs::kMemorySave);
  }
  chat_memory_ = std::move(memory);
  return *chat_memory_;
}

std::string ClaudeChatPageHandler::ToolResultJson(ToolResult result) {
  std::optional<MemoryChange> change = std::move(result.memory_change);
  base::DictValue dict = std::move(result).ToDict();
  if (change) {
    const int32_t undo_id = next_undo_id_++;
    dict.Set("memory", base::DictValue()
                           .Set("kind", change->kind)
                           .Set("text", change->text)
                           .Set("undo", undo_id));
    memory_undo_.emplace(undo_id, std::move(*change));
  }
  std::string json;
  base::JSONWriter::Write(dict, &json);
  return json;
}

void ClaudeChatPageHandler::UndoMemory(int32_t undo_id,
                                       UndoMemoryCallback callback) {
  auto it = memory_undo_.find(undo_id);
  ClaudeMemory* store = ClaudeMemory::Get(browser_->GetProfile());
  if (it == memory_undo_.end() || !store) {
    std::move(callback).Run(false);
    return;
  }
  const MemoryChange change = std::move(it->second);
  memory_undo_.erase(it);
  const bool undone = change.before ? store->Restore(*change.before)
                                    : store->Remove(change.id).has_value();
  std::move(callback).Run(undone);
}

void ClaudeChatPageHandler::OnStreamEvent(const SseParser::Event& event) {
  if (event.name == "ping") {
    return;
  }
  // Text or a tool call shows; thinking alone doesn't.
  if (event.name == "content_block_delta" &&
      event.data.find("\"text_delta\"") != std::string::npos) {
    shown_output_ = true;
  } else if (event.name == "content_block_start" &&
             event.data.find("\"tool_use\"") != std::string::npos) {
    shown_output_ = true;
  }
  page_->OnStreamEvent(event.name, event.data);
}

void ClaudeChatPageHandler::OnStreamDone(int request_id,
                                         std::optional<std::string> error) {
  if (request_id != request_id_) {
    return;
  }
  // A gateway that adds cache breakpoints of its own refuses ours past the
  // API's 4: step down (only the top-level one, then none) with the same
  // messages while it refuses and nothing showed yet, and remember for the
  // endpoint the level that got through. Only the last error shows.
  std::optional<CacheLevel> lower =
      LowerCacheLevel(retry_options_.cache_level);
  if (error && IsTooManyCacheBreakpoints(*error) && lower && retry_messages_ &&
      !shown_output_ && client_) {
    retry_options_.cache_level = *lower;
    stepped_down_ = true;
    // Posted: the client is still finishing this request.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&ClaudeChatPageHandler::SendRetry,
                       weak_factory_.GetWeakPtr(), request_id,
                       BuildChatRequest(retry_messages_->Clone(),
                                        retry_options_)));
    return;
  }
  if (stepped_down_ && !error) {
    RememberCacheLevel(*browser_->GetProfile()->GetPrefs(), base_url_,
                       retry_options_.cache_level);
  }
  stepped_down_ = false;
  retry_messages_.reset();
  page_->OnStreamDone(error.value_or(std::string()));
}

void ClaudeChatPageHandler::SendRetry(int request_id, ChatRequest request) {
  if (request_id != request_id_ || !client_) {
    return;
  }
  client_->StreamMessage(
      std::move(request.body), std::move(request.betas),
      base::BindRepeating(&ClaudeChatPageHandler::OnStreamEvent,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&ClaudeChatPageHandler::OnStreamDone,
                     weak_factory_.GetWeakPtr(), request_id));
}

void ClaudeChatPageHandler::GetContext(GetContextCallback callback) {
  ChatOptions options;
  options.browser_scope =
      browser_->GetProfile()->GetPrefs()->GetBoolean(prefs::kBrowserScope);
  options.now = base::Time::Now();
  options.files = GetFilesConfig(browser_->GetProfile());
  options.claude_code_own_tools =
      UsesClaudeCode() && ClaudeCodeOwnToolsOn(browser_->GetProfile());
  options.devtools =
      browser_->GetProfile()->GetPrefs()->GetBoolean(prefs::kDevTools);
  options.window_tabs = WindowTabs();
  std::move(callback).Run(BuildMessageContext(options));
}

void ClaudeChatPageHandler::StartTask() {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  tools_.SetTarget(tab ? tab->GetContents() : nullptr);
}

void ClaudeChatPageHandler::RunTool(const std::string& name,
                                    const std::string& input_json,
                                    RunToolCallback callback) {
  std::optional<base::DictValue> input =
      base::JSONReader::ReadDict(input_json, base::JSON_PARSE_RFC);
  if (!input) {
    input.emplace();
  }
  tools_.Run(name, std::move(*input),
             base::BindOnce(
                 [](base::WeakPtr<ClaudeChatPageHandler> handler,
                    RunToolCallback callback, ToolResult result) {
                   if (handler) {
                     std::move(callback).Run(
                         handler->ToolResultJson(std::move(result)));
                     return;
                   }
                   // Going away ("Stopped"): the reply still goes.
                   std::string json;
                   base::JSONWriter::Write(std::move(result).ToDict(), &json);
                   std::move(callback).Run(json);
                 },
                 weak_factory_.GetWeakPtr(), std::move(callback)));
}

void ClaudeChatPageHandler::Stop() {
  ++request_id_;
  if (client_) {
    client_->Cancel();
  }
  // The conversation goes on from where it was with the next message.
  EndClaudeCode(/*forget=*/false);
  tools_.Stop();
  CancelApprovals(/*new_chat=*/false);
}

bool ClaudeChatPageHandler::UsesClaudeCode() const {
  return browser_->GetProfile()->GetPrefs()->GetInteger(prefs::kBackend) ==
         static_cast<int>(Backend::kClaudeCode);
}

void ClaudeChatPageHandler::SendToClaudeCode(base::ListValue messages) {
  base::Value& last = messages.back();
  const std::string* role =
      last.is_dict() ? last.GetDict().FindString("role") : nullptr;
  base::ListValue* content =
      last.is_dict() ? last.GetDict().FindList("content") : nullptr;
  if (!role || *role != "user" || !content) {
    page_->OnStreamDone("Nothing to send.");
    return;
  }
  const PrefService* prefs = browser_->GetProfile()->GetPrefs();
  Profile* profile = browser_->GetProfile();
  const bool own_tools = ClaudeCodeOwnToolsOn(profile);
  const base::FilePath folder =
      own_tools ? GetFilesConfig(profile).folder : base::FilePath();
  if (messages.size() == 1) {
    // The conversation's first message: a new one (New chat, or the first
    // message Retried).
    EndClaudeCode(/*forget=*/true);
    CancelApprovals(/*new_chat=*/true);
    FreezeMemory(/*new_chat=*/true);
  } else if (own_tools != claude_code_own_tools_ ||
             folder != claude_code_folder_) {
    // Its tools or folder changed: a session can't move to another folder,
    // so a new one starts (the page keeps showing the conversation).
    EndClaudeCode(/*forget=*/true);
  } else if (claude_code_ &&
             claude_code_devtools_ != prefs->GetBoolean(prefs::kDevTools)) {
    // The developer tools came or went: Claude Code lists Compass's tools
    // as it starts, so the conversation goes on in a new process.
    EndClaudeCode(/*forget=*/false);
  } else if (claude_code_ &&
             claude_code_model_ != prefs->GetString(prefs::kModel)) {
    // Another model: the same conversation, resumed on it.
    EndClaudeCode(/*forget=*/false);
  }
  std::string line = ClaudeCodeUserLine(std::move(*content));
  claude_code_replying_ = true;
  claude_code_output_.clear();
  if (claude_code_) {
    claude_code_->Write(std::move(line));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&FindAndPrepare,
                     base::FilePath::FromUTF8Unsafe(
                         prefs->GetString(prefs::kClaudeCodePath)),
                     browser_->GetProfile()->GetPath().Append(kClaudeCodeDir)),
      base::BindOnce(&ClaudeChatPageHandler::OnClaudeCodeFound,
                     weak_factory_.GetWeakPtr(), request_id_,
                     std::move(line)));
}

void ClaudeChatPageHandler::OnClaudeCodeFound(
    int request_id,
    std::string line,
    std::optional<base::FilePath> program) {
  if (request_id != request_id_) {
    // Stopped, or sent again, meanwhile.
    return;
  }
  if (!program) {
    claude_code_replying_ = false;
    page_->OnStreamDone(
        browser_->GetProfile()->GetPrefs()->GetString(prefs::kClaudeCodePath)
                .empty()
            ? "Claude Code isn't installed where Compass looks. Install it "
              "(claude.com/claude-code), or set its path in Compass's Claude "
              "settings."
            : "Claude Code isn't at the path set in Compass's Claude "
              "settings.");
    return;
  }
  Profile* profile = browser_->GetProfile();
  ClaudeCodeOptions options;
  options.model = profile->GetPrefs()->GetString(prefs::kModel);
  options.system_prompt =
      SystemPromptText(FreezeMemory(/*new_chat=*/false).block);
  options.resume = claude_code_session_;
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)
  // Compass's tools, through the bridge, as this chat's.
  McpBridge* bridge = McpBridge::Get();
  if (chat_token_.empty()) {
    chat_token_ = bridge->RegisterChat(
        profile, base::BindRepeating(&ClaudeChatPageHandler::RunChatTool,
                                     weak_factory_.GetWeakPtr()));
  }
  base::FilePath exe_dir;
  base::PathService::Get(base::DIR_EXE, &exe_dir);
  options.mcp_relay = exe_dir.Append(kMcpRelayName);
  options.user_data_dir = bridge->socket_path().DirName();
  options.chat_token = chat_token_;
#endif
  // With its own tools it works in the user's folder; without, in an empty
  // one of the profile's.
  options.own_tools = ClaudeCodeOwnToolsOn(profile);
  claude_code_own_tools_ = options.own_tools;
  claude_code_devtools_ = profile->GetPrefs()->GetBoolean(prefs::kDevTools);
  claude_code_folder_ = options.own_tools ? GetFilesConfig(profile).folder
                                          : base::FilePath();
  claude_code_model_ = options.model;
  ++claude_code_generation_;
  claude_code_ = ClaudeCodeProcess::Start(
      *program, ClaudeCodeArgs(options),
      options.own_tools ? claude_code_folder_
                        : profile->GetPath().Append(kClaudeCodeDir),
      base::BindRepeating(&ClaudeChatPageHandler::OnClaudeCodeLine,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&ClaudeChatPageHandler::OnClaudeCodeExit,
                     weak_factory_.GetWeakPtr()));
  if (!claude_code_) {
    claude_code_replying_ = false;
    page_->OnStreamDone(base::StrCat(
        {"Claude Code couldn't start (", program->AsUTF8Unsafe(), ")."}));
    return;
  }
  claude_code_->Write(std::move(line));
}

void ClaudeChatPageHandler::OnClaudeCodeLine(std::string line) {
  std::vector<ClaudeCodeEvent> events = ParseClaudeCodeLine(line);
  if (events.empty() && !line.starts_with("{")) {
    // stderr: a warning, or why it's about to stop.
    claude_code_output_ += line + "\n";
    if (claude_code_output_.size() > kMaxClaudeCodeOutput) {
      claude_code_output_.erase(
          0, claude_code_output_.size() - kMaxClaudeCodeOutput);
    }
    return;
  }
  for (ClaudeCodeEvent& event : events) {
    switch (event.type) {
      case ClaudeCodeEvent::Type::kIgnore:
        break;
      case ClaudeCodeEvent::Type::kInit:
        claude_code_session_ = std::move(event.session_id);
        break;
      case ClaudeCodeEvent::Type::kStreamEvent:
        page_->OnStreamEvent(event.name, event.data);
        break;
      case ClaudeCodeEvent::Type::kToolResult:
        page_->OnToolResult(event.tool_use_id, event.data);
        break;
      case ClaudeCodeEvent::Type::kPermission:
        OnClaudeCodePermission(std::move(event.name),
                               std::move(event.request_id),
                               std::move(event.data));
        break;
      case ClaudeCodeEvent::Type::kResult:
        if (claude_code_replying_) {
          claude_code_replying_ = false;
          page_->OnStreamDone(event.error);
        }
        break;
    }
  }
}

void ClaudeChatPageHandler::OnClaudeCodeExit(int exit_code) {
  claude_code_.reset();
  CancelApprovals(/*new_chat=*/false);
  if (claude_code_replying_) {
    claude_code_replying_ = false;
    page_->OnStreamDone(
        DescribeClaudeCodeExit(exit_code, claude_code_output_));
  }
}

void ClaudeChatPageHandler::EndClaudeCode(bool forget) {
  if (claude_code_) {
    // Its questions can't be answered anymore.
    CancelApprovals(/*new_chat=*/false);
  }
  claude_code_.reset();
  claude_code_replying_ = false;
  if (forget) {
    claude_code_session_.clear();
  }
}

void ClaudeChatPageHandler::RunChatTool(
    const std::string& name,
    base::DictValue arguments,
    base::OnceCallback<void(ToolResult)> done) {
  if (!claude_code_replying_) {
    // Stopped: a call that was on its way.
    std::move(done).Run(ToolResult::Error("Stopped by the user."));
    return;
  }
  tools_.Run(name, std::move(arguments), std::move(done));
}

void ClaudeChatPageHandler::OnClaudeCodePermission(std::string tool,
                                                   std::string request_id,
                                                   std::string input_json) {
  const ClaudeCodeApproval described =
      DescribeClaudeCodePermission(tool, input_json);
  ApprovalRequest request;
  request.tool = tool;
  request.title = described.title;
  request.detail = described.detail;
  AskApproval(
      std::move(request),
      base::BindOnce(
          [](base::WeakPtr<ClaudeChatPageHandler> handler, int generation,
             std::string request_id, std::string input_json, bool allow) {
            if (!handler || !handler->claude_code_ ||
                handler->claude_code_generation_ != generation) {
              return;
            }
            handler->claude_code_->Write(
                ClaudeCodePermissionLine(request_id, allow, input_json));
          },
          weak_factory_.GetWeakPtr(), claude_code_generation_,
          std::move(request_id), std::move(input_json)));
}

void ClaudeChatPageHandler::AskApproval(ApprovalRequest request,
                                        base::OnceCallback<void(bool)> done) {
  if (allowed_for_chat_.contains(request.tool)) {
    std::move(done).Run(true);
    return;
  }
  const int32_t id = next_approval_id_++;
  PendingApproval pending;
  pending.tool = request.tool;
  pending.done = std::move(done);
  approvals_.emplace(id, std::move(pending));
  page_->OnApproval(id, request.title, request.detail);
  // The question needs the panel: it may have been closed while Claude
  // worked.
  if (ClaudePanel* panel = ClaudePanel::From(browser_);
      panel && !panel->IsShowing()) {
    panel->Show();
  }
}

void ClaudeChatPageHandler::AnswerApproval(
    int32_t id,
    chat::mojom::ApprovalDecision decision) {
  auto it = approvals_.find(id);
  if (it == approvals_.end()) {
    return;
  }
  if (decision == chat::mojom::ApprovalDecision::kAllowForChat) {
    allowed_for_chat_.insert(it->second.tool);
  }
  base::OnceCallback<void(bool)> done = std::move(it->second.done);
  approvals_.erase(it);
  std::move(done).Run(decision != chat::mojom::ApprovalDecision::kDeny);
}

void ClaudeChatPageHandler::CancelApprovals(bool new_chat) {
  if (new_chat) {
    allowed_for_chat_.clear();
  }
  std::map<int32_t, PendingApproval> waiting = std::move(approvals_);
  approvals_.clear();
  for (auto& [id, pending] : waiting) {
    page_->OnApprovalCancelled(id);
    std::move(pending.done).Run(false);
  }
}

void ClaudeChatPageHandler::OpenSettings() {
  chrome::ShowSettingsSubPage(browser_, kClaudeSettingsSubPage);
}

void ClaudeChatPageHandler::SetModel(const std::string& model) {
  // As ClaudeSettingsHandler::SetModel(); the pref change sends the page
  // its setup again.
  PrefService* prefs = browser_->GetProfile()->GetPrefs();
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(model, base::TRIM_ALL);
  if (trimmed.empty()) {
    prefs->ClearPref(prefs::kModel);
  } else {
    prefs->SetString(prefs::kModel, std::string(trimmed));
  }
}

void ClaudeChatPageHandler::OpenLink(const std::string& url) {
  const GURL link(url);
  if (link.is_valid() && link.SchemeIsHTTPOrHTTPS()) {
    browser_->OpenGURL(link, WindowOpenDisposition::NEW_FOREGROUND_TAB);
  }
}

void ClaudeChatPageHandler::ClosePanel() {
  if (ClaudePanel* panel = ClaudePanel::From(browser_)) {
    panel->Close();
  }
}

void ClaudeChatPageHandler::TakeQuestion(TakeQuestionCallback callback) {
  ClaudePanel* panel = ClaudePanel::From(browser_);
  std::move(callback).Run(panel ? panel->TakeQuestion() : std::string());
}

void ClaudeChatPageHandler::Ask(const std::string& question) {
  page_->OnAsk(question);
}

void ClaudeChatPageHandler::PrimaryPageChanged(content::Page& page) {
  page_->OnActivePageChanged(MakeActivePage());
}

void ClaudeChatPageHandler::TitleWasSet(content::NavigationEntry* entry) {
  page_->OnActivePageChanged(MakeActivePage());
}

std::string ClaudeChatPageHandler::WindowTabs() {
  // Titles and URLs only, as context: without "Let Claude use the whole
  // browser", Claude can't read these tabs, but it can name them.
  constexpr int kMaxTabs = 40;
  std::string text;
  TabStripModel* model = browser_->GetTabStripModel();
  for (int i = 0; i < model->count() && i < kMaxTabs; ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    std::string title = base::UTF16ToUTF8(contents->GetTitle());
    if (title.size() > 100) {
      title.resize(100);
      title += "…";
    }
    base::StrAppend(&text, {"- ", title, " — ",
                            contents->GetVisibleURL().spec(),
                            i == model->active_index() ? " (active)" : "",
                            "\n"});
  }
  if (model->count() > kMaxTabs) {
    base::StrAppend(&text, {"… and ",
                            base::NumberToString(model->count() - kMaxTabs),
                            " more\n"});
  }
  return text;
}

chat::mojom::SetupPtr ClaudeChatPageHandler::MakeSetup() {
  Profile* profile = browser_->GetProfile();
  const PrefService* prefs = profile->GetPrefs();
  const bool claude_code = UsesClaudeCode();
  return chat::mojom::Setup::New(
      claude_code || ClaudeCredentials::HasApiKey(profile), claude_code,
      prefs->GetString(prefs::kModel),
      prefs->GetBoolean(prefs::kBrowserScope));
}

chat::mojom::ActivePagePtr ClaudeChatPageHandler::MakeActivePage() {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  if (!tab) {
    return chat::mojom::ActivePage::New(std::string(), std::string(), false);
  }
  content::WebContents* contents = tab->GetContents();
  const GURL& url = contents->GetLastCommittedURL();
  return chat::mojom::ActivePage::New(
      url.spec(), base::UTF16ToUTF8(contents->GetTitle()), CanUsePage(url));
}

void ClaudeChatPageHandler::OnSettingsChanged() {
  if (!UsesClaudeCode() && claude_code_) {
    // Back to the API: Claude Code's conversation isn't the page's.
    EndClaudeCode(/*forget=*/true);
  }
  page_->OnSetupChanged(MakeSetup());
}

void ClaudeChatPageHandler::OnActiveTabChanged(
    BrowserWindowInterface* browser) {
  tabs::TabInterface* tab = browser->GetActiveTabInterface();
  Observe(tab ? tab->GetContents() : nullptr);
  page_->OnActivePageChanged(MakeActivePage());
}

}  // namespace compass::claude
