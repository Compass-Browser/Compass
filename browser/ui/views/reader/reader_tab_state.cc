// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/reader/reader_tab_state.h"

#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_capture_border_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/tabs/public/tab_interface.h"
#include "compass/browser/reader/reader_assets.h"
#include "compass/browser/ui/webui/reader/reader_untrusted_ui.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/controls/webview/webview.h"
#include "url/url_constants.h"

namespace compass {

namespace {

// After a same-document navigation (single-page apps), and once after load
// for pages that build their text late.
constexpr base::TimeDelta kRecheckDelay = base::Seconds(1);
constexpr base::TimeDelta kRecheckAfterLoadDelay = base::Seconds(2);

constexpr int kKeyModifiers =
    blink::WebInputEvent::kShiftKey | blink::WebInputEvent::kControlKey |
    blink::WebInputEvent::kAltKey | blink::WebInputEvent::kMetaKey;

bool IsKeyDown(const input::NativeWebKeyboardEvent& event,
               ui::KeyboardCode key,
               int modifiers) {
  return event.GetType() == blink::WebInputEvent::Type::kRawKeyDown &&
         event.windows_key_code == key &&
         (event.GetModifiers() & kKeyModifiers) == modifiers;
}

// Readability-readerable.js and reader.js, then check(): only the small
// script, on every page load.
const std::u16string& GetCheckScript() {
  static const base::NoDestructor<std::u16string> script(
      base::UTF8ToUTF16(base::StrCat(
          {reader::assets::kReaderable, "\n;\n", reader::assets::kReaderJs,
           "\n;__compassReader.check({});"})));
  return *script;
}

// With Readability, then parse(). Each script defines its globals once per
// world; reader.js keeps the one a check defined.
const std::u16string& GetParseScript() {
  static const base::NoDestructor<std::u16string> script(
      base::UTF8ToUTF16(base::StrCat(
          {reader::assets::kReadability, "\n;\n", reader::assets::kReaderable,
           "\n;\n", reader::assets::kReaderJs,
           "\n;__compassReader.parse({});"})));
  return *script;
}

// Runs |script| in an isolated world of |contents|' main frame: the page's
// scripts can't see or change it, and its CSP doesn't apply.
void RunInPage(content::WebContents* contents,
               const std::u16string& script,
               base::OnceCallback<void(base::Value)> callback) {
  content::RenderFrameHost* frame = contents->GetPrimaryMainFrame();
  if (!frame || !frame->IsRenderFrameLive()) {
    std::move(callback).Run(base::Value());
    return;
  }
  frame->ExecuteJavaScriptInIsolatedWorld(script, std::move(callback),
                                          ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

base::RepeatingCallbackList<void(content::WebContents*)>& ChangedCallbacks() {
  static base::NoDestructor<
      base::RepeatingCallbackList<void(content::WebContents*)>>
      callbacks;
  return *callbacks;
}

BrowserWindowInterface* GetBrowserFor(content::WebContents* contents) {
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  return tab ? tab->GetBrowserWindowInterface() : nullptr;
}

}  // namespace

ReaderTabState::ReaderTabState(content::WebContents* contents)
    : content::WebContentsUserData<ReaderTabState>(*contents),
      content::WebContentsObserver(contents) {
  // A page that finished loading before Reader looked at it.
  if (!contents->IsLoading()) {
    Check();
  }
}

ReaderTabState::~ReaderTabState() {
  Teardown();
}

// static
base::CallbackListSubscription ReaderTabState::RegisterChanged(
    base::RepeatingCallback<void(content::WebContents*)> callback) {
  return ChangedCallbacks().Add(std::move(callback));
}

// static
bool ReaderTabState::CanRead(const GURL& url) {
  return url.is_valid() && (url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile());
}

bool ReaderTabState::CanEnter() const {
  return !showing_ && !parsing_ &&
         CanRead(GetWebContents().GetLastCommittedURL());
}

void ReaderTabState::Enter() {
  if (!CanEnter()) {
    return;
  }
  parsing_ = true;
  RunInPage(&GetWebContents(), GetParseScript(),
            base::BindOnce(&ReaderTabState::OnParsed,
                           weak_factory_.GetWeakPtr(), ++parse_id_));
}

void ReaderTabState::Leave() {
  // A parse still running is dropped.
  ++parse_id_;
  parsing_ = false;
  if (!showing_) {
    return;
  }
  showing_ = false;
  const bool focus_page = IsActiveTab();
  Teardown();
  if (focus_page) {
    GetWebContents().Focus();
  }
  NotifyChanged();
}

void ReaderTabState::OnParsed(int parse_id, base::Value result) {
  if (parse_id != parse_id_ || !parsing_) {
    return;
  }
  parsing_ = false;
  if (!result.is_dict()) {
    result = base::Value(
        base::DictValue()
            .Set("ok", false)
            .Set("error", "This page couldn't be read (it may still be "
                          "loading)."));
  }
  std::string json;
  base::JSONWriter::Write(result, &json);
  Show(std::move(json));
}

void ReaderTabState::Show(std::string json) {
  content::BrowserContext* context = GetWebContents().GetBrowserContext();
  token_ = ReaderArticleStore::Get().Add(context, std::move(json));
  // The tab's profile, Incognito too: the view's storage (its settings) is
  // the profile's.
  reader_contents_ =
      content::WebContents::Create(content::WebContents::CreateParams(context));
  reader_contents_->SetDelegate(this);
  reader_contents_->GetController().LoadURL(
      GetReaderPageUrl(token_), content::Referrer(),
      ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
  showing_ = true;
  // ReaderMode puts the view over the tab's pane.
  NotifyChanged();
}

void ReaderTabState::AttachTo(ContentsContainerView* container) {
  if (container && container == container_ && view_) {
    return;
  }
  if (view_) {
    view_observations_.RemoveAllObservations();
    detached_subscription_ = {};
    views::WebView* view = view_;
    ContentsContainerView* old_container = container_;
    view_ = nullptr;
    container_ = nullptr;
    std::unique_ptr<views::WebView> owned =
        old_container->RemoveChildViewT(view);
    owned->SetWebContents(nullptr);
  }
  container_ = nullptr;
  if (!container || !reader_contents_) {
    return;
  }

  container_ = container;
  auto view = std::make_unique<views::WebView>(
      GetWebContents().GetBrowserContext());
  view->SetWebContents(reader_contents_.get());
  // Over the page and everything else in the pane, but under the capture
  // border, which ContentsContainerView keeps on top.
  views::View* border = container->capture_contents_border_view();
  std::optional<size_t> border_index =
      border ? container->GetIndexOf(border) : std::nullopt;
  view_ = border_index
              ? container->AddChildViewAt(std::move(view), *border_index)
              : container->AddChildView(std::move(view));
  views::View* contents_view = container->contents_view();
  view_->SetBoundsRect(contents_view->bounds());
  view_observations_.AddObservation(view_);
  view_observations_.AddObservation(contents_view);
  // When the pane shows another tab, the view goes (ReaderMode puts it back
  // when this tab shows again).
  detached_subscription_ =
      container->contents_view()->AddWebContentsDetachedCallback(
          base::BindRepeating(
              [](base::WeakPtr<ReaderTabState> state, views::WebView*) {
                base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                    FROM_HERE, base::BindOnce(
                                   [](base::WeakPtr<ReaderTabState> state) {
                                     if (state && state->container_ &&
                                         state->container_->contents_view()
                                                 ->web_contents() !=
                                             &state->GetWebContents()) {
                                       state->AttachTo(nullptr);
                                     }
                                   },
                                   state));
              },
              weak_factory_.GetWeakPtr()));
  if (IsActiveTab()) {
    view_->RequestFocus();
  }
}

void ReaderTabState::Check() {
  recheck_timer_.Stop();
  if (!CanRead(GetWebContents().GetLastCommittedURL())) {
    checked_ = true;
    SetReaderable(false);
    return;
  }
  RunInPage(&GetWebContents(), GetCheckScript(),
            base::BindOnce(&ReaderTabState::OnChecked,
                           weak_factory_.GetWeakPtr(), generation_));
}

void ReaderTabState::OnChecked(int generation, base::Value result) {
  if (generation != generation_) {
    return;
  }
  checked_ = true;
  const base::DictValue* dict = result.GetIfDict();
  SetReaderable(dict && dict->FindBool("readerable").value_or(false));
}

void ReaderTabState::SetReaderable(bool readerable) {
  if (readerable_ == readerable) {
    return;
  }
  readerable_ = readerable;
  NotifyChanged();
}

void ReaderTabState::NotifyChanged() {
  ChangedCallbacks().Notify(&GetWebContents());
}

void ReaderTabState::Teardown() {
  AttachTo(nullptr);
  if (reader_contents_) {
    reader_contents_->SetDelegate(nullptr);
    reader_contents_.reset();
  }
  if (!token_.empty()) {
    ReaderArticleStore::Get().Remove(token_);
    token_.clear();
  }
}

BrowserView* ReaderTabState::GetBrowserView() const {
  BrowserWindowInterface* browser =
      GetBrowserFor(&const_cast<content::WebContents&>(GetWebContents()));
  return browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
}

bool ReaderTabState::IsActiveTab() const {
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(
      &const_cast<content::WebContents&>(GetWebContents()));
  return tab && tab->IsActivated();
}

void ReaderTabState::PostLeave() {
  // Not while the view's WebContents is in the middle of an event.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&ReaderTabState::Leave,
                                weak_factory_.GetWeakPtr()));
}

void ReaderTabState::DocumentOnLoadCompletedInPrimaryMainFrame() {
  Check();
  recheck_timer_.Start(FROM_HERE, kRecheckAfterLoadDelay,
                       base::BindOnce(&ReaderTabState::Check,
                                      base::Unretained(this)));
}

void ReaderTabState::PrimaryPageChanged(content::Page& page) {
  // A new document: whatever Reader showed was the old one's.
  ++generation_;
  checked_ = false;
  recheck_timer_.Stop();
  const bool was_readerable = readerable_;
  const bool was_showing = showing_;
  readerable_ = false;
  // Leave() tells observers if it was showing.
  Leave();
  if (was_readerable && !was_showing) {
    NotifyChanged();
  }
}

void ReaderTabState::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (navigation_handle->IsInPrimaryMainFrame() &&
      navigation_handle->IsSameDocument() &&
      navigation_handle->HasCommitted()) {
    recheck_timer_.Start(FROM_HERE, kRecheckDelay,
                         base::BindOnce(&ReaderTabState::Check,
                                        base::Unretained(this)));
  }
}

content::WebContents* ReaderTabState::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)>
        navigation_handle_callback) {
  // Links in the article: the reader page is WebUI, so Blink hands every
  // navigation to the browser.
  if (source != reader_contents_.get() ||
      !(params.url.SchemeIsHTTPOrHTTPS() ||
        params.url.SchemeIs(url::kMailToScheme))) {
    return nullptr;
  }
  if (params.disposition == WindowOpenDisposition::CURRENT_TAB) {
    // The tab goes there, which leaves Reader (PrimaryPageChanged()).
    content::NavigationController::LoadURLParams load(params.url);
    load.transition_type = ui::PAGE_TRANSITION_LINK;
    load.has_user_gesture = params.user_gesture;
    base::WeakPtr<content::NavigationHandle> handle =
        GetWebContents().GetController().LoadURLWithParams(load);
    if (handle && navigation_handle_callback) {
      std::move(navigation_handle_callback).Run(*handle);
    }
    return &GetWebContents();
  }
  // New tabs and windows open as from the tab, without a referrer.
  BrowserWindowInterface* browser = GetBrowserFor(&GetWebContents());
  if (!browser) {
    return nullptr;
  }
  NavigateParams navigate(browser, params.url, ui::PAGE_TRANSITION_LINK);
  navigate.disposition = params.disposition;
  navigate.source_contents = &GetWebContents();
  navigate.user_gesture = params.user_gesture;
  base::WeakPtr<content::NavigationHandle> handle = Navigate(&navigate);
  if (handle && navigation_handle_callback) {
    std::move(navigation_handle_callback).Run(*handle);
  }
  return navigate.navigated_or_inserted_contents;
}

content::WebContents* ReaderTabState::AddNewContents(
    content::WebContents* source,
    std::unique_ptr<content::WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& window_features,
    bool user_gesture,
    bool* was_blocked) {
  BrowserWindowInterface* browser = GetBrowserFor(&GetWebContents());
  if (!browser) {
    return nullptr;
  }
  return chrome::AddWebContents(browser, &GetWebContents(),
                                std::move(new_contents), target_url,
                                disposition, window_features);
}

void ReaderTabState::CloseContents(content::WebContents* source) {
  // The view's close button (window.close()).
  PostLeave();
}

content::KeyboardEventProcessingResult ReaderTabState::PreHandleKeyboardEvent(
    content::WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  // The window's own shortcuts (Cmd+T, Cmd+L, Reader's own, ...) as from the
  // tab.
  BrowserView* browser_view = GetBrowserView();
  return browser_view ? browser_view->PreHandleKeyboardEvent(event)
                      : content::KeyboardEventProcessingResult::NOT_HANDLED;
}

bool ReaderTabState::HandleKeyboardEvent(
    content::WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  // After the page, which uses Esc to close its settings panel first.
  if (IsKeyDown(event, ui::VKEY_ESCAPE, 0)) {
    PostLeave();
    return true;
  }
  BrowserView* browser_view = GetBrowserView();
  return browser_view && browser_view->HandleKeyboardEvent(event);
}

void ReaderTabState::OnViewBoundsChanged(views::View* observed_view) {
  if (view_ && container_ && observed_view != view_) {
    view_->SetBoundsRect(container_->contents_view()->bounds());
  }
}

void ReaderTabState::OnViewIsDeleting(views::View* observed_view) {
  // The pane is going (its window closes): it deletes the view itself.
  view_observations_.RemoveAllObservations();
  detached_subscription_ = {};
  view_ = nullptr;
  container_ = nullptr;
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(ReaderTabState);

}  // namespace compass
