// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/little_window/little_window.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/command_observer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/startup/startup_tab.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/toolbar/back_forward_button.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/window_features.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/ui_base_types.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"
#include "url/gurl.h"

namespace compass {

namespace {

// Arc's Little Arc is about this size.
constexpr gfx::Size kWindowSize(760, 820);
// Each little window already open moves the next one by this much.
constexpr int kCascadeOffset = 24;

int g_little_window_count = 0;

bool IsLittleWindowURL(const GURL& url) {
  return url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile();
}

gfx::Rect GetNewWindowBounds() {
  display::Screen* screen = display::Screen::Get();
  const gfx::Rect work_area =
      screen->GetDisplayNearestPoint(screen->GetCursorScreenPoint())
          .work_area();
  gfx::Size size = kWindowSize;
  size.SetToMin(gfx::Size(work_area.width() * 9 / 10,
                          work_area.height() * 9 / 10));
  gfx::Rect bounds(size);
  bounds.set_origin(work_area.CenterPoint() -
                    gfx::Vector2d(size.width() / 2, size.height() / 2));
  const int offset = (g_little_window_count % 5) * kCascadeOffset;
  bounds.Offset(offset, offset);
  bounds.AdjustToFit(work_area);
  return bounds;
}

// Back, enabled when the page can go back. No forward, as in Arc.
class LittleBackButton : public BackForwardButton, public CommandObserver {
  METADATA_HEADER(LittleBackButton, BackForwardButton)

 public:
  explicit LittleBackButton(BrowserWindowInterface* browser)
      : BackForwardButton(
            BackForwardButton::Direction::kBack,
            base::BindRepeating(
                [](BrowserWindowInterface* browser, const ui::Event& event) {
                  chrome::ExecuteCommand(browser, IDC_BACK);
                },
                browser),
            browser),
        browser_(browser) {
    chrome::AddCommandObserver(browser_, IDC_BACK, this);
    SetEnabled(chrome::IsCommandEnabled(browser_, IDC_BACK));
  }
  LittleBackButton(const LittleBackButton&) = delete;
  LittleBackButton& operator=(const LittleBackButton&) = delete;
  ~LittleBackButton() override {
    chrome::RemoveCommandObserver(browser_, IDC_BACK, this);
  }

  // CommandObserver:
  void EnabledStateChangedForCommand(int id, bool enabled) override {
    SetEnabled(enabled);
  }

 private:
  const raw_ptr<BrowserWindowInterface> browser_;
};

BEGIN_METADATA(LittleBackButton)
END_METADATA

// "Open in <space>", at the end of the toolbar. Marks the window as a little
// one (LittleWindowButton::From()) for as long as the toolbar lives.
class LittleWindowButton : public views::MdTextButton,
                           public views::WidgetObserver {
  METADATA_HEADER(LittleWindowButton, views::MdTextButton)

 public:
  DECLARE_USER_DATA(LittleWindowButton);

  explicit LittleWindowButton(BrowserWindowInterface* browser)
      : views::MdTextButton(
            base::BindRepeating(
                [](BrowserWindowInterface* browser, const ui::Event& event) {
                  OpenLittleWindowInSpace(browser);
                },
                browser)),
        browser_(browser),
        scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {
    SetStyle(ui::ButtonStyle::kTonal);
    ++g_little_window_count;
    UpdateText();
  }
  LittleWindowButton(const LittleWindowButton&) = delete;
  LittleWindowButton& operator=(const LittleWindowButton&) = delete;
  ~LittleWindowButton() override { --g_little_window_count; }

  static LittleWindowButton* From(BrowserWindowInterface* browser) {
    return Get(browser->GetUnownedUserDataHost());
  }

  // The space the page would go to: the last active window's, which can
  // change while this window is open.
  void UpdateText() {
    BrowserWindowInterface* target =
        FindLastActiveWindow(browser_->GetProfile());
    WindowFeatures* features = target ? WindowFeatures::From(target) : nullptr;
    const std::u16string name =
        features ? features->space_model().name() : std::u16string();
    SetText(name.empty() ? u"Open in Compass" : u"Open in " + name);
    SetTooltipText(
#if BUILDFLAG(IS_MAC)
        u"Move this page to a tab (⌘O)"
#else
        u"Move this page to a tab (Ctrl+O)"
#endif
    );
  }

  // views::View:
  void AddedToWidget() override {
    widget_observation_.Observe(GetWidget());
  }
  void RemovedFromWidget() override { widget_observation_.Reset(); }

  // views::WidgetObserver:
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override {
    if (active) {
      UpdateText();
    }
  }

 private:
  const raw_ptr<BrowserWindowInterface> browser_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
  ui::ScopedUnownedUserData<LittleWindowButton> scoped_data_holder_;
};

DEFINE_USER_DATA(LittleWindowButton);

BEGIN_METADATA(LittleWindowButton)
END_METADATA

// Adds the back button before the location bar and "Open in <space>" after
// everything else in the popup's toolbar (a horizontal FlexLayout in
// location mode).
void AddControls(BrowserWindowInterface* browser) {
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  ToolbarView* toolbar = browser_view ? browser_view->toolbar() : nullptr;
  if (!toolbar) {
    return;
  }
  toolbar->AddChildViewAt(std::make_unique<LittleBackButton>(browser), 0);
  auto open = std::make_unique<LittleWindowButton>(browser);
  open->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 4, 0, 6));
  toolbar->AddChildView(std::move(open));
  toolbar->InvalidateLayout();
}

}  // namespace

bool OpenInLittleWindows(const std::vector<StartupTab>& tabs,
                         Profile* profile) {
  if (tabs.empty() || !profile) {
    return false;
  }
  for (const StartupTab& tab : tabs) {
    if (!IsLittleWindowURL(tab.url)) {
      return false;
    }
  }
  for (const StartupTab& tab : tabs) {
    OpenLittleWindow(profile, tab.url);
  }
  return true;
}

BrowserWindowInterface* OpenLittleWindow(Profile* profile, const GURL& url) {
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_POPUP, profile,
                                   /*from_user_gesture=*/true);
  params.initial_bounds = GetNewWindowBounds();
  // A little window is for now: it isn't restored, and opening one isn't the
  // start of a session.
  params.omit_from_session_restore = true;
  params.should_trigger_session_restore = false;
  BrowserWindowInterface* browser = CreateBrowserWindow(std::move(params));
  if (!browser) {
    return nullptr;
  }
  AddControls(browser);

  NavigateParams navigate(browser, url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  navigate.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  navigate.window_action = NavigateParams::WindowAction::kShowWindow;
  Navigate(&navigate);
  return browser;
}

bool IsLittleWindow(BrowserWindowInterface* browser) {
  return browser && LittleWindowButton::From(browser);
}

BrowserWindowInterface* OpenLittleWindowInSpace(
    BrowserWindowInterface* browser) {
  if (!IsLittleWindow(browser)) {
    return nullptr;
  }
  TabStripModel* from = browser->GetTabStripModel();
  if (from->empty()) {
    return nullptr;
  }
  Profile* profile = browser->GetProfile();
  BrowserWindowInterface* target = FindLastActiveWindow(profile);
  if (!target) {
    target = CreateBrowserWindow(
        BrowserWindowCreateParams(profile, /*from_user_gesture=*/true));
    if (!target) {
      return nullptr;
    }
  }
  // The strip's bridge makes it a Today entry, as any new tab. The little
  // window, left without tabs, closes.
  std::unique_ptr<tabs::TabModel> tab =
      from->DetachTabAtForInsertion(from->active_index());
  target->GetTabStripModel()->AppendTab(std::move(tab), /*foreground=*/true);
  target->GetWindow()->Show();
  target->GetWindow()->Activate();
  return target;
}

bool HandleLittleWindowCommand(BrowserWindowInterface* browser, int id) {
  if (id != IDC_OPEN_FILE || !IsLittleWindow(browser)) {
    return false;
  }
  OpenLittleWindowInSpace(browser);
  return true;
}

}  // namespace compass
