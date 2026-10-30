// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Switching a window to another profile's space in place: the other
// profile's BrowserView takes the native window (BrowserViewExchange). See
// docs/design/profiles-in-place.md, "Tests".

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/extensions/api/tabs/tabs_api.h"
#include "chrome/browser/extensions/chrome_test_extension_loader.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/constrained_window/constrained_window_views.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "extensions/browser/api_test_utils.h"
#include "extensions/common/extension_builder.h"
#include "extensions/test/test_extension_dir.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/ui/views/command_bar/command_bar.h"
#include "strata/browser/ui/views/sidebar/sidebar_preloader.h"
#include "strata/browser/ui/views/sidebar/sidebar_swap.h"
#include "strata/browser/ui/views/sidebar/sidebar_tint.h"
#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"
#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"
#include "strata/browser/window_sync/space_directory.h"
#include "strata/browser/window_sync/space_service.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/base_window.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/models/dialog_model.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"
#include "url/gurl.h"

namespace strata {
namespace {

template <typename T>
T* FindDescendant(views::View* root) {
  if (auto* match = views::AsViewClass<T>(root)) {
    return match;
  }
  for (views::View* child : root->children()) {
    if (T* match = FindDescendant<T>(child)) {
      return match;
    }
  }
  return nullptr;
}

StrataSidebarView* FindSidebar(views::View* view) {
  return FindDescendant<StrataSidebarView>(view);
}

int CountNormalWindows() {
  int normal = 0;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* window) {
        normal += window->GetType() == BrowserWindowInterface::TYPE_NORMAL;
        return true;
      });
  return normal;
}

// Whether a widget of the window that switches away ever shows over the view
// that took the native window. A switch can finish, and close the old window
// with its widgets, before a test looks again, so this follows the widget's
// changes as they happen.
class LeavingWidgetWatch : public views::WidgetObserver {
 public:
  LeavingWidgetWatch(views::Widget* widget,
                     BrowserWindowInterface* old_window,
                     gfx::NativeWindow native)
      : old_window_(old_window->GetWeakPtr()), native_(native) {
    observation_.Observe(widget);
  }

  bool shown_over_new_view() const { return shown_over_new_view_; }
  bool destroyed() const { return destroyed_; }

  // views::WidgetObserver:
  void OnWidgetVisibilityChanged(views::Widget* widget, bool visible) override {
    Check(widget);
  }
  // Not checked: it goes with the old window, whose view is going too.
  void OnWidgetDestroying(views::Widget* widget) override {
    destroyed_ = true;
    observation_.Reset();
  }

  void Check(views::Widget* widget) {
    const bool exchanged =
        !old_window_ || old_window_->GetWindow()->GetNativeWindow() != native_;
    if (exchanged && widget->IsVisible() &&
        widget->parent() == views::Widget::GetWidgetForNativeWindow(native_)) {
      shown_over_new_view_ = true;
    }
  }

 private:
  base::WeakPtr<BrowserWindowInterface> old_window_;
  gfx::NativeWindow native_;
  bool shown_over_new_view_ = false;
  bool destroyed_ = false;
  base::ScopedObservation<views::Widget, views::WidgetObserver> observation_{
      this};
};

class StrataProfileSwitchBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    // The debug build takes longer than kMaxTreeWait to build a sidebar.
    SetMaxTreeWaitForTesting(base::Seconds(10));
    profile_ = browser()->GetProfile();
    home_ = &SpaceService::Get(profile_)->space_list().active_space();
    ProfileManager* profile_manager = g_browser_process->profile_manager();
    other_ = &profiles::testing::CreateProfileSync(
        profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
    SpaceDirectory::Get()->List(other_);
    other_home_ = &SpaceService::Get(other_)->space_list().active_space();
    native_ = browser()->GetWindow()->GetNativeWindow();
  }

  views::Widget* widget() {
    return views::Widget::GetWidgetForNativeWindow(native_);
  }

  // Switches the window showing |from| to |space| of |profile| and waits
  // until the new view holds the native window and, with |settle|, the old
  // one has closed. Returns the window shown.
  BrowserWindowInterface* SwitchTo(BrowserWindowInterface* from,
                                   Profile* profile,
                                   SpaceModel* space,
                                   bool settle = true) {
    const int normal = CountNormalWindows();
    BrowserWindowInterface* shown = SwitchWindowToSpace(from, profile, *space);
    EXPECT_TRUE(shown);
    if (!shown) {
      return nullptr;
    }
    EXPECT_TRUE(base::test::RunUntil(
        [&] { return shown->GetWindow()->GetNativeWindow() == native_; }));
    if (settle) {
      EXPECT_TRUE(
          base::test::RunUntil([&] { return CountNormalWindows() == normal; }));
    }
    SetBrowser(shown);
    return shown;
  }

  BrowserWindowInterface* SwitchToOther(BrowserWindowInterface* from,
                                        bool settle = true) {
    return SwitchTo(from, other_, other_home_, settle);
  }
  BrowserWindowInterface* SwitchBack(BrowserWindowInterface* from) {
    return SwitchTo(from, profile_, home_);
  }

  raw_ptr<Profile> profile_ = nullptr;
  raw_ptr<Profile> other_ = nullptr;
  raw_ptr<SpaceModel> home_ = nullptr;
  raw_ptr<SpaceModel> other_home_ = nullptr;
  gfx::NativeWindow native_;
};

// Cmd+T in the window after a switch opens the command bar of the new
// profile, and its tab is the new profile's.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       CommandBarAndNewTabUseTheNewProfile) {
  ASSERT_TRUE(embedded_test_server()->Start());
  BrowserWindowInterface* shown = SwitchToOther(browser());
  ASSERT_TRUE(shown);

  CommandBar* bar = CommandBar::From(shown);
  ASSERT_TRUE(bar);
  chrome::ExecuteCommand(shown, IDC_NEW_TAB);
  ASSERT_TRUE(base::test::RunUntil([&] { return bar->IsShowing(); }));
  content::WebContents* contents = bar->web_contents_for_testing();
  ASSERT_TRUE(contents);
  EXPECT_EQ(contents->GetBrowserContext(), other_.get());
  // In the shown window.
  views::Widget* bar_widget =
      views::Widget::GetTopLevelWidgetForNativeView(contents->GetNativeView());
  ASSERT_TRUE(bar_widget);
  EXPECT_EQ(bar_widget->parent(), widget());

  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(content::ExecJs(contents, content::JsReplace(R"(
    (async (text) => {
      const root = document.querySelector('strata-command-bar-app').shadowRoot;
      const input = root.querySelector('#input');
      input.value = text;
      input.dispatchEvent(new InputEvent('input', {inputType: 'insertText'}));
      const first = () => root.querySelector('.result .title')?.textContent;
      while (!first()?.includes(text)) {
        await new Promise(r => setTimeout(r, 20));
      }
      input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));
    })($1))",
                                                           url.spec())));
  TabStripModel* model = shown->GetTabStripModel();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return model->GetActiveWebContents()->GetVisibleURL() == url;
  }));
  EXPECT_EQ(model->GetActiveWebContents()->GetBrowserContext(), other_.get());
}

// A bubble open in the window when it switches goes out of sight with its
// view, and closes with it.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       BubbleOpenAtTheSwitchCloses) {
  CommandBar* old_bar = CommandBar::From(browser());
  ASSERT_TRUE(old_bar);
  chrome::ExecuteCommand(browser(), IDC_NEW_TAB);
  ASSERT_TRUE(base::test::RunUntil([&] { return old_bar->IsShowing(); }));
  views::Widget* old_bar_widget = views::Widget::GetTopLevelWidgetForNativeView(
      old_bar->web_contents_for_testing()->GetNativeView());
  ASSERT_TRUE(old_bar_widget);
  LeavingWidgetWatch watch(old_bar_widget, browser(), native_);

  BrowserWindowInterface* shown = SwitchToOther(browser(), /*settle=*/false);
  ASSERT_TRUE(shown);
  if (!watch.destroyed()) {
    watch.Check(old_bar_widget);
  }
  ASSERT_TRUE(base::test::RunUntil([&] { return CountNormalWindows() == 1; }));
  // Never over the new profile's view.
  EXPECT_FALSE(watch.shown_over_new_view());

  // The new window's bar works.
  CommandBar* bar = CommandBar::From(shown);
  chrome::ExecuteCommand(shown, IDC_NEW_TAB);
  ASSERT_TRUE(base::test::RunUntil([&] { return bar->IsShowing(); }));
  bar->Close();
}

// A page keeps its state across switches: its tab waits in the parking
// window, alive.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       FormValueSurvivesSwitches) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<input id=field>")));
  content::WebContents* page = browser()->GetTabStripModel()->GetWebContentsAt(0);
  ASSERT_TRUE(content::ExecJs(
      page, "document.getElementById('field').value = 'typed';"));

  BrowserWindowInterface* shown = browser();
  for (int i = 0; i < 2; ++i) {
    shown = SwitchToOther(shown);
    ASSERT_TRUE(shown);
    shown = SwitchBack(shown);
    ASSERT_TRUE(shown);
  }
  EXPECT_EQ(shown->GetTabStripModel()->GetActiveWebContents(), page);
  EXPECT_EQ(content::EvalJs(page, "document.getElementById('field').value"),
            "typed");
}

// A tab-modal dialog never shows over the other profile's view. Upstream
// closes it with the window it opened in (ModalDialogHostObserverViews), as
// when its tab is dragged out of a window that then closes: the old window
// closes after a switch to another profile.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       TabModalDialogLeavesWithItsWindow) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* page = browser()->GetTabStripModel()->GetWebContentsAt(0);
  views::Widget* dialog = constrained_window::ShowWebModal(
      ui::DialogModel::Builder()
          .SetTitle(u"Dialog")
          .AddOkButton(base::DoNothing())
          .Build(),
      page);
  ASSERT_TRUE(dialog);
  base::WeakPtr<views::Widget> alive = dialog->GetWeakPtr();
  ASSERT_TRUE(base::test::RunUntil([&] { return dialog->IsVisible(); }));
  EXPECT_EQ(dialog->parent(), widget());
  LeavingWidgetWatch watch(dialog, browser(), native_);

  BrowserWindowInterface* shown =
      SwitchToOther(browser(), /*settle=*/false);
  ASSERT_TRUE(shown);
  if (alive) {
    watch.Check(dialog);
  }
  ASSERT_TRUE(base::test::RunUntil([&] { return CountNormalWindows() == 1; }));
  // Out of sight at once, then closed with the old window.
  EXPECT_FALSE(watch.shown_over_new_view());
  EXPECT_FALSE(alive);

  shown = SwitchBack(shown);
  ASSERT_TRUE(shown);
  EXPECT_EQ(shown->GetTabStripModel()->GetActiveWebContents(), page);
  EXPECT_EQ(content::EvalJs(page, "document.title"), "One");
}

// The window stays fullscreen across switches, and the view shown knows it.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest, FullscreenIsKept) {
  ui_test_utils::ToggleFullscreenModeAndWait(browser());
  ASSERT_TRUE(browser()->GetWindow()->IsFullscreen());

  BrowserWindowInterface* shown = SwitchToOther(browser());
  ASSERT_TRUE(shown);
  EXPECT_TRUE(widget()->IsFullscreen());
  EXPECT_TRUE(shown->GetWindow()->IsFullscreen());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      shown, GURL("data:text/html,<title>Two</title>")));

  shown = SwitchBack(shown);
  ASSERT_TRUE(shown);
  EXPECT_TRUE(shown->GetWindow()->IsFullscreen());
  // And it leaves fullscreen as usual.
  ui_test_utils::ToggleFullscreenModeAndWait(shown);
  EXPECT_FALSE(shown->GetWindow()->IsFullscreen());
}

// Pages take their colors from the window they show in.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       PageColorsComeFromTheShownWindow) {
  BrowserWindowInterface* shown = SwitchToOther(browser());
  ASSERT_TRUE(shown);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      shown, GURL("data:text/html,<title>Two</title>")));
  EXPECT_EQ(&shown->GetTabStripModel()->GetActiveWebContents()
                 ->GetColorProvider(),
            widget()->GetColorProvider());
}

// To extensions the window is the new profile's: windows.getAll() in that
// profile lists it, and the old profile's no longer does.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       ExtensionsSeeTheNewProfilesWindow) {
  auto get_all = [](Profile* profile) {
    auto function = base::MakeRefCounted<extensions::WindowsGetAllFunction>();
    function->set_extension(
        extensions::ExtensionBuilder("Windows").Build().get());
    std::optional<base::Value> result =
        extensions::api_test_utils::RunFunctionAndReturnSingleResult(
            function, "[]", profile);
    std::vector<int> ids;
    if (result && result->is_list()) {
      for (const base::Value& window : result->GetList()) {
        ids.push_back(*window.GetDict().FindInt("id"));
      }
    }
    return ids;
  };
  EXPECT_EQ(get_all(profile_),
            std::vector<int>{extensions::ExtensionTabUtil::GetWindowId(
                browser())});

  BrowserWindowInterface* shown = SwitchToOther(browser());
  ASSERT_TRUE(shown);
  EXPECT_EQ(get_all(other_),
            std::vector<int>{extensions::ExtensionTabUtil::GetWindowId(
                shown)});
  EXPECT_TRUE(get_all(profile_).empty());
}

// An extension shortcut of the new profile works in the window: it moved to
// the native window's focus manager with the view, and leaves with it.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       ExtensionShortcutsMoveWithTheView) {
  extensions::TestExtensionDir dir;
  dir.WriteManifest(R"({
    "name": "Shortcut",
    "version": "1",
    "manifest_version": 3,
    "background": {"service_worker": "worker.js"},
    "commands": {
      "run": {
        "suggested_key": {"default": "Ctrl+Shift+Y", "mac": "Command+Shift+Y"},
        "description": "Run"
      }
    }
  })");
  dir.WriteFile(FILE_PATH_LITERAL("worker.js"), "");
  ASSERT_TRUE(extensions::ChromeTestExtensionLoader(other_).LoadExtension(
      dir.UnpackedPath()));
#if BUILDFLAG(IS_MAC)
  const ui::Accelerator shortcut(ui::VKEY_Y,
                                 ui::EF_COMMAND_DOWN | ui::EF_SHIFT_DOWN);
#else
  const ui::Accelerator shortcut(ui::VKEY_Y,
                                 ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN);
#endif
  EXPECT_FALSE(widget()->GetFocusManager()->IsAcceleratorRegistered(shortcut));

  BrowserWindowInterface* shown = SwitchToOther(browser());
  ASSERT_TRUE(shown);
  EXPECT_TRUE(widget()->GetFocusManager()->IsAcceleratorRegistered(shortcut));

  shown = SwitchBack(shown);
  ASSERT_TRUE(shown);
  EXPECT_FALSE(widget()->GetFocusManager()->IsAcceleratorRegistered(shortcut));
}

// The last used profile follows the window, for new windows from the Dock
// and the next launch.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       LastUsedProfileFollows) {
  BrowserWindowInterface* shown = SwitchToOther(browser());
  ASSERT_TRUE(shown);
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  EXPECT_EQ(profile_manager->GetLastUsedProfileDir(), other_->GetPath());
  shown = SwitchBack(shown);
  ASSERT_TRUE(shown);
  EXPECT_EQ(profile_manager->GetLastUsedProfileDir(), profile_->GetPath());
}

// Two windows on the first profile's Home; one switches to the other
// profile. Its tabs go to the other window on Home, not to parking, and that
// window keeps working.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       TabsGoToAnotherWindowOnTheSpace) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* one = browser()->GetTabStripModel()->GetWebContentsAt(0);
  BrowserWindowInterface* first = browser();
  BrowserWindowInterface* second = CreateBrowser(profile_);
  ASSERT_EQ(&WindowFeatures::From(second)->space_model(), home_.get());
  // The second one is now the active one; switch the first.
  BrowserWindowInterface* shown = SwitchToOther(first);
  ASSERT_TRUE(shown);
  EXPECT_EQ(shown->GetProfile(), other_.get());
  EXPECT_NE(second->GetTabStripModel()->GetIndexOfWebContents(one),
            TabStripModel::kNoTab);

  // The second window stays its profile's and shows its tabs.
  second->GetTabStripModel()->ActivateTabAt(
      second->GetTabStripModel()->GetIndexOfWebContents(one));
  EXPECT_EQ(second->GetTabStripModel()->GetActiveWebContents(), one);
  EXPECT_EQ(second->GetProfile(), profile_.get());
}


// A switch takes the other profile's preloaded sidebar, whose page connected
// without a window and gets one then, and the profile left gets one in turn.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       SwitchTakesThePreloadedSidebar) {
  SidebarPreloader* preloader = SidebarPreloader::Get();
  preloader->Refill();
  content::WebContents* preloaded = preloader->GetForTesting(other_);
  ASSERT_TRUE(preloaded);
  // Only a window of another profile could switch to this one's spaces.
  EXPECT_FALSE(preloader->GetForTesting(profile_));
  EXPECT_TRUE(content::WaitForLoadStop(preloaded));
  // No window, so no page handler yet.
  auto* ui = preloaded->GetWebUI()->GetController()->GetAs<StrataSidebarUI>();
  ASSERT_TRUE(ui);
  bool tree_shown = false;
  ui->RunWhenTreeShown(base::BindLambdaForTesting([&] { tree_shown = true; }));
  EXPECT_FALSE(tree_shown);

  BrowserWindowInterface* shown = SwitchToOther(browser());
  ASSERT_TRUE(shown);
  StrataSidebarView* sidebar = FindSidebar(
      BrowserView::GetBrowserViewForBrowser(shown));
  ASSERT_TRUE(sidebar);
  EXPECT_EQ(sidebar->GetWebContents(), preloaded);
  EXPECT_TRUE(base::test::RunUntil([&] { return tree_shown; }));

  preloader->Refill();
  EXPECT_FALSE(preloader->GetForTesting(other_));
  EXPECT_TRUE(preloader->GetForTesting(profile_));
}


// After every switch, the profile left gets a preloaded sidebar again, and
// the next switch back takes it.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       EverySwitchFindsAPreloadedSidebar) {
  SidebarPreloader* preloader = SidebarPreloader::Get();
  preloader->SetRefillDelayForTesting(base::Milliseconds(10));
  BrowserWindowInterface* shown = browser();
  for (int i = 0; i < 4; ++i) {
    const bool to_other = i % 2 == 0;
    Profile* target = to_other ? other_.get() : profile_.get();
    ASSERT_TRUE(
        base::test::RunUntil([&] { return preloader->GetForTesting(target); }))
        << "switch " << i;
    content::WebContents* preloaded = preloader->GetForTesting(target);
    shown = to_other ? SwitchToOther(shown) : SwitchBack(shown);
    ASSERT_TRUE(shown);
    StrataSidebarView* sidebar =
        FindSidebar(BrowserView::GetBrowserViewForBrowser(shown));
    ASSERT_TRUE(sidebar);
    EXPECT_EQ(sidebar->GetWebContents(), preloaded) << "switch " << i;
  }
}


// A switch asked for as the sidebar's slide starts builds the new window
// during the slide and exchanges once it has ended; the leaving window's
// tint, which the page can no longer move, finishes the blend itself.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       ExchangeWaitsForTheSlide) {
  other_home_->SetColor(SpaceColor::kGreen);
  SidebarTint* tint = FindDescendant<SidebarTint>(
      BrowserView::GetBrowserViewForBrowser(browser()));
  ASSERT_TRUE(tint);
  WindowFeatures::From(browser())->SetSpacePreview(other_home_, 0.2);

  constexpr base::TimeDelta kSlide = base::Seconds(3);
  const base::TimeTicks start = base::TimeTicks::Now();
  BrowserWindowInterface* shown =
      SwitchWindowToSpace(browser(), other_, *other_home_, kSlide);
  ASSERT_TRUE(shown);
  ASSERT_TRUE(base::test::RunUntil([&] {
    std::optional<SkColor> color = tint->GetTintForTesting();
    return color && SkColorSetA(*color, SK_AlphaOPAQUE) ==
                        *GetSpaceTint(SpaceColor::kGreen);
  }));
  EXPECT_NE(shown->GetWindow()->GetNativeWindow(), native_);

  ASSERT_TRUE(base::test::RunUntil(
      [&] { return shown->GetWindow()->GetNativeWindow() == native_; }));
  EXPECT_GE(base::TimeTicks::Now() - start, kSlide);
  SetBrowser(shown);
  ASSERT_TRUE(base::test::RunUntil([&] { return CountNormalWindows() == 1; }));
}

}  // namespace
}  // namespace strata
