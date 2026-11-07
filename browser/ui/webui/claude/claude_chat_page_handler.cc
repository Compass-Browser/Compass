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
#include "base/strings/utf_string_conversions.h"
#include "base/path_service.h"
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
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/claude/claude_request.h"
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
  pref_registrar_.Init(browser_->GetProfile()->GetPrefs());
  const base::RepeatingClosure changed = base::BindRepeating(
      &ClaudeChatPageHandler::OnSettingsChanged, base::Unretained(this));
  pref_registrar_.Add(prefs::kApiKey, changed);
  pref_registrar_.Add(prefs::kModel, changed);
  pref_registrar_.Add(prefs::kBrowserScope, changed);
  pref_registrar_.Add(prefs::kBackend, changed);
  active_tab_subscription_ = browser_->RegisterActiveTabDidChange(
      base::BindRepeating(&ClaudeChatPageHandler::OnActiveTabChanged,
                          base::Unretained(this)));
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  Observe(tab ? tab->GetContents() : nullptr);
  if (ClaudePanel* panel = ClaudePanel::From(browser_)) {
    panel->SetPageHandler(this);
  }
}

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

  ChatOptions options;
  options.model = prefs->GetString(prefs::kModel);
  options.claude_api = endpoint.base_url == GURL(kDefaultBaseUrl);
  options.oauth_token = IsOAuthToken(endpoint.api_key);
  options.browser_scope = prefs->GetBoolean(prefs::kBrowserScope);
  options.now = base::Time::Now();
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

void ClaudeChatPageHandler::OnStreamEvent(const SseParser::Event& event) {
  if (event.name == "ping") {
    return;
  }
  page_->OnStreamEvent(event.name, event.data);
}

void ClaudeChatPageHandler::OnStreamDone(int request_id,
                                         std::optional<std::string> error) {
  if (request_id != request_id_) {
    return;
  }
  page_->OnStreamDone(error.value_or(std::string()));
}

void ClaudeChatPageHandler::GetContext(GetContextCallback callback) {
  ChatOptions options;
  options.browser_scope =
      browser_->GetProfile()->GetPrefs()->GetBoolean(prefs::kBrowserScope);
  options.now = base::Time::Now();
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
                 [](RunToolCallback callback, ToolResult result) {
                   std::string json;
                   base::JSONWriter::Write(std::move(result).ToDict(), &json);
                   std::move(callback).Run(json);
                 },
                 std::move(callback)));
}

void ClaudeChatPageHandler::Stop() {
  ++request_id_;
  if (client_) {
    client_->Cancel();
  }
  // The conversation goes on from where it was with the next message.
  EndClaudeCode(/*forget=*/false);
  tools_.Stop();
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
  if (messages.size() == 1) {
    // The conversation's first message: a new one (New chat, or the first
    // message Retried).
    EndClaudeCode(/*forget=*/true);
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
  options.system_prompt = SystemPromptText();
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
  claude_code_model_ = options.model;
  claude_code_ = ClaudeCodeProcess::Start(
      *program, ClaudeCodeArgs(options),
      profile->GetPath().Append(kClaudeCodeDir),
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
  if (claude_code_replying_) {
    claude_code_replying_ = false;
    page_->OnStreamDone(
        DescribeClaudeCodeExit(exit_code, claude_code_output_));
  }
}

void ClaudeChatPageHandler::EndClaudeCode(bool forget) {
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

void ClaudeChatPageHandler::OpenSettings() {
  chrome::ShowSettingsSubPage(browser_, kClaudeSettingsSubPage);
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
