// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/translate/translation_tab_state.h"

#include <utility>

#include "base/no_destructor.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "components/translate/core/browser/language_state.h"
#include "components/translate/core/common/translate_constants.h"
#include "content/public/browser/web_contents.h"

namespace compass::translate {

namespace {

base::RepeatingCallback<void(content::WebContents*)>& ChangedCallback() {
  static base::NoDestructor<base::RepeatingCallback<void(content::WebContents*)>>
      callback;
  return *callback;
}

base::RepeatingCallback<bool(content::WebContents*)>& CanShowCallback() {
  static base::NoDestructor<base::RepeatingCallback<bool(content::WebContents*)>>
      callback;
  return *callback;
}

}  // namespace

// static
void TranslationTabState::SetChangedCallback(
    base::RepeatingCallback<void(content::WebContents*)> callback) {
  ChangedCallback() = std::move(callback);
}

// static
void TranslationTabState::SetCanShowCallback(
    base::RepeatingCallback<bool(content::WebContents*)> callback) {
  CanShowCallback() = std::move(callback);
}

TranslationTabState::TranslationTabState(content::WebContents* web_contents)
    : content::WebContentsUserData<TranslationTabState>(*web_contents),
      content::WebContentsObserver(web_contents) {
  if (ChromeTranslateClient* client =
          ChromeTranslateClient::FromWebContents(web_contents)) {
    client->translate_driver()->AddTranslationObserver(this);
    observing_driver_ = true;
  }
}

TranslationTabState::~TranslationTabState() = default;

// static
bool TranslationTabState::OnTranslateStep(
    content::WebContents* web_contents,
    ::translate::TranslateStep step,
    const std::string& source_language,
    const std::string& target_language,
    ::translate::TranslateErrors error) {
  if (!web_contents || !CanShowCallback() ||
      !CanShowCallback().Run(web_contents)) {
    return false;
  }
  TranslationTabState* state =
      TranslationTabState::GetOrCreateForWebContents(web_contents);
  state->source_ = source_language;
  state->target_ = target_language;
  state->error_ = error;
  switch (step) {
    case ::translate::TRANSLATE_STEP_BEFORE_TRANSLATE:
    case ::translate::TRANSLATE_STEP_AFTER_UNDO:
      state->Set(Step::kOffered);
      break;
    case ::translate::TRANSLATE_STEP_TRANSLATING:
      // A download already showing stays until it ends.
      if (state->step_ != Step::kDownloading) {
        state->Set(Step::kTranslating);
      }
      break;
    case ::translate::TRANSLATE_STEP_AFTER_TRANSLATE:
      state->Set(Step::kTranslated);
      break;
    case ::translate::TRANSLATE_STEP_TRANSLATE_ERROR:
      state->Set(Step::kError);
      break;
  }
  return true;
}

void TranslationTabState::SetDownloading(int64_t received, int64_t total) {
  received_ = received;
  total_ = total;
  Set(Step::kDownloading);
}

void TranslationTabState::DownloadFinished() {
  if (step_ == Step::kDownloading) {
    Set(Step::kTranslating);
  }
}

void TranslationTabState::Set(Step step) {
  step_ = step;
  if (ChangedCallback()) {
    ChangedCallback().Run(&GetWebContents());
  }
}

void TranslationTabState::PrimaryPageChanged(content::Page& page) {
  // Another page: upstream offers again once it knows its language.
  error_ = ::translate::TranslateErrors::NONE;
  Set(Step::kNone);
}

void TranslationTabState::WebContentsDestroyed() {
  if (observing_driver_) {
    if (ChromeTranslateClient* client =
            ChromeTranslateClient::FromWebContents(web_contents())) {
      client->translate_driver()->RemoveTranslationObserver(this);
    }
    observing_driver_ = false;
  }
}

void TranslationTabState::OnIsPageTranslatedChanged(
    content::WebContents* source) {
  // Back to the original (Show Original, from anywhere): offered again.
  ChromeTranslateClient* client =
      ChromeTranslateClient::FromWebContents(&GetWebContents());
  if (client && !client->GetLanguageState().IsPageTranslated() &&
      step_ == Step::kTranslated) {
    Set(Step::kOffered);
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(TranslationTabState);

}  // namespace compass::translate
