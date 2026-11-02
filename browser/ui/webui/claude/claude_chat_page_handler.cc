// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/claude/claude_chat_page_handler.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_client.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/claude/claude_request.h"
#include "compass/browser/ui/views/claude/claude_panel.h"
#include "compass/browser/ui/webui/settings/settings_sections.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace compass::claude {

namespace {

// About 8,000 tokens of page text: enough for an article, not a whole
// manual.
constexpr int kMaxPageChars = 30000;

}  // namespace

ClaudeChatPageHandler::ClaudeChatPageHandler(
    mojo::PendingReceiver<chat::mojom::PageHandler> receiver,
    mojo::PendingRemote<chat::mojom::Page> page,
    BrowserWindowInterface* browser,
    TopChromeWebUIController* controller)
    : receiver_(this, std::move(receiver)),
      page_(std::move(page)),
      browser_(browser),
      controller_(controller) {
  pref_registrar_.Init(browser_->GetProfile()->GetPrefs());
  const base::RepeatingClosure changed = base::BindRepeating(
      &ClaudeChatPageHandler::OnSettingsChanged, base::Unretained(this));
  pref_registrar_.Add(prefs::kApiKey, changed);
  pref_registrar_.Add(prefs::kModel, changed);
  pref_registrar_.Add(prefs::kBrowserScope, changed);
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

void ClaudeChatPageHandler::Stop() {
  ++request_id_;
  if (client_) {
    client_->Cancel();
  }
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

chat::mojom::SetupPtr ClaudeChatPageHandler::MakeSetup() {
  Profile* profile = browser_->GetProfile();
  const PrefService* prefs = profile->GetPrefs();
  return chat::mojom::Setup::New(ClaudeCredentials::HasApiKey(profile),
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
  page_->OnSetupChanged(MakeSetup());
}

void ClaudeChatPageHandler::OnActiveTabChanged(
    BrowserWindowInterface* browser) {
  tabs::TabInterface* tab = browser->GetActiveTabInterface();
  Observe(tab ? tab->GetContents() : nullptr);
  page_->OnActivePageChanged(MakeActivePage());
}

}  // namespace compass::claude
