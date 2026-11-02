// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/claude/claude_panel.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/side_panel/side_panel_entry.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_id.h"
#include "chrome/browser/ui/side_panel/side_panel_registry.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/views/side_panel/side_panel_web_ui_view.h"
#include "chrome/browser/ui/webui/top_chrome/webui_contents_wrapper.h"
#include "chrome/grit/generated_resources.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "third_party/skia/include/core/SkColor.h"
#include "compass/browser/ui/webui/claude/claude_chat_page_handler.h"
#include "compass/browser/ui/webui/claude/claude_chat_ui.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/controls/webview/web_contents_set_background_color.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/view.h"
#include "url/gurl.h"

using SidePanelWebUIViewT_ClaudeChatUI =
    SidePanelWebUIViewT<compass::claude::ClaudeChatUI>;
BEGIN_TEMPLATE_METADATA(SidePanelWebUIViewT_ClaudeChatUI, SidePanelWebUIViewT)
END_METADATA

namespace compass::claude {

namespace {

// Wide enough for code and tables; the user can drag it wider.
constexpr int kDefaultWidth = 420;

SidePanelEntry::Key EntryKey() {
  return SidePanelEntry::Key(SidePanelEntry::Id::kCompassClaude);
}

}  // namespace

DEFINE_USER_DATA(ClaudePanel);

ClaudePanel::ClaudePanel(BrowserWindowInterface* browser)
    : browser_(browser),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {}

ClaudePanel::~ClaudePanel() {
  // The entry's view callback points here.
  if (registered_) {
    if (SidePanelRegistry* registry = SidePanelRegistry::From(browser_)) {
      registry->Deregister(EntryKey());
    }
  }
}

// static
ClaudePanel* ClaudePanel::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

void ClaudePanel::Show() {
  RegisterEntryIfNeeded();
  if (SidePanelUI* ui = SidePanelUI::From(browser_)) {
    ui->Show(EntryKey());
  }
}

void ClaudePanel::Close() {
  if (IsShowing()) {
    SidePanelUI::From(browser_)->Close();
  }
}

void ClaudePanel::Toggle() {
  if (IsShowing()) {
    Close();
  } else {
    Show();
  }
}

void ClaudePanel::Ask(const std::string& question) {
  Show();
  if (question.empty()) {
    return;
  }
  if (page_handler_) {
    page_handler_->Ask(question);
  } else {
    pending_question_ = question;
  }
}

void ClaudePanel::SetPageHandler(ClaudeChatPageHandler* handler) {
  page_handler_ = handler;
}

void ClaudePanel::RemovePageHandler(ClaudeChatPageHandler* handler) {
  if (page_handler_ == handler) {
    page_handler_ = nullptr;
  }
}

std::string ClaudePanel::TakeQuestion() {
  return std::exchange(pending_question_, std::string());
}

bool ClaudePanel::IsShowing() const {
  const SidePanelUI* ui = SidePanelUI::From(browser_);
  return registered_ && ui && ui->IsSidePanelEntryShowing(EntryKey());
}

content::WebContents* ClaudePanel::web_contents_for_testing() {
  if (!IsShowing() || !view_.view()) {
    return nullptr;
  }
  auto* web_view = static_cast<views::WebView*>(
      view_.view()->GetViewByID(SidePanelWebUIView::kSidePanelWebViewId));
  return web_view ? web_view->web_contents() : nullptr;
}

void ClaudePanel::RegisterEntryIfNeeded() {
  if (registered_) {
    return;
  }
  SidePanelRegistry* registry = SidePanelRegistry::From(browser_);
  if (!registry) {
    return;
  }
  auto entry = std::make_unique<SidePanelEntry>(
      EntryKey(),
      base::BindRepeating(&ClaudePanel::CreateView, base::Unretained(this)),
      base::BindRepeating([] { return kDefaultWidth; }));
  // The page draws its own header (title, new chat, close).
  entry->set_should_show_header(false);
  registered_ = registry->Register(std::move(entry));
}

std::unique_ptr<views::View> ClaudePanel::CreateView(
    SidePanelEntryScope& scope) {
  auto wrapper = std::make_unique<WebUIContentsWrapperT<ClaudeChatUI>>(
      GURL(kClaudeChatURL), browser_->GetProfile(),
      IDS_TASK_MANAGER_TOOL_PREFIX,
      // Esc stops a reply, in the page.
      /*esc_closes_ui=*/false);
  // The chat sits on the window's tint, as the sidebar does, not in a box
  // (the side_panel.cc patch drops upstream's toolbar-colored one).
  views::WebContentsSetBackgroundColor::CreateForWebContentsWithColor(
      wrapper->web_contents(), SK_ColorTRANSPARENT);
  auto view = std::make_unique<SidePanelWebUIViewT<ClaudeChatUI>>(
      scope, base::RepeatingClosure(), base::RepeatingClosure(),
      std::move(wrapper));
  view_.SetView(view.get());
  return view;
}

bool HandleClaudeCommand(BrowserWindowInterface* browser, int id) {
  if (id != kToggleClaudeCommand) {
    return false;
  }
  if (ClaudePanel* panel = ClaudePanel::From(browser)) {
    panel->Toggle();
  }
  return true;
}

}  // namespace compass::claude
