// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Upstream's page translation, on Compass's engine (docs/design/
// translation.md, "Desktop: the page").

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/strings/strcat.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "chrome/browser/translate/translate_test_utils.h"
#include "chrome/test/base/chrome_test_utils.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/translate/translation_pack_installer.h"
#include "compass/browser/translate/translation_tab_state.h"
#include "compass/browser/ui/views/sidebar/sidebar_header.h"
#include "components/prefs/pref_service.h"
#include "components/translate/content/browser/translate_waiter.h"
#include "components/translate/core/browser/translate_download_manager.h"
#include "components/translate/core/browser/translate_manager.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "ui/events/event.h"
#include "ui/events/event_utils.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/menu/menu_controller.h"
#include "ui/views/widget/widget.h"

namespace compass {
namespace {

using ::translate::TranslateWaiter;

constexpr char kGermanPage[] = R"(<!doctype html>
<html lang="de"><head><meta charset="utf-8"><title>Test</title></head><body>
<p id="plain">Guten Morgen. Die Übersetzung läuft vollständig auf dem
Gerät, und kein Text verlässt den Computer.</p>
<p id="link">Klicken Sie <a id="a" href="/hilfe">hier</a>, um
fortzufahren.</p>
</body></html>)";

std::optional<std::string> TestPackages() {
  std::optional<std::string> dir =
      base::Environment::Create()->GetVar("COMPASS_TRANSLATEKIT_TEST_PACKAGES");
  if (!dir || dir->empty()) {
    return std::nullopt;
  }
  return dir;
}

class PageTranslationBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    if (std::optional<std::string> packages = TestPackages()) {
      command_line->AppendSwitchASCII(
          "translate-kit-packages",
          base::StrCat({"de,en,", *packages, "/de-en"}));
    }
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_https_test_server().RegisterRequestHandler(base::BindRepeating(
        [](const net::test_server::HttpRequest& request)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          if (request.relative_url != "/german.html") {
            return nullptr;
          }
          auto response =
              std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_content(kGermanPage);
          response->set_content_type("text/html; charset=utf-8");
          return response;
        }));
    ASSERT_TRUE(embedded_https_test_server().Start());
  }

  content::WebContents* contents() {
    return chrome_test_utils::GetActiveWebContents(this);
  }

  // Loads the German page and waits for its language.
  void LoadGermanPage() {
    auto waiter = ::translate::CreateTranslateWaiter(
        contents(), TranslateWaiter::WaitEvent::kLanguageDetermined);
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_https_test_server().GetURL("/german.html")));
    waiter->Wait();
  }

  compass::translate::TranslationTabState* tab_state() {
    return compass::translate::TranslationTabState::FromWebContents(
        contents());
  }

  views::LabelButton* translate_button() {
    return SidebarHeader::From(browser())->translate_button_for_testing();
  }

  // A real left click, through the widget's hit testing.
  void ClickTranslateButton() {
    views::Widget* widget = translate_button()->GetWidget();
    gfx::Point center = translate_button()->GetLocalBounds().CenterPoint();
    views::View::ConvertPointToWidget(translate_button(), &center);
    ui::MouseEvent press(ui::EventType::kMousePressed, center, center,
                         ui::EventTimeForNow(), ui::EF_LEFT_MOUSE_BUTTON,
                         ui::EF_LEFT_MOUSE_BUTTON);
    ui::MouseEvent release(ui::EventType::kMouseReleased, center, center,
                           ui::EventTimeForNow(), ui::EF_LEFT_MOUSE_BUTTON,
                           ui::EF_LEFT_MOUSE_BUTTON);
    widget->OnMouseEvent(&press);
    widget->OnMouseEvent(&release);
  }

  ChromeTranslateClient* client() {
    return ChromeTranslateClient::FromWebContents(contents());
  }

  std::string Text(const std::string& id) {
    return content::EvalJs(contents(),
                           "document.getElementById('" + id + "').textContent")
        .ExtractString();
  }
};

// Without Google's keys, upstream would offer nothing; Compass's languages
// are offered, and only those.
IN_PROC_BROWSER_TEST_F(PageTranslationBrowserTest, OffersTranslation) {
  LoadGermanPage();
  EXPECT_EQ(client()->GetLanguageState().source_language(), "de");
  EXPECT_TRUE(::translate::TranslateManager::IsAvailable(
      client()->GetTranslatePrefs().get()));

  std::vector<std::string> languages;
  ::translate::TranslateDownloadManager::GetSupportedLanguages(
      /*translate_allowed=*/true, &languages);
  auto has = [&](const std::string& code) {
    return std::ranges::find(languages, code) != languages.end();
  };
  EXPECT_TRUE(has("de"));
  EXPECT_TRUE(has("it"));
  EXPECT_TRUE(has("en"));
  EXPECT_FALSE(has("jw"));  // Javanese: no model.
  EXPECT_FALSE(has("la"));  // Latin: no model.
}

// The pill's translate button offers it, in place of upstream's bubble, and
// shows a download's progress.
IN_PROC_BROWSER_TEST_F(PageTranslationBrowserTest, PillButtonOffers) {
  using Step = compass::translate::TranslationTabState::Step;
  LoadGermanPage();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return tab_state() && tab_state()->step() == Step::kOffered; }));
  EXPECT_EQ(tab_state()->source_language(), "de");
  EXPECT_TRUE(translate_button()->GetVisible());
  EXPECT_TRUE(translate_button()->GetText().empty());

  tab_state()->SetDownloading(12'300'000, 45'000'000);
  EXPECT_EQ(translate_button()->GetText(), u"12/45 MB");
  tab_state()->DownloadFinished();
  EXPECT_EQ(tab_state()->step(), Step::kTranslating);
  EXPECT_TRUE(translate_button()->GetText().empty());

  // Another page in the user's language: nothing to offer.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  EXPECT_EQ(tab_state()->step(), Step::kNone);
  EXPECT_FALSE(translate_button()->GetVisible());
}

// Clicking the pill's button translates into the user's language: with
// the German pack there, at once; without it, first asking to download.
IN_PROC_BROWSER_TEST_F(PageTranslationBrowserTest, PillButtonTranslates) {
  using Step = compass::translate::TranslationTabState::Step;
  LoadGermanPage();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return tab_state() && tab_state()->step() == Step::kOffered; }));
  ClickTranslateButton();
  auto* dialogs =
      web_modal::WebContentsModalDialogManager::FromWebContents(contents());
  if (TestPackages()) {
    EXPECT_TRUE(base::test::RunUntil(
        [&] { return tab_state()->step() == Step::kTranslated; }))
        << "step " << static_cast<int>(tab_state()->step());
    EXPECT_NE(Text("plain").find("Good morning"), std::string::npos)
        << Text("plain");
    return;
  }
  EXPECT_TRUE(base::test::RunUntil([&] { return dialogs->IsDialogActive(); }))
      << "step " << static_cast<int>(tab_state()->step());
  EXPECT_EQ(tab_state()->step(), Step::kTranslating);
  dialogs->CloseAllDialogs();
}

// On a translated page, clicking the pill's button opens its menu.
IN_PROC_BROWSER_TEST_F(PageTranslationBrowserTest, PillButtonOpensMenu) {
  if (!TestPackages()) {
    GTEST_SKIP() << "COMPASS_TRANSLATEKIT_TEST_PACKAGES isn't set";
  }
  using Step = compass::translate::TranslationTabState::Step;
  LoadGermanPage();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return tab_state() && tab_state()->step() == Step::kOffered; }));
  auto translated = ::translate::CreateTranslateWaiter(
      contents(), TranslateWaiter::WaitEvent::kPageTranslated);
  client()->GetTranslateManager()->TranslatePage("de", "en",
                                                 /*triggered_from_menu=*/false);
  translated->Wait();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return tab_state()->step() == Step::kTranslated; }))
      << "step " << static_cast<int>(tab_state()->step());
  ASSERT_TRUE(translate_button()->GetVisible());

  ClickTranslateButton();
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return views::MenuController::GetActiveInstance() != nullptr; }));
  if (views::MenuController* menu = views::MenuController::GetActiveInstance()) {
    menu->Cancel(views::MenuController::ExitType::kAll);
  }
}

// The pill's Translate: Compass's script in the page, the engine through
// compassTranslate and the browser, then Show Original.
IN_PROC_BROWSER_TEST_F(PageTranslationBrowserTest, TranslatesAndRestores) {
  if (!TestPackages()) {
    GTEST_SKIP() << "COMPASS_TRANSLATEKIT_TEST_PACKAGES isn't set";
  }
  LoadGermanPage();
  ASSERT_TRUE(content::ExecJs(contents(),
                              "window.link = document.getElementById('a');"));

  auto translated = ::translate::CreateTranslateWaiter(
      contents(), TranslateWaiter::WaitEvent::kPageTranslated);
  client()->GetTranslateManager()->TranslatePage("de", "en",
                                                 /*triggered_from_menu=*/false);
  translated->Wait();
  EXPECT_FALSE(client()->GetLanguageState().translation_error());

  EXPECT_NE(Text("plain").find("Good morning"), std::string::npos)
      << Text("plain");
  const std::string link = Text("link");
  EXPECT_EQ(link.find("Klicken"), std::string::npos) << link;
  EXPECT_EQ(content::EvalJs(contents(), R"(
      window.link === document.getElementById('a') &&
      window.link.getAttribute('href') === '/hilfe' &&
      document.getElementById('link').contains(window.link))"),
            true);

  auto reverted = ::translate::CreateTranslateWaiter(
      contents(), TranslateWaiter::WaitEvent::kIsPageTranslatedChanged);
  client()->GetTranslateManager()->RevertTranslation();
  reverted->Wait();
  EXPECT_NE(Text("plain").find("Guten Morgen"), std::string::npos)
      << Text("plain");
}

// A language that isn't downloaded asks first; turning it down leaves the
// page as it was, records nothing and downloads nothing.
IN_PROC_BROWSER_TEST_F(PageTranslationBrowserTest, AsksBeforeDownloading) {
  ASSERT_FALSE(g_browser_process->local_state()->GetBoolean(
      compass::translate::kModelDownloadsAllowed));
  LoadGermanPage();
  const std::string before = Text("plain");

  auto translated = ::translate::CreateTranslateWaiter(
      contents(), TranslateWaiter::WaitEvent::kPageTranslated);
  // Italian's pack is never installed in tests.
  client()->GetTranslateManager()->TranslatePage("de", "it",
                                                 /*triggered_from_menu=*/false);
  auto* dialogs =
      web_modal::WebContentsModalDialogManager::FromWebContents(contents());
  ASSERT_TRUE(base::test::RunUntil([&] { return dialogs->IsDialogActive(); }));

  // Not Now (or Esc, or closing the tab's dialog).
  dialogs->CloseAllDialogs();
  translated->Wait();
  EXPECT_TRUE(client()->GetLanguageState().translation_error());
  EXPECT_EQ(Text("plain"), before);
  EXPECT_FALSE(g_browser_process->local_state()->GetBoolean(
      compass::translate::kModelDownloadsAllowed));
  base::ScopedAllowBlockingForTesting allow_blocking;
  EXPECT_FALSE(base::PathExists(
      compass::translate::PackInstaller::Get().packs_dir().AppendASCII(
          "en_it")));
}

}  // namespace
}  // namespace compass
