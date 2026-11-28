// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_TRANSLATE_TRANSLATION_TAB_STATE_H_
#define COMPASS_BROWSER_TRANSLATE_TRANSLATION_TAB_STATE_H_

#include <cstdint>
#include <string>

#include "base/functional/callback.h"
#include "components/translate/content/browser/content_translate_driver.h"
#include "components/translate/core/browser/translate_step.h"
#include "components/translate/core/common/translate_errors.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

// Where a tab's page translation is, for the pill's translate button
// (docs/design/translation.md, "What the user sees"), in place of upstream's
// translate bubble.

namespace compass::translate {

class TranslationTabState
    : public content::WebContentsUserData<TranslationTabState>,
      public content::WebContentsObserver,
      public ::translate::ContentTranslateDriver::TranslationObserver {
 public:
  enum class Step {
    // Nothing to offer: the page is in a language the user reads, or not
    // known yet.
    kNone,
    // In another language: the button offers to translate.
    kOffered,
    kTranslating,
    // Translating, and first downloading the language (received of total).
    kDownloading,
    kTranslated,
    kError,
  };

  ~TranslationTabState() override;

  // How the UI hears of a change, for any tab (browser/ui/translate/).
  static void SetChangedCallback(
      base::RepeatingCallback<void(content::WebContents*)> callback);
  // Whether the UI can show the state for `web_contents`: false when its
  // window's header has no pill (laid out collapsed), where upstream's
  // bubble stays.
  static void SetCanShowCallback(
      base::RepeatingCallback<bool(content::WebContents*)> callback);

  Step step() const { return step_; }
  // Upstream's codes ("de"), as the translate manager has them.
  const std::string& source_language() const { return source_; }
  const std::string& target_language() const { return target_; }
  int64_t received() const { return received_; }
  int64_t total() const { return total_; }
  ::translate::TranslateErrors error() const { return error_; }

  // A translate step, from upstream's ChromeTranslateClient::ShowTranslateUI()
  // (patch). False when the UI can't show it, so upstream's bubble does.
  static bool OnTranslateStep(content::WebContents* web_contents,
                              ::translate::TranslateStep step,
                              const std::string& source_language,
                              const std::string& target_language,
                              ::translate::TranslateErrors error);

  // Nothing more to offer (the user chose Never Translate).
  void Dismiss() { Set(Step::kNone); }

  // From PageTranslatorHost while it downloads a language the page needs.
  void SetDownloading(int64_t received, int64_t total);
  void DownloadFinished();

 private:
  friend class content::WebContentsUserData<TranslationTabState>;
  explicit TranslationTabState(content::WebContents* web_contents);

  void Set(Step step);

  // content::WebContentsObserver:
  void PrimaryPageChanged(content::Page& page) override;
  void WebContentsDestroyed() override;

  // ::translate::ContentTranslateDriver::TranslationObserver:
  void OnIsPageTranslatedChanged(content::WebContents* source) override;

  Step step_ = Step::kNone;
  std::string source_;
  std::string target_;
  int64_t received_ = 0;
  int64_t total_ = 0;
  ::translate::TranslateErrors error_ = ::translate::TranslateErrors::NONE;
  bool observing_driver_ = false;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace compass::translate

#endif  // COMPASS_BROWSER_TRANSLATE_TRANSLATION_TAB_STATE_H_
