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
#include "chrome/browser/favicon/favicon_service_factory.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "components/favicon/core/favicon_service.h"
#include "components/history/core/browser/history_service.h"
#include "components/keyed_service/core/service_access_type.h"
#include "base/strings/escape.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/ui/views/command_bar/command_bar.h"
#include "strata/browser/ui/views/sidebar/sidebar_swap.h"
#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"
#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"
#include "strata/browser/window_sync/space_directory.h"
#include "strata/browser/window_sync/space_service.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/image/image.h"
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


// The window keeps its sidebar page through switches to another profile's
// space and back: the new window's handler takes it, and it shows that
// window's space.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       SwitchesKeepTheSidebarPage) {
  content::WebContents* page =
      FindSidebar(BrowserView::GetBrowserViewForBrowser(browser()))
          ->GetWebContents();
  ASSERT_TRUE(page);
  other_home_->SetName(u"Elsewhere");
  home_->SetName(u"Here");
  BrowserWindowInterface* shown = browser();
  for (int i = 0; i < 4; ++i) {
    const bool to_other = i % 2 == 0;
    shown = to_other ? SwitchToOther(shown) : SwitchBack(shown);
    ASSERT_TRUE(shown);
    EXPECT_EQ(FindSidebar(BrowserView::GetBrowserViewForBrowser(shown))
                  ->GetWebContents(),
              page)
        << "switch " << i;
    EXPECT_EQ(webui::GetBrowserWindowInterface(page), shown);
    EXPECT_EQ(page->GetBrowserContext(), profile_.get());
    EXPECT_EQ(true, content::EvalJs(page, content::JsReplace(R"(
      (async (name) => {
        const shown = () => document.querySelector('strata-sidebar-app')
            ?.shadowRoot?.querySelector('#spaceName')?.textContent?.trim();
        while (shown() !== name) {
          await new Promise(r => setTimeout(r, 20));
        }
        return true;
      })($1))",
                                                       to_other ? "Elsewhere"
                                                                : "Here")))
        << "switch " << i;
  }
}

// The page renders while it moves: it goes hidden as it leaves the old
// window, and without a frame for the new one the window would keep its last
// frame until kMaxSwapWait.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       TheMovingPageKeepsRendering) {
  content::WebContents* page =
      FindSidebar(BrowserView::GetBrowserViewForBrowser(browser()))
          ->GetWebContents();
  ASSERT_TRUE(page);
  ASSERT_FALSE(page->IsBeingCaptured());
  BrowserWindowInterface* shown =
      SwitchWindowToSpace(browser(), other_, *other_home_);
  ASSERT_TRUE(shown);
  EXPECT_EQ(FindSidebar(BrowserView::GetBrowserViewForBrowser(shown))
                ->GetWebContents(),
            page);
  EXPECT_TRUE(page->IsBeingCaptured());
  // Until the switch ends.
  EXPECT_TRUE(base::test::RunUntil([&] { return !page->IsBeingCaptured(); }));
  SetBrowser(shown);
}

// The sidebar shows another profile's icons: its spaces' previews, and the
// window's own entries after a switch, which keeps the page.
IN_PROC_BROWSER_TEST_F(StrataProfileSwitchBrowserTest,
                       IconsComeFromTheirProfile) {
  const GURL page_url("https://icons.test/");
  HistoryServiceFactory::GetForProfile(other_,
                                       ServiceAccessType::EXPLICIT_ACCESS)
      ->AddPage(page_url, base::Time::Now(), history::SOURCE_BROWSED);
  SkBitmap red;
  red.allocN32Pixels(16, 16);
  red.eraseColor(SK_ColorRED);
  FaviconServiceFactory::GetForProfile(other_,
                                       ServiceAccessType::EXPLICIT_ACCESS)
      ->SetFavicons({page_url}, GURL("https://icons.test/favicon.ico"),
                    favicon_base::IconType::kFavicon,
                    gfx::Image::CreateFrom1xBitmap(red));

  content::WebContents* page =
      FindSidebar(BrowserView::GetBrowserViewForBrowser(browser()))
          ->GetWebContents();
  // The middle pixel of the icon at |url|.
  constexpr char kPixel[] = R"(
    (async (url) => {
      const image = new Image();
      image.src = url;
      await image.decode();
      const canvas = new OffscreenCanvas(16, 16);
      const context = canvas.getContext('2d');
      context.drawImage(image, 0, 0, 16, 16);
      return [...context.getImageData(8, 8, 1, 1).data.slice(0, 3)];
    })($1))";
  const std::string query = "size=16&scaleFactor=1x&pageUrl=" +
                            base::EscapeQueryParamValue(page_url.spec(), true);
  const std::string other_icon =
      "chrome://strata-favicon/?profile=" +
      other_->GetPath().BaseName().AsUTF8Unsafe() + "&" + query;
  EXPECT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page, content::JsReplace(kPixel, other_icon)) ==
           base::Value(base::ListValue().Append(255).Append(0).Append(0));
  }));
  // Not in the page's own profile.
  EXPECT_NE(content::EvalJs(page, content::JsReplace(
                                      kPixel, "chrome://strata-favicon/?" +
                                                  query)),
            base::Value(base::ListValue().Append(255).Append(0).Append(0)));
}

}  // namespace
}  // namespace strata
