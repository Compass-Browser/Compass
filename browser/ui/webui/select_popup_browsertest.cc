// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The <select> menus of Compass's WebUI (the Claude panel's model menu, the
// space menu's in the sidebar) open readable in either mode, read off the
// open menu's pixels. On Windows and Linux blink draws the menu itself: on
// the <select>'s background (white when that is transparent), in its color
// scheme, with its text color. In dark mode the menus were white with the
// panel's light gray text on them (2026-11-14). On the Mac the menu is the
// system's. Web pages' menus stay upstream's.

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/function_ref.h"
#include "base/run_loop.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/strings/strcat.h"
#include "base/strings/stringprintf.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "compass/browser/ui/views/claude/claude_panel.h"
#include "compass/browser/ui/views/sidebar/compass_sidebar_view.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/color_utils.h"
#include "ui/native_theme/mock_os_settings_provider.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/view_utils.h"
#include "url/gurl.h"

namespace compass {
namespace {

#if !BUILDFLAG(IS_MAC)

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

std::string Hex(SkColor color) {
  return base::StringPrintf("#%02x%02x%02x", SkColorGetR(color),
                            SkColorGetG(color), SkColorGetB(color));
}

// A band of the open menu: its background and the text color farthest from
// it (glyph stems reach the text's own color), as read off the pixels.
struct Band {
  SkColor background = SK_ColorTRANSPARENT;
  SkColor text = SK_ColorTRANSPARENT;
  int rows = 0;
  float Contrast() const {
    return color_utils::GetContrastRatio(background, text);
  }
};

// The menu's items on their usual background, and the highlighted (the
// selected) item's on its own.
struct MenuColors {
  Band items;
  std::optional<Band> highlight;
};

MenuColors ReadMenu(const SkBitmap& bitmap) {
  const int width = bitmap.width();
  const int height = bitmap.height();
  // Each row's most common color: its background.
  std::vector<SkColor> row_background(height);
  std::map<SkColor, int> backgrounds;
  for (int y = 0; y < height; ++y) {
    std::map<SkColor, int> counts;
    for (int x = 0; x < width; ++x) {
      ++counts[SkColorSetA(bitmap.getColor(x, y), 0xFF)];
    }
    row_background[y] =
        std::max_element(counts.begin(), counts.end(), [](auto& a, auto& b) {
          return a.second < b.second;
        })->first;
    ++backgrounds[row_background[y]];
  }
  // The items' background is the most rows'; the highlight's the next most
  // rows', if it differs from it clearly (not a border's line).
  std::vector<std::pair<SkColor, int>> sorted(backgrounds.begin(),
                                              backgrounds.end());
  std::sort(sorted.begin(), sorted.end(),
            [](auto& a, auto& b) { return a.second > b.second; });
  auto read_band = [&](SkColor background) {
    Band band;
    band.background = background;
    band.text = background;
    for (int y = 0; y < height; ++y) {
      if (row_background[y] != background) {
        continue;
      }
      ++band.rows;
      // Two pixels in from the sides: the menu's border.
      for (int x = 2; x < width - 2; ++x) {
        const SkColor color = SkColorSetA(bitmap.getColor(x, y), 0xFF);
        if (color_utils::GetContrastRatio(background, color) >
            band.Contrast()) {
          band.text = color;
        }
      }
    }
    return band;
  };
  MenuColors colors;
  colors.items = read_band(sorted[0].first);
  for (size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i].second >= 8 &&
        color_utils::GetContrastRatio(sorted[i].first, sorted[0].first) >
            1.3f) {
      colors.highlight = read_band(sorted[i].first);
      break;
    }
  }
  return colors;
}

// Runs tasks until |condition| holds or |timeout| passes, without failing.
void WaitFor(base::FunctionRef<bool()> condition, base::TimeDelta timeout) {
  const base::TimeTicks end = base::TimeTicks::Now() + timeout;
  while (!condition() && base::TimeTicks::Now() < end) {
    base::RunLoop run_loop;
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), base::Milliseconds(50));
    run_loop.Run();
  }
}

class SelectPopupBrowserTest : public InProcessBrowserTest {
 protected:
  // The menus are read off their pixels.
  SelectPopupBrowserTest() { EnablePixelOutput(); }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    if (SidePanelUI* ui = SidePanelUI::From(browser())) {
      ui->DisableAnimationsForTesting();
      ui->SetNoDelaysForTesting(true);
    }
  }

  // The system's and the space's mode both |dark|, until |contents| (WebUI,
  // which follows the window) reports it.
  void SetMode(bool dark, content::WebContents* contents) {
    os_settings_provider_.SetPreferredColorScheme(
        dark ? ui::NativeTheme::PreferredColorScheme::kDark
             : ui::NativeTheme::PreferredColorScheme::kLight);
    WindowFeatures::From(browser())->space_model().SetAppearance(
        dark ? SpaceAppearance::kDark : SpaceAppearance::kLight);
    ASSERT_TRUE(base::test::RunUntil([&] {
      return content::EvalJs(
                 contents, "matchMedia('(prefers-color-scheme: dark)').matches")
                 .ExtractBool() == dark;
    }));
  }

  // Opens the <select> |select| (a JS expression) finds in |contents| and
  // reads the menu that opens.
  MenuColors OpenMenu(content::WebContents* contents,
                      const std::string& select,
                      const std::string& name) {
    content::RenderFrameHost* frame = contents->GetPrimaryMainFrame();
    const int process_id = frame->GetProcess()->GetDeprecatedID();
    content::ShowPopupWidgetWaiter waiter(contents, frame);
    // Focused first: a menu whose page loses focus closes. Then
    // showPicker(), with the user activation ExecJs gives, and if no menu
    // came, a click.
    for (int attempt = 0; attempt < 3 && waiter.last_initial_rect().IsEmpty();
         ++attempt) {
      contents->Focus();
      if (attempt == 1) {
        const content::EvalJsResult point = content::EvalJs(
            contents, "(() => { const r = (" + select +
                          ").getBoundingClientRect();"
                          " return [Math.round(r.x + r.width / 2),"
                          " Math.round(r.y + r.height / 2)]; })()");
        const auto& xy = point.ExtractList();
        content::SimulateMouseClickAt(
            contents, 0, blink::WebMouseEvent::Button::kLeft,
            gfx::Point(xy[0].GetInt(), xy[1].GetInt()));
      } else {
        EXPECT_TRUE(content::ExecJs(
            contents,
            "{ const s = " + select + "; s.focus(); s.showPicker(); }"));
      }
      WaitFor([&] { return !waiter.last_initial_rect().IsEmpty(); },
              base::Seconds(3));
    }
    // The popup's widget: the waiter's.
    auto get_popup = [&] {
      return content::RenderWidgetHost::FromID(process_id,
                                               waiter.last_routing_id());
    };
    LOG(WARNING) << name << ": popup " << waiter.last_initial_rect().ToString()
                 << " found " << !!get_popup();
    if (waiter.last_initial_rect().IsEmpty() || !get_popup()) {
      ADD_FAILURE() << "no menu opened";
      return MenuColors();
    }

    // Until the menu has drawn its items: more than one color.
    SkBitmap bitmap;
    EXPECT_TRUE(base::test::RunUntil([&] {
      content::RenderWidgetHost* popup = get_popup();
      content::RenderWidgetHostView* view = popup ? popup->GetView() : nullptr;
      if (!view) {
        return false;
      }
      base::test::TestFuture<const content::CopyFromSurfaceResult&> copied;
      view->CopyFromSurface(gfx::Rect(), gfx::Size(), base::Seconds(5),
                            copied.GetCallback());
      const content::CopyFromSurfaceResult& result = copied.Get();
      if (!result.has_value() || result->bitmap.drawsNothing()) {
        return false;
      }
      bitmap = result->bitmap;
      const SkColor first = bitmap.getColor(bitmap.width() / 2, 0);
      for (int y = 0; y < bitmap.height(); ++y) {
        for (int x = 0; x < bitmap.width(); ++x) {
          if (bitmap.getColor(x, y) != first) {
            return true;
          }
        }
      }
      return false;
    }));
    MenuColors colors = ReadMenu(bitmap);
    LOG(WARNING) << name << ": " << bitmap.width() << "x" << bitmap.height()
                 << " items " << Hex(colors.items.text) << " on "
                 << Hex(colors.items.background) << " ("
                 << colors.items.Contrast() << ":1)"
                 << (colors.highlight
                         ? base::StrCat({", highlight ",
                                         Hex(colors.highlight->text), " on ",
                                         Hex(colors.highlight->background)})
                         : std::string(", no highlight"))
                 << (colors.highlight ? base::StringPrintf(
                                            " (%.2f:1)",
                                            colors.highlight->Contrast())
                                      : std::string());
    // COMPASS_SELECT_POPUP_DUMP=<dir> keeps the menus as PNGs.
    if (std::optional<std::string> dir =
            base::Environment::Create()->GetVar("COMPASS_SELECT_POPUP_DUMP")) {
      base::ScopedAllowBlockingForTesting allow_blocking;
      if (std::optional<std::vector<uint8_t>> png =
              gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, false)) {
        base::WriteFile(base::FilePath::FromUTF8Unsafe(*dir).AppendASCII(
                            name + ".png"),
                        *png);
      }
    }
    // Closed again for the next.
    content::SimulateKeyPress(contents, ui::DomKey::ESCAPE,
                              ui::DomCode::ESCAPE, ui::VKEY_ESCAPE, false,
                              false, false, false);
    EXPECT_TRUE(base::test::RunUntil([&] { return !get_popup(); }));
    return colors;
  }

  // The menu takes the mode, and both its items and its highlighted item
  // read (WCAG AA, 4.5:1).
  void ExpectReadable(const MenuColors& colors, bool dark) {
    const float items_luminance =
        color_utils::GetRelativeLuminance(colors.items.background);
    if (dark) {
      EXPECT_LT(items_luminance, 0.2f) << Hex(colors.items.background);
    } else {
      EXPECT_GT(items_luminance, 0.6f) << Hex(colors.items.background);
    }
    EXPECT_GE(colors.items.Contrast(), 4.5f)
        << Hex(colors.items.text) << " on " << Hex(colors.items.background);
    ASSERT_TRUE(colors.highlight);
    EXPECT_GE(colors.highlight->Contrast(), 4.5f)
        << Hex(colors.highlight->text) << " on "
        << Hex(colors.highlight->background);
  }

  ui::MockOsSettingsProvider os_settings_provider_;
};

constexpr char kChatRoot[] =
    "document.querySelector('compass-claude-chat-app').shadowRoot";

// The Claude panel's model menu.
IN_PROC_BROWSER_TEST_F(SelectPopupBrowserTest, ClaudeModelMenu) {
  ASSERT_TRUE(chrome::ExecuteCommand(browser(), kToggleClaudeCommand));
  claude::ClaudePanel* panel = claude::ClaudePanel::From(browser());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return panel->IsShowing() && panel->web_contents_for_testing();
  }));
  content::WebContents* contents = panel->web_contents_for_testing();
  ASSERT_TRUE(content::WaitForLoadStop(contents));
  const std::string select = std::string(kChatRoot) + ".querySelector('#model')";
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(contents, select + "?.options.length >= 2")
        .ExtractBool();
  }));
  for (const bool dark : {true, false}) {
    SCOPED_TRACE(dark ? "dark" : "light");
    SetMode(dark, contents);
    ExpectReadable(
        OpenMenu(contents, select, dark ? "claude-dark" : "claude-light"),
        dark);
  }
}

// The space menu's "Archive tabs after" menu, in the sidebar.
IN_PROC_BROWSER_TEST_F(SelectPopupBrowserTest, SpaceMenu) {
  auto* sidebar = FindDescendant<CompassSidebarView>(
      BrowserView::GetBrowserViewForBrowser(browser()));
  ASSERT_TRUE(sidebar);
  content::WebContents* contents = sidebar->GetWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(contents));
  const std::string select =
      "document.querySelector('compass-sidebar-app').shadowRoot"
      ".querySelector('#archiveDelay select')";
  for (const bool dark : {true, false}) {
    SCOPED_TRACE(dark ? "dark" : "light");
    SetMode(dark, contents);
    ASSERT_TRUE(content::ExecJs(contents, R"(
      (async () => {
        const root =
            document.querySelector('compass-sidebar-app').shadowRoot;
        while (!root.querySelector('.space')) {
          await new Promise(r => setTimeout(r, 20));
        }
        root.querySelector('.space').dispatchEvent(new MouseEvent(
            'contextmenu', {bubbles: true, cancelable: true, composed: true}));
        while (!root.querySelector('#archiveDelay select')) {
          await new Promise(r => setTimeout(r, 20));
        }
      })())"));
    ExpectReadable(
        OpenMenu(contents, select, dark ? "space-dark" : "space-light"),
        dark);
    // The space menu closes.
    content::SimulateKeyPress(contents, ui::DomKey::ESCAPE,
                              ui::DomCode::ESCAPE, ui::VKEY_ESCAPE, false,
                              false, false, false);
  }
}

// Web pages' menus, as upstream: in the <select>'s used color scheme (light
// without `color-scheme`, whatever the system's mode), readable in each.
IN_PROC_BROWSER_TEST_F(SelectPopupBrowserTest, WebPageMenus) {
  // Four items: most of each menu is its items' background.
  constexpr char kOptions[] =
      "<option>One<option>Two<option>Three<option>Four</select>";
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL(base::StrCat(
          {"data:text/html,<select id=none>", kOptions,
           "<select id=dark style='color-scheme:dark'>", kOptions,
           "<select id=both style='color-scheme:light dark'>", kOptions,
           "<select id=light style='color-scheme:light'>", kOptions}))));
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  for (const bool dark : {true, false}) {
    SCOPED_TRACE(dark ? "dark" : "light");
    SetMode(dark, contents);
    for (const char* id : {"none", "dark", "both", "light"}) {
      SCOPED_TRACE(id);
      const bool dark_menu =
          std::string_view(id) == "dark" ||
          (dark && std::string_view(id) == "both");
      ExpectReadable(
          OpenMenu(contents,
                   base::StrCat({"document.getElementById('", id, "')"}),
                   base::StrCat({"page-", id, dark ? "-dark" : "-light"})),
          dark_menu);
    }
  }
}

#endif  // !BUILDFLAG(IS_MAC)

}  // namespace
}  // namespace compass
