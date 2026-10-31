// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/little_window/little_window.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "base/scoped_observation.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/command_observer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/startup/startup_tab.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/themed_background.h"
#include "chrome/browser/ui/views/frame/top_container_view.h"
#include "chrome/browser/ui/views/toolbar/back_forward_button.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/views/frame/content_frame.h"
#include "compass/browser/ui/views/sidebar/sidebar_tint.h"
#include "compass/browser/window_sync/window_features.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_observer.h"
#include "ui/views/view_tracker.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_MAC)
#include "compass/browser/ui/views/little_window/little_window_mac.h"
#endif

namespace compass {

#if !BUILDFLAG(IS_MAC)
struct OpenerWindow {
  gfx::Rect bounds;
  int number = 0;
};
std::optional<OpenerWindow> FindOpenerWindow() {
  return std::nullopt;
}
void KeepOtherWindowsBehind(views::Widget* little,
                            const OpenerWindow& opener) {}
#endif

namespace {

// The bar: the traffic lights, back, the address and "Open in", centered on
// one row at the top of the frame.
constexpr int kBarHeight = 40;
// Room for the traffic lights before the bar's controls.
constexpr int kTrafficLightsWidth = 78;

bool g_creating_little_window = false;

// Somewhat larger than Arc's Little Arc.
constexpr gfx::Size kWindowSize(900, 940);
// "Open in <space>", the shape of the sidebar header's URL pill.
constexpr int kPillHeight = 28;
constexpr int kPillHorizontalPadding = 12;

bool IsLittleWindowURL(const GURL& url) {
  return url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile();
}

// Centered on the display the link came from: the opener's window's, or
// the pointer's.
gfx::Rect GetNewWindowBounds(const std::optional<OpenerWindow>& opener) {
  display::Screen* screen = display::Screen::Get();
  const display::Display display =
      opener ? screen->GetDisplayMatching(opener->bounds)
             : screen->GetDisplayNearestPoint(screen->GetCursorScreenPoint());
  const gfx::Rect work_area = display.work_area();
  gfx::Size size = kWindowSize;
  size.SetToMin(gfx::Size(work_area.width() * 9 / 10,
                          work_area.height() * 9 / 10));
  gfx::Rect bounds(size);
  bounds.set_origin(work_area.CenterPoint() -
                    gfx::Vector2d(size.width() / 2, size.height() / 2));
  return bounds;
}

// The space "Open in" goes to: the last active window's.
const SpaceModel* GetTargetSpace(Profile* profile) {
  BrowserWindowInterface* target = FindLastActiveWindow(profile);
  WindowFeatures* features = target ? WindowFeatures::From(target) : nullptr;
  return features ? &features->space_model() : nullptr;
}

// The frame around the page, in the target space's color, as the main
// window's (SidebarTint): the browser view's first child, which covers the
// whole window (no native title bar), with the window's theme seeded from
// the same color.
class LittleWindowFrame : public views::View, public views::ViewObserver {
  METADATA_HEADER(LittleWindowFrame, views::View)

 public:
  explicit LittleWindowFrame(BrowserView* browser_view)
      : browser_view_(browser_view) {
    SetProperty(views::kViewIgnoredByLayoutKey, true);
    SetCanProcessEventsWithinSubtree(false);
    GetViewAccessibility().SetIsIgnored(true);
    observation_.Observe(browser_view);
  }
  LittleWindowFrame(const LittleWindowFrame&) = delete;
  LittleWindowFrame& operator=(const LittleWindowFrame&) = delete;
  ~LittleWindowFrame() override = default;

  void SetSpaceColor(std::optional<SpaceColor> color) {
    const SkColor tint =
        color ? GetSpaceFrameTint(*color) : SK_ColorTRANSPARENT;
    if (tint != tint_) {
      tint_ = tint;
      SchedulePaint();
    }
    views::Widget* widget = GetWidget();
    if (!widget) {
      return;
    }
    // Off the record keeps its grey; an extension theme paints its own.
    Profile* profile = browser_view_->browser()->GetProfile();
    const ThemeService* theme_service =
        ThemeServiceFactory::GetForProfile(profile);
    const bool keep_theme =
        profile->IsOffTheRecord() ||
        (theme_service && theme_service->UsingExtensionTheme());
    widget->SetUserColorOverride(keep_theme || !color ? std::nullopt
                                                      : GetSpaceTint(*color));
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    ThemedBackground::PaintBackground(canvas, this, browser_view_,
                                      ThemedBackground::kFrameTheme);
    if (SkColorGetA(tint_)) {
      canvas->FillRect(GetLocalBounds(), tint_);
    }
  }

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override {
    SetBoundsRect(observed_view->GetLocalBounds());
  }

 private:
  const raw_ptr<BrowserView> browser_view_;
  SkColor tint_ = SK_ColorTRANSPARENT;
  base::ScopedObservation<views::View, views::ViewObserver> observation_{this};
};

BEGIN_METADATA(LittleWindowFrame)
END_METADATA

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

// "Open in <space>", a pill at the end of the toolbar. Marks the window as a
// little one (LittleWindowButton::From()) for as long as the toolbar lives,
// and keeps the frame on the target space's color.
class LittleWindowButton : public views::LabelButton,
                           public views::WidgetObserver {
  METADATA_HEADER(LittleWindowButton, views::LabelButton)

 public:
  DECLARE_USER_DATA(LittleWindowButton);

  LittleWindowButton(BrowserWindowInterface* browser, LittleWindowFrame* frame)
      : views::LabelButton(base::BindRepeating(
            [](BrowserWindowInterface* browser, const ui::Event& event) {
              OpenLittleWindowInSpace(browser);
            },
            browser)),
        browser_(browser),
        frame_(frame),
        scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {
    SetHorizontalAlignment(gfx::ALIGN_CENTER);
    SetBackground(views::CreateRoundedRectBackground(
        kColorLocationBarBackground, kPillHeight / 2.f));
    SetEnabledTextColors(kColorOmniboxText);
    SetBorder(views::CreateEmptyBorder(
        gfx::Insets::VH(0, kPillHorizontalPadding)));
    SetMinSize(gfx::Size(0, kPillHeight));
    SetTooltipText(
#if BUILDFLAG(IS_MAC)
        u"Move this page to a tab (⌘O)"
#else
        u"Move this page to a tab (Ctrl+O)"
#endif
    );
    Update();
  }
  LittleWindowButton(const LittleWindowButton&) = delete;
  LittleWindowButton& operator=(const LittleWindowButton&) = delete;
  ~LittleWindowButton() override = default;

  static LittleWindowButton* From(BrowserWindowInterface* browser) {
    return Get(browser->GetUnownedUserDataHost());
  }

  // The target space can change while the window is open.
  void Update() {
    const SpaceModel* space = GetTargetSpace(browser_->GetProfile());
    const std::u16string name = space ? space->name() : std::u16string();
    SetText(name.empty() ? u"Open in Compass" : u"Open in " + name);
    if (auto* frame = static_cast<LittleWindowFrame*>(frame_.view())) {
      frame->SetSpaceColor(space ? std::optional(space->color())
                                 : std::nullopt);
    }
  }

  // views::LabelButton:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    gfx::Size size = views::LabelButton::CalculatePreferredSize(available_size);
    size.set_height(kPillHeight);
    return size;
  }
  void AddedToWidget() override {
    widget_observation_.Observe(GetWidget());
    Update();
  }
  void RemovedFromWidget() override { widget_observation_.Reset(); }

  // views::WidgetObserver:
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override {
    if (active) {
      Update();
    }
  }

 private:
  const raw_ptr<BrowserWindowInterface> browser_;
  views::ViewTracker frame_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
  ui::ScopedUnownedUserData<LittleWindowButton> scoped_data_holder_;
};

DEFINE_USER_DATA(LittleWindowButton);

BEGIN_METADATA(LittleWindowButton)
END_METADATA

// Gives the popup the frame: its color behind everything, the toolbar on it
// with the back button first and "Open in <space>" last (a horizontal
// FlexLayout in location mode), and the page as a rounded card
// (GetPopupContentFrameInsets()).
void Decorate(BrowserWindowInterface* browser) {
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  ToolbarView* toolbar = browser_view ? browser_view->toolbar() : nullptr;
  if (!toolbar) {
    return;
  }
  auto* frame = browser_view->AddChildViewAt(
      std::make_unique<LittleWindowFrame>(browser_view), 0);
  frame->SetBoundsRect(browser_view->GetLocalBounds());

  toolbar->SetBackground(nullptr);
  toolbar->SetPreferredSize(gfx::Size(0, kBarHeight));
  toolbar->AddChildViewAt(std::make_unique<LittleBackButton>(browser), 0);
#if BUILDFLAG(IS_MAC)
  // The traffic lights sit on the bar's row, at its start.
  auto traffic_lights = std::make_unique<views::View>();
  traffic_lights->SetPreferredSize(gfx::Size(kTrafficLightsWidth, 1));
  traffic_lights->SetCanProcessEventsWithinSubtree(false);
  toolbar->AddChildViewAt(std::move(traffic_lights), 0);
#endif
  auto open = std::make_unique<LittleWindowButton>(browser, frame);
  open->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 6, 0, 8));
  toolbar->AddChildView(std::move(open));

  browser_view->multi_contents_view()->SetBackgroundRadii(
      gfx::RoundedCornersF(kContentCornerRadius));
  browser_view->InvalidateLayout();
}

}  // namespace

gfx::Insets GetPopupContentFrameInsets(const views::View* view) {
  // Only read from.
  auto* browser_view =
      views::AsViewClass<BrowserView>(const_cast<views::View*>(view));
  if (!browser_view || !IsLittleWindow(browser_view->browser())) {
    return gfx::Insets();
  }
  return gfx::Insets::TLBR(0, kContentFrameInset, kContentFrameInset,
                           kContentFrameInset);
}

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
  // Before Compass's own windows can come over it.
  const std::optional<OpenerWindow> opener = FindOpenerWindow();

  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_POPUP, profile,
                                   /*from_user_gesture=*/true);
  params.initial_bounds = GetNewWindowBounds(opener);
  // A little window is for now: it isn't restored, and opening one isn't the
  // start of a session.
  params.omit_from_session_restore = true;
  params.should_trigger_session_restore = false;
  BrowserWindowInterface* browser;
  {
    base::AutoReset<bool> creating(&g_creating_little_window, true);
    browser = CreateBrowserWindow(std::move(params));
  }
  if (!browser) {
    return nullptr;
  }
  Decorate(browser);

  NavigateParams navigate(browser, url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  navigate.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  navigate.window_action = NavigateParams::WindowAction::kShowWindow;
  Navigate(&navigate);

  // Over the app the link came from, which stays over Compass's other
  // windows.
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (opener && browser_view && browser_view->GetWidget()) {
    KeepOtherWindowsBehind(browser_view->GetWidget(), *opener);
  }
  return browser;
}

bool IsLittleWindow(BrowserWindowInterface* browser) {
  return browser && LittleWindowButton::From(browser);
}

bool IsCreatingLittleWindow() {
  return g_creating_little_window;
}

std::optional<int> GetLittleWindowBarHeight(views::View* view) {
  auto* browser_view = views::AsViewClass<BrowserView>(view);
  if (g_creating_little_window ||
      (browser_view && IsLittleWindow(browser_view->browser()))) {
    return kBarHeight;
  }
  return std::nullopt;
}

bool IsOnLittleWindowCaption(views::View* view, const gfx::Point& point) {
  auto* browser_view = views::AsViewClass<BrowserView>(view);
  if (!browser_view || !IsLittleWindow(browser_view->browser()) ||
      !browser_view->GetLocalBounds().Contains(point)) {
    return false;
  }
  // The frame (only the browser view itself is there: the tint takes no
  // events) and the bar's empty space.
  const views::View* target = browser_view->GetEventHandlerForPoint(point);
  return target == browser_view ||
         target == static_cast<views::View*>(browser_view->toolbar()) ||
         target == static_cast<views::View*>(browser_view->top_container());
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
