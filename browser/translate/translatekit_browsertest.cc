// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Upstream's on-device translation service running Compass's library
// (docs/design/translation.md, "Where the library lives").

#include <optional>
#include <string>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/strings/strcat.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/test/base/chrome_test_utils.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/translate/translatekit_library.h"
#include "components/language/core/browser/pref_names.h"
#include "components/on_device_translation/features.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"

namespace compass {
namespace {

// A folder with a German-English package, as the library's tests use:
// <dir>/de-en/de-en/{model.bin,vocab.spm}. Without it the translation test
// is skipped.
std::optional<std::string> TestPackages() {
  std::optional<std::string> dir =
      base::Environment::Create()->GetVar("COMPASS_TRANSLATEKIT_TEST_PACKAGES");
  if (!dir || dir->empty()) {
    return std::nullopt;
  }
  return dir;
}

class TranslateKitBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    if (std::optional<std::string> packages = TestPackages()) {
      // Upstream's switch for packages that aren't installed.
      command_line->AppendSwitchASCII(
          "translate-kit-packages",
          base::StrCat({"de,en,", *packages, "/de-en"}));
    }
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_https_test_server().ServeFilesFromSourceDirectory(
        "chrome/test/data");
    ASSERT_TRUE(embedded_https_test_server().Start());
  }
};

// Startup points the service at the bundled library, so Google's
// TranslateKit component is never registered.
IN_PROC_BROWSER_TEST_F(TranslateKitBrowserTest, UsesTheBundledLibrary) {
  base::ScopedAllowBlockingForTesting allow_blocking;  // It looks on disk.
  base::FilePath library = GetBundledTranslateKitLibrary();
  ASSERT_FALSE(library.empty());
  EXPECT_EQ(base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
                on_device_translation::kTranslateKitBinaryPath),
            library);
}

// The web Translator API, through the sandboxed service, translates with
// Bergamot.
IN_PROC_BROWSER_TEST_F(TranslateKitBrowserTest, TranslatorApiTranslates) {
  if (!TestPackages()) {
    GTEST_SKIP() << "COMPASS_TRANSLATEKIT_TEST_PACKAGES isn't set";
  }
  // Both languages among the user's: "available", not "downloadable".
  chrome_test_utils::GetProfile(this)->GetPrefs()->SetString(
      language::prefs::kSelectedLanguages, "en,de");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_https_test_server().GetURL("/empty.html")));
  content::WebContents* contents =
      chrome_test_utils::GetActiveWebContents(this);

  EXPECT_EQ(content::EvalJs(contents, R"(
      Translator.availability({sourceLanguage: 'de', targetLanguage: 'en'}))"),
            "available");
  EXPECT_EQ(content::EvalJs(contents, R"((async () => {
      const translator = await Translator.create(
          {sourceLanguage: 'de', targetLanguage: 'en'});
      return await translator.translate('Guten Morgen.');
    })())"),
            "Good morning.");
}

}  // namespace
}  // namespace compass
