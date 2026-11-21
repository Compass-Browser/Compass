// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/settings_window/settings_window.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/containers/flat_map.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "compass/browser/ui/settings_window_hooks.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/browser/window_sync/window_features.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "third_party/blink/public/mojom/page/draggable_region.mojom.h"
#include "ui/base/base_window.h"
#include "ui/base/hit_test.h"
#include "ui/base/page_transition_types.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/draggable_region.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/window/client_view.h"
#include "url/gurl.h"

namespace compass {

namespace {

// The window's height before the page says how much it needs, and the least
// it takes.
constexpr int kInitialHeight = 560;
constexpr int kMinHeight = 320;
// The most of the screen's height it takes; a taller pane scrolls.
constexpr float kMaxScreenFraction = 0.85f;

// Compass's sections, which the window has under upstream's paths
// (settings_sections.h), and the window's panes whose names upstream doesn't
// use. Upstream's sections the window has rebuilt (privacy's) are mapped in
// SettingsWindowPathFor(); the others ("payments", "ai", "safetyCheck") stay
// upstream's until the window rebuilds them.
constexpr auto kWindowPaths = std::to_array<std::string_view>({
    "general",
    "profiles",
    "sync",
    "importArc",
    "links",
    "shortcuts",
    "boosts",
    "plugins",
    "advanced",
    "devMode",
});

base::flat_map<Profile*, SettingsWindow*>& Windows() {
  static base::NoDestructor<base::flat_map<Profile*, SettingsWindow*>> windows;
  return *windows;
}

// The client view: the page's toolbar's empty parts drag the window.
class SettingsClientView : public views::ClientView {
 public:
  SettingsClientView(views::Widget* widget,
                     views::View* contents,
                     base::WeakPtr<SettingsWindow> window)
      : views::ClientView(widget, contents), window_(std::move(window)) {}

  int NonClientHitTest(const gfx::Point& point) override {
    if (window_ && window_->IsDraggableAt(point)) {
      return HTCAPTION;
    }
    return views::ClientView::NonClientHitTest(point);
  }

 private:
  base::WeakPtr<SettingsWindow> window_;
};

class SettingsWidgetDelegate : public views::WidgetDelegate {
 public:
  explicit SettingsWidgetDelegate(base::WeakPtr<SettingsWindow> window)
      : window_(std::move(window)) {
    SetTitle(u"Settings");
    SetCanResize(false);
    SetCanMinimize(true);
    SetShowCloseButton(true);
    // The page draws "Settings" itself, over the toolbar.
#if BUILDFLAG(IS_MAC)
    SetShowTitle(false);
#endif
  }

  // On Windows and Linux, events in the toolbar's empty parts stay with the
  // widget (they'd go to the page's native window), so they drag.
  bool ShouldDescendIntoChildForEventHandling(
      gfx::NativeView child,
      const gfx::Point& location) override {
    return !window_ || !window_->IsDraggableAtWidgetPoint(location);
  }

  views::ClientView* CreateClientView(views::Widget* widget) override {
    return new SettingsClientView(widget, TransferOwnershipOfContentsView(),
                                  window_);
  }

 private:
  base::WeakPtr<SettingsWindow> window_;
};

}  // namespace

std::optional<std::string> SettingsWindowPathFor(std::string_view sub_page) {
  std::string_view path = sub_page.substr(0, sub_page.find_first_of("?#"));
  if (path.starts_with('/')) {
    path.remove_prefix(1);
  }
  if (path.empty()) {
    return std::string(sub_page.substr(sub_page.starts_with('/') ? 1 : 0));
  }
  // Claude's section is the AI pane.
  if (path == "claude") {
    return base::StrCat({"ai", sub_page.substr(sub_page.find("claude") + 6)});
  }
  // Upstream's search sections are General's (phase 5b).
  if (path == "searchEngines") {
    return std::string("general?view=engines");
  }
  if (path == "search" || path == "defaultBrowser" || path == "downloads" ||
      path == "appearance" || path == "fonts") {
    return base::StrCat(
        {"general?focus=", path == "fonts" ? "fontSize" : path});
  }
  // Upstream's privacy sections are the Privacy pane's (phase 3): its views
  // and parts, by query (privacy_pane.ts).
  if (path == "privacy") {
    return std::string("privacy");
  }
  // Upstream's advanced sections are the Advanced pane's (phase 5).
  if (path == "performance" || path == "accessibility" || path == "reset") {
    return base::StrCat({"advanced?focus=", path});
  }
  if (path == "help") {
    return std::string("advanced?focus=about");
  }
  // And its passwords' (phase 4). Payments and addresses stay upstream's: the
  // pane opens them to add or edit one.
  if (path == "passwords") {
    return std::string("passwords");
  }
  if (path == "security" || path == "cookies") {
    return base::StrCat({"privacy?focus=", path});
  }
  if (path == "clearBrowserData") {
    return std::string("privacy?view=clear");
  }
  if (path == "content/siteDetails") {
    const size_t query = sub_page.find('?');
    std::string_view site;
    if (query != std::string_view::npos) {
      const std::string_view params = sub_page.substr(query + 1);
      if (params.starts_with("site=")) {
        site = params.substr(5, params.find('&') == std::string_view::npos
                                    ? std::string_view::npos
                                    : params.find('&') - 5);
      }
    }
    return site.empty() ? std::string("privacy?view=sites")
                        : base::StrCat({"privacy?site=", site});
  }
  if (path == "content/all") {
    return std::string("privacy?view=sites");
  }
  if (path == "content" || path.starts_with("content/")) {
    return std::string("privacy?view=permissions");
  }
  if (std::ranges::contains(kWindowPaths, path)) {
    return std::string(sub_page.substr(sub_page.starts_with('/') ? 1 : 0));
  }
  return std::nullopt;
}

GURL SettingsWindowURL(std::string_view path) {
  return GURL(base::StrCat(
      {content::kChromeUIScheme, "://", kSettingsHost, "/", path}));
}

bool MaybeShowSettingsWindow(Profile* profile, std::string_view sub_page) {
  std::optional<std::string> path = SettingsWindowPathFor(sub_page);
  if (!profile || !path) {
    return false;
  }
  SettingsWindow::Show(profile, *path);
  return true;
}

// static
void SettingsWindow::Show(Profile* profile, std::string_view path) {
  SettingsWindow* window = FromProfile(profile);
  if (window && window->widget_ && !window->widget_->IsClosed()) {
    window->ShowPath(path);
    window->widget_->Activate();
    return;
  }
  // Deletes itself when it closes.
  window = new SettingsWindow(profile);
  window->Open(path);
}

// static
SettingsWindow* SettingsWindow::FromWebContents(
    content::WebContents* contents) {
  for (auto& [profile, window] : Windows()) {
    if (window->contents_.get() == contents) {
      return window;
    }
  }
  return nullptr;
}

// static
SettingsWindow* SettingsWindow::FromProfile(Profile* profile) {
  auto it = Windows().find(profile);
  return it == Windows().end() ? nullptr : it->second;
}

SettingsWindow::SettingsWindow(Profile* profile) : profile_(profile) {
  Windows()[profile] = this;
  profile_observation_.Observe(profile);
}

SettingsWindow::~SettingsWindow() {
  Forget();
  if (contents_) {
    contents_->SetDelegate(nullptr);
  }
}

void SettingsWindow::Open(std::string_view path) {
  contents_ = content::WebContents::Create(
      content::WebContents::CreateParams(profile_));
  contents_->SetDelegate(this);
  // The toolbar's empty parts (app-region: drag) move the window.
  contents_->SetSupportsDraggableRegions(true);

  auto web_view = std::make_unique<views::WebView>(profile_);
  web_view_ = web_view.get();
  auto contents_view = std::make_unique<views::View>();
  contents_view->SetLayoutManager(std::make_unique<views::FillLayout>());
  contents_view->AddChildView(std::move(web_view));

  widget_delegate_ =
      std::make_unique<SettingsWidgetDelegate>(weak_factory_.GetWeakPtr());
  widget_delegate_->SetContentsView(std::move(contents_view));
  widget_delegate_->SetInitiallyFocusedView(web_view_);

  // Centered on the last active window's display, toward its top.
  display::Screen* screen = display::Screen::Get();
  BrowserWindowInterface* browser = FindLastActiveWindow(profile_);
  display::Display display = browser
                                 ? screen->GetDisplayNearestWindow(
                                       browser->GetWindow()->GetNativeWindow())
                                 : screen->GetPrimaryDisplay();
  const gfx::Rect area = display.work_area();
  const int height = std::min(kInitialHeight, area.height());
  gfx::Rect bounds(area.x() + (area.width() - kWidth) / 2,
                   area.y() + std::max(0, (area.height() - height) / 4), kWidth,
                   height);

  views::Widget::InitParams params(
      views::Widget::InitParams::CLIENT_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_WINDOW);
  params.delegate = widget_delegate_.get();
  params.name = "CompassSettingsWindow";
  params.bounds = bounds;
  widget_ = std::make_unique<views::Widget>();
#if BUILDFLAG(IS_MAC)
  params.native_widget = CreateSettingsNativeWidget(widget_.get());
#endif
  widget_->Init(std::move(params));
  widget_observation_.Observe(widget_.get());

  web_view_->SetWebContents(contents_.get());
  contents_->GetController().LoadURL(
      SettingsWindowURL(path), content::Referrer(),
      ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
  widget_->Show();
  web_view_->RequestFocus();
}

void SettingsWindow::ShowPath(std::string_view path) {
  if (show_path_) {
    show_path_.Run(std::string(path));
    return;
  }
  // The page isn't up yet: load it at |path| instead.
  contents_->GetController().LoadURL(
      SettingsWindowURL(path), content::Referrer(),
      ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
}

void SettingsWindow::SetShowPathCallback(
    base::RepeatingCallback<void(const std::string&)> callback) {
  show_path_ = std::move(callback);
}

void SettingsWindow::SetContentHeight(int height) {
  if (!widget_ || widget_->IsClosed() || height <= 0) {
    return;
  }
  const gfx::Rect current = widget_->GetWindowBoundsInScreen();
  const gfx::Rect area =
      display::Screen::Get()->GetDisplayMatching(current).work_area();
  const int max_height = std::max(
      kMinHeight, static_cast<int>(area.height() * kMaxScreenFraction));
  // The page's height is the client area's; the frame (none on macOS, where
  // the page runs under the title bar) comes on top.
  const int frame =
      current.height() - widget_->GetClientAreaBoundsInScreen().height();
  const int new_height = std::clamp(height + frame, kMinHeight, max_height);
  if (new_height == current.height()) {
    return;
  }
  gfx::Rect bounds = current;
  bounds.set_height(new_height);
  // Kept on the screen: grows upward if it would go past the bottom.
  if (bounds.bottom() > area.bottom()) {
    bounds.set_y(std::max(area.y(), area.bottom() - new_height));
  }
#if BUILDFLAG(IS_MAC)
  AnimateSettingsWindowBounds(widget_.get(), bounds);
#else
  widget_->SetBounds(bounds);
#endif
}

void SettingsWindow::Close() {
  // Settings opened from now on get a new window.
  Forget();
  if (widget_ && !widget_->IsClosed()) {
    widget_->Close();
  }
}

bool SettingsWindow::IsDraggableAtWidgetPoint(const gfx::Point& point) const {
  if (!web_view_) {
    return false;
  }
  gfx::Point in_web_view = point;
  views::View::ConvertPointFromWidget(web_view_, &in_web_view);
  return IsDraggableAt(in_web_view);
}

bool SettingsWindow::IsDraggableAt(const gfx::Point& point) const {
  return draggable_region_ && web_view_ &&
         draggable_region_->contains(point.x(), point.y());
}

void SettingsWindow::OpenInBrowser(const GURL& url,
                                   WindowOpenDisposition disposition) {
  NavigateParams params(profile_.get(), url, ui::PAGE_TRANSITION_LINK);
  params.disposition = disposition == WindowOpenDisposition::NEW_BACKGROUND_TAB
                           ? disposition
                           : WindowOpenDisposition::NEW_FOREGROUND_TAB;
  params.window_action = NavigateParams::WindowAction::kShowWindow;
  Navigate(&params);
}

content::WebContents* SettingsWindow::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)>
        navigation_handle_callback) {
  // The page's own navigations (between panes, a reload) stay here; links
  // out of it open in a browser window.
  if (params.disposition == WindowOpenDisposition::CURRENT_TAB &&
      params.url.SchemeIs(content::kChromeUIScheme) &&
      params.url.host() == kSettingsHost) {
    content::NavigationController::LoadURLParams load(params);
    return source->GetController().LoadURLWithParams(load) ? source : nullptr;
  }
  OpenInBrowser(params.url, params.disposition);
  return nullptr;
}

content::WebContents* SettingsWindow::AddNewContents(
    content::WebContents* source,
    std::unique_ptr<content::WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& window_features,
    bool user_gesture,
    bool* was_blocked) {
  // A target="_blank" link: opened in a browser window instead.
  if (target_url.is_valid()) {
    OpenInBrowser(target_url, disposition);
  }
  return nullptr;
}

bool SettingsWindow::HandleKeyboardEvent(
    content::WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  return widget_ && unhandled_keyboard_event_handler_.HandleKeyboardEvent(
                        event, widget_->GetFocusManager());
}

void SettingsWindow::CloseContents(content::WebContents* source) {
  Close();
}

void SettingsWindow::DraggableRegionsChanged(
    const std::vector<blink::mojom::DraggableRegionPtr>& regions,
    content::WebContents* contents) {
  draggable_region_ = gfx::DraggableRegionsToSkRegion(regions);
}

void SettingsWindow::Forget() {
  auto it = Windows().find(profile_);
  if (it != Windows().end() && it->second == this) {
    Windows().erase(it);
  }
}

void SettingsWindow::OnWidgetDestroying(views::Widget* widget) {
  // Opening settings now makes a new window.
  Forget();
  widget_observation_.Reset();
  show_path_.Reset();
  if (web_view_) {
    web_view_->SetWebContents(nullptr);
    web_view_ = nullptr;
  }
  // The widget is still being torn down: go once it's done.
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
}

void SettingsWindow::OnProfileWillBeDestroyed(Profile* profile) {
  profile_observation_.Reset();
  Forget();
  if (web_view_) {
    web_view_->SetWebContents(nullptr);
    web_view_ = nullptr;
  }
  contents_.reset();
  Close();
}

}  // namespace compass
