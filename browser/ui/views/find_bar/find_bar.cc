// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/find_bar/find_bar.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "compass/browser/ui/views/frame/content_frame.h"
#include "compass/browser/ui/webui/find_bar/find_bar_ui.h"
#include "components/find_in_page/find_notification_details.h"
#include "components/find_in_page/find_types.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/page.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/page_transition_types.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/controls/webview/web_contents_set_background_color.h"
#include "ui/views/view_class_properties.h"
#include "url/gurl.h"

namespace compass {

namespace {

// The bar's distance from the page's top and trailing edges, past its
// shadow: the frame's inset, as the page's from the window.
constexpr int kMargin = kContentFrameInset;
// Around the bar the page draws, inside the view: room for its shadow
// (find_bar/app.css, --shadow-room).
constexpr int kShadowRoom = 12;

std::u16string Utf16(const std::string& text) {
  return base::UTF8ToUTF16(text);
}

// Whether a page's selection is something to look for: one short line.
bool IsFindableSelection(const std::u16string& text) {
  return !text.empty() && text.size() <= 200 &&
         text.find(u'\n') == std::u16string::npos;
}

// Whether a tab's bar is open. Not FindTabHelper::find_ui_active(), which
// upstream's FindBarController reads to show its own bar on a tab switch.
class FindBarTabState : public content::WebContentsUserData<FindBarTabState> {
 public:
  ~FindBarTabState() override = default;

  static bool IsOpen(content::WebContents* contents) {
    FindBarTabState* state = FromWebContents(contents);
    return state && state->open_;
  }
  static void SetOpen(content::WebContents* contents, bool open) {
    CreateForWebContents(contents);
    FromWebContents(contents)->open_ = open;
  }

 private:
  friend class content::WebContentsUserData<FindBarTabState>;
  explicit FindBarTabState(content::WebContents* contents)
      : content::WebContentsUserData<FindBarTabState>(*contents) {}

  bool open_ = false;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(FindBarTabState);

}  // namespace

DEFINE_USER_DATA(FindBar);

// static
FindBar* FindBar::Install(views::View* browser_view,
                          BrowserWindowInterface* browser) {
  auto* bar = browser_view->AddChildView(std::make_unique<FindBar>(browser));
  bar->host_observation_.Observe(browser_view);
  return bar;
}

// static
FindBar* FindBar::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

FindBar::FindBar(BrowserWindowInterface* browser)
    : views::WebView(nullptr),
      browser_(browser),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {
  SetProperty(views::kViewIgnoredByLayoutKey, true);
  SetVisible(false);
  active_tab_subscription_ = browser->RegisterActiveTabDidChange(
      base::BindRepeating(&FindBar::OnActiveTabChanged,
                          base::Unretained(this)));
  OnActiveTabChanged(browser);
}

FindBar::~FindBar() {
  helper_observation_.Reset();
  tab_observer_.Watch(nullptr);
  SetWebContents(nullptr);
  if (contents_) {
    contents_->SetDelegate(nullptr);
  }
}

void FindBar::Show() {
  find_in_page::FindTabHelper* helper = GetFindTabHelper();
  if (!helper) {
    return;
  }
  CreatePageIfNeeded();

  if (!IsShowing()) {
    // The page's selection, else what this tab looked for last, else what
    // the window looked for last (macOS's find pasteboard elsewhere).
    // The selection the browser knows of; the page is asked too, below.
    std::u16string text;
    if (content::RenderWidgetHostView* view =
            helper->GetWebContents().GetRenderWidgetHostView()) {
      text = view->GetSelectedText();
      if (!IsFindableSelection(text)) {
        text.clear();
      }
    }
    if (text.empty()) {
      text = helper->find_text();
    }
    if (text.empty()) {
      text = helper->previous_find_text();
    }
    if (text.empty()) {
      text = helper->GetInitialSearchText();
    }
    FindBarTabState::SetOpen(&helper->GetWebContents(), true);
    text_ = text;
    SendText(text_);
    UpdateBounds();
    SetVisible(true);
    if (!text_.empty()) {
      // Highlights the matches and moves to the first, as typing it would.
      helper->StartFinding(text_, /*forward_direction=*/true,
                           /*case_sensitive=*/false, /*find_match=*/true);
    } else if (page_) {
      page_->ClearResult();
    }
    // The browser hears of a selection only from a focused page; ask the
    // page itself, and start from its answer unless something was typed
    // meanwhile.
    typed_since_show_ = false;
    content::WebContents* contents = &helper->GetWebContents();
    content::RenderFrameHost* frame = contents->GetFocusedFrame();
    if (!frame) {
      frame = contents->GetPrimaryMainFrame();
    }
    if (frame && frame->IsRenderFrameLive()) {
      frame->ExecuteJavaScriptInIsolatedWorld(
          u"getSelection().toString()",
          base::BindOnce(&FindBar::OnPageSelection, weak_factory_.GetWeakPtr(),
                         contents->GetWeakPtr()),
          ISOLATED_WORLD_ID_CHROME_INTERNAL);
    }
  }
  FocusPage();
}

void FindBar::OnPageSelection(base::WeakPtr<content::WebContents> contents,
                              base::Value result) {
  find_in_page::FindTabHelper* helper = GetFindTabHelper();
  if (!contents || !helper || &helper->GetWebContents() != contents.get() ||
      !IsShowing() || typed_since_show_ || !result.is_string()) {
    return;
  }
  const std::u16string text = base::UTF8ToUTF16(result.GetString());
  if (!IsFindableSelection(text) || text == text_) {
    return;
  }
  text_ = text;
  SendText(text_);
  helper->StartFinding(text_, /*forward_direction=*/true,
                       /*case_sensitive=*/false, /*find_match=*/true);
  if (page_) {
    page_->FocusAndSelectAll();
  }
}

void FindBar::Close() {
  find_in_page::FindTabHelper* helper = GetFindTabHelper();
  if (helper && FindBarTabState::IsOpen(&helper->GetWebContents())) {
    helper->StopFinding(find_in_page::SelectionAction::kKeep);
    FindBarTabState::SetOpen(&helper->GetWebContents(), false);
    helper->set_find_ui_focused(false);
  }
  Hide(/*restore_focus=*/true);
}

bool FindBar::IsShowing() const {
  return GetVisible();
}

void FindBar::FindNextFromCommand(bool reverse) {
  if (!IsShowing()) {
    // Opening finds the first match already.
    Show();
    return;
  }
  FindNext(reverse);
}

void FindBar::BindPage(
    mojo::PendingRemote<find_bar::mojom::Page> page,
    mojo::PendingReceiver<find_bar::mojom::PageHandler> receiver) {
  page_.reset();
  receiver_.reset();
  page_.Bind(std::move(page));
  receiver_.Bind(std::move(receiver));
  // What happened while it loaded.
  SendText(text_);
  SendResult();
  if (focus_pending_) {
    focus_pending_ = false;
    page_->FocusAndSelectAll();
  }
}

void FindBar::Find(const std::string& text) {
  text_ = Utf16(text);
  typed_since_show_ = true;
  find_in_page::FindTabHelper* helper = GetFindTabHelper();
  if (!helper) {
    return;
  }
  // An empty text stops finding and clears the highlights
  // (OnFindEmptyText()).
  helper->StartFinding(text_, /*forward_direction=*/true,
                       /*case_sensitive=*/false, /*find_match=*/true);
}

void FindBar::FindNext(bool reverse) {
  find_in_page::FindTabHelper* helper = GetFindTabHelper();
  if (!helper || text_.empty()) {
    return;
  }
  helper->StartFinding(text_, /*forward_direction=*/!reverse,
                       /*case_sensitive=*/false, /*find_match=*/true);
}

void FindBar::ForwardScrollKey(find_bar::mojom::ScrollKey key) {
  find_in_page::FindTabHelper* helper = GetFindTabHelper();
  if (!helper) {
    return;
  }
  using Key = find_bar::mojom::ScrollKey;
  ui::KeyboardCode code = ui::VKEY_DOWN;
  int flags = ui::EF_NONE;
  switch (key) {
    case Key::kUp:
      code = ui::VKEY_UP;
      break;
    case Key::kDown:
      code = ui::VKEY_DOWN;
      break;
    case Key::kPageUp:
      code = ui::VKEY_PRIOR;
      break;
    case Key::kPageDown:
      code = ui::VKEY_NEXT;
      break;
    case Key::kHome:
      code = ui::VKEY_HOME;
      flags = ui::EF_CONTROL_DOWN;
      break;
    case Key::kEnd:
      code = ui::VKEY_END;
      flags = ui::EF_CONTROL_DOWN;
      break;
  }
  // As upstream's bar does (FindBarHost::MaybeForwardKeyEventToWebpage()):
  // a focused field in the page would take the arrows instead.
  content::WebContents* contents = &helper->GetWebContents();
  content::RenderWidgetHostView* view = contents->GetRenderWidgetHostView();
  if (!view) {
    return;
  }
  contents->ClearFocusedElement();
  ui::KeyEvent event(ui::EventType::kKeyPressed, code, flags);
  view->GetRenderWidgetHost()->ForwardKeyboardEvent(
      input::NativeWebKeyboardEvent(event));
}

void FindBar::OnFindResultAvailable(content::WebContents* web_contents) {
  SendResult();
}

void FindBar::OnFindEmptyText(content::WebContents* web_contents) {
  if (page_) {
    page_->ClearResult();
  }
}

void FindBar::OnFindTabHelperDestroyed(find_in_page::FindTabHelper* helper) {
  helper_observation_.Reset();
}

bool FindBar::HandleKeyboardEvent(content::WebContents* source,
                                  const input::NativeWebKeyboardEvent& event) {
  // After the page, which keeps the editing keys: Cmd+G, Cmd+F and the
  // window's other shortcuts, as from the tab.
  auto* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  return browser_view && browser_view->HandleKeyboardEvent(event);
}

void FindBar::TabObserver::PrimaryPageChanged(content::Page& page) {
  bar_->OnPrimaryPageChanged();
}

void FindBar::OnPrimaryPageChanged() {
  // Another page: the old one's matches are gone. The bar stays, with its
  // text, as upstream's does.
  if (IsShowing() && page_) {
    page_->ClearResult();
  }
}

void FindBar::OnChildViewAdded(views::View* observed_view,
                               views::View* child) {
  // Stays over what the browser view adds later (the page among them).
  if (child != this) {
    observed_view->ReorderChildView(this, observed_view->children().size());
  }
}

void FindBar::OnViewBoundsChanged(views::View* observed_view) {
  UpdateBounds();
}

void FindBar::OnViewIsDeleting(views::View* observed_view) {
  host_observation_.Reset();
}

void FindBar::OnActiveTabChanged(BrowserWindowInterface* browser) {
  helper_observation_.Reset();
  tabs::TabInterface* tab = browser->GetActiveTabInterface();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  tab_observer_.Watch(contents);
  find_in_page::FindTabHelper* helper =
      contents ? find_in_page::FindTabHelper::FromWebContents(contents)
               : nullptr;
  if (helper) {
    helper_observation_.Observe(helper);
  }
  // Each tab has its own bar, as upstream: open where it was left open, with
  // that tab's text and matches.
  if (!helper || !FindBarTabState::IsOpen(contents)) {
    Hide(/*restore_focus=*/false);
    return;
  }
  CreatePageIfNeeded();
  text_ = helper->find_text();
  SendText(text_);
  SendResult();
  UpdateBounds();
  SetVisible(true);
}

find_in_page::FindTabHelper* FindBar::GetFindTabHelper() {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  return tab ? find_in_page::FindTabHelper::FromWebContents(tab->GetContents())
             : nullptr;
}

void FindBar::CreatePageIfNeeded() {
  if (contents_) {
    return;
  }
  const GURL url(kCompassFindBarURL);
  Profile* profile = browser_->GetProfile();
  content::WebContents::CreateParams params(profile);
  params.site_instance = content::SiteInstance::CreateForURL(profile, url);
  contents_ = content::WebContents::Create(params);
  // As the WebView does with its own: keys the page doesn't handle (Cmd+G,
  // Cmd+F, the window's shortcuts) go to the window.
  contents_->SetDelegate(this);
  // The page draws the bar and its shadow; the page under it shows around.
  views::WebContentsSetBackgroundColor::CreateForWebContentsWithColor(
      contents_.get(), SK_ColorTRANSPARENT);
  // The page asks for its window when it connects (FindBarUI).
  webui::SetBrowserWindowInterface(contents_.get(), browser_);
  SetWebContents(contents_.get());
  contents_->GetController().LoadURL(url, content::Referrer(),
                                     ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                     std::string());
}

void FindBar::SendText(const std::u16string& text) {
  if (!page_) {
    return;
  }
  const std::string utf8 = base::UTF16ToUTF8(text);
  page_->SetText(utf8, 0, static_cast<uint32_t>(text.size()));
}

void FindBar::SendResult() {
  if (!page_) {
    return;
  }
  find_in_page::FindTabHelper* helper = GetFindTabHelper();
  if (!helper || text_.empty() || helper->find_text() != text_) {
    page_->ClearResult();
    return;
  }
  const find_in_page::FindNotificationDetails& result = helper->find_result();
  if (result.number_of_matches() < 0) {
    page_->ClearResult();
    return;
  }
  page_->SetResult(std::max(result.active_match_ordinal(), 0),
                   result.number_of_matches(), result.final_update());
}

void FindBar::FocusPage() {
  RequestFocus();
  if (find_in_page::FindTabHelper* helper = GetFindTabHelper()) {
    helper->set_find_ui_focused(true);
  }
  if (page_) {
    page_->FocusAndSelectAll();
  } else {
    focus_pending_ = true;
  }
}

void FindBar::UpdateBounds() {
  if (!parent()) {
    return;
  }
  auto* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  MultiContentsView* multi =
      browser_view ? browser_view->multi_contents_view() : nullptr;
  views::View* pane = multi ? multi->GetActiveContentsContainerView() : nullptr;
  if (!pane || !pane->GetVisible() || pane->bounds().IsEmpty()) {
    SetBoundsRect(gfx::Rect());
    return;
  }
  const gfx::Rect card =
      views::View::ConvertRectToTarget(pane, parent(), pane->GetLocalBounds());
  // The bar's edge kMargin inside the page's; the view is larger by its
  // shadow's room.
  const int width = std::min(kWidth, card.width());
  const int x = base::i18n::IsRTL()
                    ? card.x() + kMargin - kShadowRoom
                    : card.right() - kMargin + kShadowRoom - width;
  SetBoundsRect(
      gfx::Rect(x, card.y() + kMargin - kShadowRoom, width, kHeight));
}

void FindBar::Hide(bool restore_focus) {
  const bool had_focus = HasFocus();
  focus_pending_ = false;
  SetVisible(false);
  if (restore_focus && had_focus) {
    if (tabs::TabInterface* tab = browser_->GetActiveTabInterface()) {
      tab->GetContents()->Focus();
    }
  }
}

BEGIN_METADATA(FindBar)
END_METADATA

bool HandleFindInPage(BrowserWindowInterface* browser,
                      bool find_next,
                      bool forward_direction) {
  FindBar* bar = FindBar::From(browser);
  if (!bar) {
    // No Compass frame (a popup, an app window): upstream's bar.
    return false;
  }
  if (find_next) {
    bar->FindNextFromCommand(/*reverse=*/!forward_direction);
  } else {
    bar->Show();
  }
  return true;
}

}  // namespace compass
