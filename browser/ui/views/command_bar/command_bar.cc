// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/command_bar/command_bar.h"

#include <algorithm>
#include <string>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "chrome/app/chrome_command_ids.h"
#include "compass/browser/ui/app_menu_sync.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/bubble/webui_bubble_dialog_view.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/webui/top_chrome/webui_contents_wrapper.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/generated_resources.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "compass/browser/ui/webui/command_bar/command_bar_page_handler.h"
#include "compass/browser/ui/views/claude/claude_panel.h"
#include "compass/browser/ui/views/little_window/little_window.h"
#include "compass/browser/ui/views/peek/peek.h"
#include "compass/browser/ui/webui/command_bar/command_bar_ui.h"
#include "compass/browser/window_sync/empty_tab.h"
#include "compass/browser/window_sync/undo_close.h"
#include "compass/common/compass_url.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace compass {

namespace {

using command_bar::mojom::Mode;

// How far below the top of the content area the bar hangs: a quarter of its
// height, near the middle as Arc's, but at least this.
constexpr int kMinTopOffset = 48;

// The text the bar starts from in current-entry mode: the active tab's URL,
// unless it's a new tab page.
std::u16string GetCurrentUrlText(BrowserWindowInterface* browser) {
  tabs::TabInterface* tab = browser->GetActiveTabInterface();
  if (!tab) {
    return std::u16string();
  }
  const GURL url = ToChromeURL(tab->GetContents()->GetVisibleURL());
  if (IsEmptyTabURL(url)) {
    return std::u16string();
  }
  return base::UTF8ToUTF16(ToCompassURL(url).spec());
}

}  // namespace

DEFINE_USER_DATA(CommandBar);

CommandBar::CommandBar(BrowserWindowInterface* browser)
    : browser_(browser),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {}

CommandBar::~CommandBar() {
  Close();
}

// static
CommandBar* CommandBar::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

void CommandBar::Show(Mode mode) {
  mode_ = mode;
  text_ = mode == Mode::kCurrentEntry ? GetCurrentUrlText(browser_)
                                      : std::u16string();
  if (bubble_) {
    // Start over in the new mode.
    if (CommandBarPageHandler* handler = GetPageHandler()) {
      handler->Open(mode_, text_);
    }
    return;
  }

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  if (!browser_view) {
    return;
  }
  const gfx::Rect content =
      browser_view->contents_container()->GetBoundsInScreen();
  const gfx::Rect anchor(content.CenterPoint().x(),
                         content.y() + std::max(kMinTopOffset,
                                                content.height() / 4),
                         0, 0);

  WebUIContentsWrapper* contents = GetOrCreateContents();
  auto bubble = std::make_unique<WebUIBubbleDialogView>(
      nullptr, contents->GetWeakPtr(), anchor,
      views::BubbleBorder::TOP_CENTER);
  bubble->set_parent_window(platform_util::GetViewForWindow(
      browser_->GetWindow()->GetNativeWindow()));
  bubble_ = bubble->GetWeakPtr();
  // Hidden until the page has shown the new state (ShowUI()).
  views::BubbleDialogDelegateView::CreateBubble(std::move(bubble));

  if (CommandBarPageHandler* handler = GetPageHandler()) {
    handler->Open(mode_, text_);
  }
  // Otherwise the page is still loading; OnPageHandlerCreated() opens it.
}

void CommandBar::Close() {
  if (bubble_ && bubble_->GetWidget()) {
    bubble_->GetWidget()->CloseNow();
  }
  bubble_ = nullptr;
}

bool CommandBar::IsShowing() const {
  return bubble_ && bubble_->GetWidget() && bubble_->GetWidget()->IsVisible();
}

void CommandBar::Preload() {
  GetOrCreateContents();
}

void CommandBar::OnPageHandlerCreated(CommandBarPageHandler* handler) {
  if (bubble_) {
    handler->Open(mode_, text_);
  }
}

content::WebContents* CommandBar::web_contents_for_testing() {
  return contents_ ? contents_->web_contents() : nullptr;
}

WebUIContentsWrapper* CommandBar::GetOrCreateContents() {
  if (!contents_) {
    contents_ = std::make_unique<WebUIContentsWrapperT<CommandBarUI>>(
        GURL(kCompassCommandBarURL), browser_->GetProfile(),
        IDS_TASK_MANAGER_TOOL_PREFIX);
    // The page asks for its window when it connects, which is later.
    webui::SetBrowserWindowInterface(contents_->web_contents(), browser_);
  }
  return contents_.get();
}

CommandBarPageHandler* CommandBar::GetPageHandler() {
  if (!contents_) {
    return nullptr;
  }
  CommandBarUI* ui =
      static_cast<WebUIContentsWrapperT<CommandBarUI>*>(contents_.get())
          ->GetWebUIController();
  return ui ? ui->page_handler() : nullptr;
}

namespace {

// The page of |browser|'s window that has the focus (the active tab's if
// the window has no focused view), if what's focused in it is editable: a
// field, or a document being edited.
content::WebContents* GetEditingContents(BrowserWindowInterface* browser) {
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  views::FocusManager* focus_manager =
      browser_view ? browser_view->GetFocusManager() : nullptr;
  views::View* focused =
      focus_manager ? focus_manager->GetFocusedView() : nullptr;
  content::WebContents* contents = nullptr;
  if (views::WebView* web_view = views::AsViewClass<views::WebView>(focused)) {
    contents = web_view->web_contents();
  } else if (!focused) {
    contents = browser->GetTabStripModel()->GetActiveWebContents();
  }
  return contents && contents->IsFocusedElementEditable() ? contents : nullptr;
}

}  // namespace

bool IsBrowserCommandEnabled(BrowserWindowInterface* browser, int id) {
  return id == IDC_RESTORE_TAB && CanUndoClose(browser);
}

bool HandleBrowserCommand(BrowserWindowInterface* browser, int id) {
  if (id == kUndoCloseTabCommand || id == kRedoCloseTabCommand) {
    // They come only after the page passed on the keys (shortcuts_mac.h).
    // In text being edited, they're the Edit menu's Undo and Redo.
    if (content::WebContents* editing = GetEditingContents(browser)) {
      if (id == kUndoCloseTabCommand) {
        editing->Undo();
      } else {
        editing->Redo();
      }
    } else if (id == kUndoCloseTabCommand) {
      UndoClose(browser);
    } else {
      RedoClose(browser);
    }
    return true;
  }
  // Cmd+Shift+T and Reopen Closed Tab bring back the last closed entry,
  // loaded or not. Upstream's restore takes what Compass doesn't have (a
  // closed window).
  if (id == IDC_RESTORE_TAB && UndoClose(browser)) {
    return true;
  }
  if (id == kToggleTabMuteCommand) {
    // The active tab (the focused pane of a split), as its speaker in the
    // sidebar would.
    if (content::WebContents* contents =
            browser->GetTabStripModel()->GetActiveWebContents()) {
      SetTabAudioMuted(contents, !contents->IsAudioMuted(),
                       TabMutedReason::kAudioIndicator, std::string());
    }
    return true;
  }
  if (claude::HandleClaudeCommand(browser, id) ||
      HandlePeekCommand(browser, id) ||
      HandleLittleWindowCommand(browser, id) ||
      HandleAppMenuSyncCommand(browser, id)) {
    return true;
  }
  if (id != IDC_NEW_TAB && id != IDC_FOCUS_LOCATION) {
    return false;
  }
  CommandBar* bar = CommandBar::From(browser);
  if (!bar) {
    return false;
  }
  bar->Show(id == IDC_NEW_TAB ? Mode::kNewEntry : Mode::kCurrentEntry);
  return true;
}

}  // namespace compass
