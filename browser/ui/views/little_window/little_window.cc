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
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/uuid.h"
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
#include "chrome/common/webui_url_constants.h"
#include "components/vector_icons/vector_icons.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/views/frame/content_frame.h"
#include "compass/browser/ui/views/sidebar/sidebar_tint.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/menu/menu_runner.h"
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
#if BUILDFLAG(IS_MAC)
// Room for the traffic lights before the bar's controls.
constexpr int kTrafficLightsWidth = 78;
#endif

bool g_creating_little_window = false;

// Somewhat larger than Arc's Little Arc.
constexpr gfx::Size kWindowSize(900, 940);
// "Open in <space>", the shape of the sidebar header's URL pill.
constexpr int kPillHeight = 28;
constexpr int kPillHorizontalPadding = 12;

bool IsLittleWindowURL(const GURL& url) {
  return url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile();
}

// Centered on the display of |near| (the window the link came from, or the
// one Cmd+Option+N was typed in), else the pointer's.
gfx::Rect GetNewWindowBounds(const std::optional<gfx::Rect>& near) {
  display::Screen* screen = display::Screen::Get();
  const display::Display display =
      near ? screen->GetDisplayMatching(*near)
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

// A space's name, or "Space <n>" for one without a name, as the switcher.
std::u16string GetSpaceName(const SpaceModel& space, size_t index) {
  return space.name().empty()
             ? u"Space " + base::NumberToString16(index + 1)
             : space.name();
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
    UpdateBackground();
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
  void StateChanged(ButtonState old_state) override {
    views::LabelButton::StateChanged(old_state);
    UpdateBackground();
  }
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
  // The location bar's colors: lighter while hovered or pressed.
  void UpdateBackground() {
    const bool hovered =
        GetState() == STATE_HOVERED || GetState() == STATE_PRESSED;
    SetBackground(views::CreateRoundedRectBackground(
        hovered ? kColorLocationBarBackgroundHovered
                : kColorLocationBarBackground,
        kPillHeight / 2.f));
  }

  const raw_ptr<BrowserWindowInterface> browser_;
  views::ViewTracker frame_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
  ui::ScopedUnownedUserData<LittleWindowButton> scoped_data_holder_;
};

DEFINE_USER_DATA(LittleWindowButton);

BEGIN_METADATA(LittleWindowButton)
END_METADATA

// Beside "Open in": a menu of the profile's spaces, to open the page in
// another one than the last active window's. Hidden while there's only one.
class LittleSpaceMenuButton : public views::ImageButton,
                              public ui::SimpleMenuModel::Delegate,
                              public views::WidgetObserver {
  METADATA_HEADER(LittleSpaceMenuButton, views::ImageButton)

 public:
  explicit LittleSpaceMenuButton(BrowserWindowInterface* browser)
      : views::ImageButton(
            base::BindRepeating(&LittleSpaceMenuButton::ShowMenu,
                                base::Unretained(this))),
        browser_(browser) {
    views::ConfigureVectorImageButton(this);
    views::SetImageFromVectorIconWithColor(
        this, vector_icons::kKeyboardArrowDownIcon, kMenuIconSize,
        views::IconColors(kColorOmniboxText, kColorOmniboxText));
    SetBackground(views::CreateRoundedRectBackground(
        kColorLocationBarBackground, kPillHeight / 2.f));
    SetPreferredSize(gfx::Size(kPillHeight, kPillHeight));
    SetTooltipText(u"Open in Another Space");
    GetViewAccessibility().SetName(u"Open in Another Space");
    Update();
  }
  LittleSpaceMenuButton(const LittleSpaceMenuButton&) = delete;
  LittleSpaceMenuButton& operator=(const LittleSpaceMenuButton&) = delete;
  ~LittleSpaceMenuButton() override = default;

  void Update() {
    SpaceService* service = SpaceService::Get(browser_->GetProfile());
    SetVisible(service && service->space_list().spaces().size() > 1);
  }

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdChecked(int command_id) const override {
    const SpaceModel* target = GetTargetSpace(browser_->GetProfile());
    return target && command_id >= 0 &&
           static_cast<size_t>(command_id) < space_ids_.size() &&
           target->id() == space_ids_[command_id];
  }
  void ExecuteCommand(int command_id, int event_flags) override {
    if (command_id < 0 || static_cast<size_t>(command_id) >= space_ids_.size()) {
      return;
    }
    // After the menu: the window closes.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&LittleSpaceMenuButton::OpenIn,
                                  weak_factory_.GetWeakPtr(),
                                  space_ids_[command_id]));
  }

  // views::View:
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
  static constexpr int kMenuIconSize = 16;

  void ShowMenu() {
    SpaceService* service = SpaceService::Get(browser_->GetProfile());
    if (!service) {
      return;
    }
    model_ = std::make_unique<ui::SimpleMenuModel>(this);
    space_ids_.clear();
    const auto& spaces = service->space_list().spaces();
    for (size_t i = 0; i < spaces.size(); ++i) {
      space_ids_.push_back(spaces[i]->id());
      model_->AddCheckItem(static_cast<int>(i),
                           GetSpaceName(*spaces[i], i));
    }
    runner_ = std::make_unique<views::MenuRunner>(
        model_.get(), views::MenuRunner::HAS_MNEMONICS);
    runner_->RunMenuAt(GetWidget(), /*button_controller=*/nullptr,
                       GetBoundsInScreen(),
                       views::MenuAnchorPosition::kTopRight,
                       ui::mojom::MenuSourceType::kMouse);
  }

  void OpenIn(const base::Uuid& space_id) {
    SpaceService* service = SpaceService::Get(browser_->GetProfile());
    SpaceModel* space =
        service ? service->space_list().FindSpace(space_id) : nullptr;
    if (space) {
      OpenLittleWindowInSpace(browser_, space);
    }
  }

  const raw_ptr<BrowserWindowInterface> browser_;
  std::unique_ptr<ui::SimpleMenuModel> model_;
  std::unique_ptr<views::MenuRunner> runner_;
  // The menu's spaces, by command id.
  std::vector<base::Uuid> space_ids_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
  base::WeakPtrFactory<LittleSpaceMenuButton> weak_factory_{this};
};

BEGIN_METADATA(LittleSpaceMenuButton)
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
  open->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 6, 0, 4));
  toolbar->AddChildView(std::move(open));
  auto spaces = std::make_unique<LittleSpaceMenuButton>(browser);
  spaces->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 0, 0, 8));
  toolbar->AddChildView(std::move(spaces));

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

namespace {

// A little window on |url|, centered on |near|'s display (else the
// pointer's). Over |opener|, another app's window, if there's one.
BrowserWindowInterface* CreateLittleWindow(
    Profile* profile,
    const GURL& url,
    const std::optional<gfx::Rect>& near,
    const std::optional<OpenerWindow>& opener) {
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_POPUP, profile,
                                   /*from_user_gesture=*/true);
  params.initial_bounds = GetNewWindowBounds(near);
  // A little window is for now: it isn't restored, and opening one isn't the
  // start of a session.
  params.omit_from_session_restore = true;
  params.should_trigger_session_restore = false;
  BrowserWindowInterface* browser;
  {
    base::AutoReset<bool> creating(&g_creating_little_window, true);
    base::AutoReset<bool> outside_sidebar = CreatingWindowOutsideSidebar();
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

}  // namespace

BrowserWindowInterface* OpenLittleWindow(Profile* profile, const GURL& url) {
  // Before Compass's own windows can come over it.
  const std::optional<OpenerWindow> opener = FindOpenerWindow();
  return CreateLittleWindow(
      profile, url,
      opener ? std::optional<gfx::Rect>(opener->bounds) : std::nullopt,
      opener);
}

BrowserWindowInterface* OpenNewLittleWindow(BrowserWindowInterface* from) {
  BrowserView* from_view = BrowserView::GetBrowserViewForBrowser(from);
  std::optional<gfx::Rect> near;
  if (from_view && from_view->GetWidget()) {
    near = from_view->GetWidget()->GetWindowBoundsInScreen();
  }
  BrowserWindowInterface* browser =
      CreateLittleWindow(from->GetProfile(),
                         GURL(chrome::kChromeUINewTabPageURL), near,
                         /*opener=*/std::nullopt);
  if (browser) {
    browser->GetWindow()->Activate();
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

BrowserWindowInterface* OpenLittleWindowInSpace(BrowserWindowInterface* browser,
                                                SpaceModel* space) {
  if (!IsLittleWindow(browser)) {
    return nullptr;
  }
  TabStripModel* from = browser->GetTabStripModel();
  if (from->empty()) {
    return nullptr;
  }
  Profile* profile = browser->GetProfile();
  BrowserWindowInterface* target =
      space ? FindLastActiveWindowShowing(profile, *space) : nullptr;
  if (!target) {
    target = FindLastActiveWindow(profile);
  }
  if (!target) {
    target = CreateBrowserWindow(
        BrowserWindowCreateParams(profile, /*from_user_gesture=*/true));
    if (!target) {
      return nullptr;
    }
  }
  // Not showing |space|: the window switches to it.
  WindowFeatures* features = WindowFeatures::From(target);
  if (space && features && &features->space_model() != space) {
    target = SwitchWindowToSpace(target, profile, *space);
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
  if (id == kNewLittleWindowCommand) {
    OpenNewLittleWindow(browser);
    return true;
  }
  if (id != IDC_OPEN_FILE || !IsLittleWindow(browser)) {
    return false;
  }
  OpenLittleWindowInSpace(browser);
  return true;
}

}  // namespace compass
