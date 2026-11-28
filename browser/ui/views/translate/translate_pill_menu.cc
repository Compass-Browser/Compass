// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/translate/translate_pill_menu.h"

#include <algorithm>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "compass/browser/translate/translation_tab_state.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/translate/core/browser/translate_download_manager.h"
#include "components/translate/core/browser/translate_manager.h"
#include "components/translate/core/browser/translate_prefs.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/view.h"

namespace compass {

namespace {

enum Command {
  kTranslatedFrom = 1,
  kShowOriginal,
  kTryAgain,
  kAlwaysTranslate,
  kNeverTranslateLanguage,
  kNeverTranslateSite,
  // The Translate To submenu's languages, from here up.
  kFirstTarget = 100,
};

std::u16string LanguageName(const std::string& code) {
  return l10n_util::GetDisplayNameForLocale(
      code, g_browser_process->GetApplicationLocale(), true);
}

using translate::TranslationTabState;

}  // namespace

TranslatePillMenu::TranslatePillMenu(content::WebContents* web_contents)
    : web_contents_(web_contents->GetWeakPtr()) {
  if (TranslationTabState* state =
          TranslationTabState::FromWebContents(web_contents)) {
    source_ = state->source_language();
    target_ = state->target_language();
  }
  Build();
}

TranslatePillMenu::~TranslatePillMenu() = default;

void TranslatePillMenu::Build() {
  model_ = std::make_unique<ui::SimpleMenuModel>(this);
  targets_model_ = std::make_unique<ui::SimpleMenuModel>(this);
  TranslationTabState* state =
      web_contents_ ? TranslationTabState::FromWebContents(web_contents_.get())
                    : nullptr;
  const bool translated =
      state && state->step() == TranslationTabState::Step::kTranslated;

  model_->AddItem(kTranslatedFrom,
                  l10n_util::GetStringFUTF16(
                      translated ? IDS_COMPASS_TRANSLATE_MENU_TRANSLATED_FROM
                                 : IDS_COMPASS_TRANSLATE_MENU_PAGE_IN,
                      LanguageName(source_)));

  // The languages Compass has (upstream's list, filtered: chromium_src/.../
  // translate_language_list.cc), by name.
  std::vector<std::string> codes;
  ::translate::TranslateDownloadManager::GetSupportedLanguages(
      /*translate_allowed=*/true, &codes);
  std::erase(codes, source_);
  std::ranges::sort(codes, {}, &LanguageName);
  targets_ = codes;
  for (size_t i = 0; i < targets_.size(); ++i) {
    targets_model_->AddCheckItem(kFirstTarget + static_cast<int>(i),
                                 LanguageName(targets_[i]));
  }
  model_->AddSubMenuWithStringId(0, IDS_COMPASS_TRANSLATE_MENU_TRANSLATE_TO,
                                 targets_model_.get());
  if (translated) {
    model_->AddItemWithStringId(kShowOriginal,
                                IDS_COMPASS_TRANSLATE_MENU_SHOW_ORIGINAL);
  } else {
    model_->AddItemWithStringId(kTryAgain, IDS_COMPASS_TRANSLATE_MENU_TRY_AGAIN);
  }
  model_->AddSeparator(ui::NORMAL_SEPARATOR);
  model_->AddCheckItem(
      kAlwaysTranslate,
      l10n_util::GetStringFUTF16(IDS_COMPASS_TRANSLATE_MENU_ALWAYS,
                                 LanguageName(source_)));
  model_->AddItem(kNeverTranslateLanguage,
                  l10n_util::GetStringFUTF16(IDS_COMPASS_TRANSLATE_MENU_NEVER,
                                             LanguageName(source_)));
  model_->AddItemWithStringId(kNeverTranslateSite,
                              IDS_COMPASS_TRANSLATE_MENU_NEVER_SITE);
}

void TranslatePillMenu::Show(views::View* anchor) {
  runner_ = std::make_unique<views::MenuRunner>(
      model_.get(), views::MenuRunner::HAS_MNEMONICS);
  runner_->RunMenuAt(anchor->GetWidget(), nullptr, anchor->GetBoundsInScreen(),
                     views::MenuAnchorPosition::kTopLeft,
                     ui::mojom::MenuSourceType::kNone);
}

bool TranslatePillMenu::IsCommandIdChecked(int command_id) const {
  if (command_id >= kFirstTarget) {
    return targets_[command_id - kFirstTarget] == target_;
  }
  if (command_id == kAlwaysTranslate && web_contents_) {
    ChromeTranslateClient* client =
        ChromeTranslateClient::FromWebContents(web_contents_.get());
    return client && client->GetTranslatePrefs()
                         ->IsLanguagePairOnAlwaysTranslateList(source_, target_);
  }
  return false;
}

bool TranslatePillMenu::IsCommandIdEnabled(int command_id) const {
  return command_id != kTranslatedFrom;
}

void TranslatePillMenu::ExecuteCommand(int command_id, int event_flags) {
  if (!web_contents_) {
    return;
  }
  ChromeTranslateClient* client =
      ChromeTranslateClient::FromWebContents(web_contents_.get());
  if (!client) {
    return;
  }
  ::translate::TranslateManager* manager = client->GetTranslateManager();
  std::unique_ptr<::translate::TranslatePrefs> prefs =
      client->GetTranslatePrefs();
  const bool translated = client->GetLanguageState().IsPageTranslated();
  if (command_id >= kFirstTarget) {
    // Another language: from the original again (page_translator.js
    // restores before it starts).
    manager->TranslatePage(source_, targets_[command_id - kFirstTarget],
                           /*triggered_from_menu=*/true);
    return;
  }
  switch (command_id) {
    case kShowOriginal:
      manager->RevertTranslation();
      break;
    case kTryAgain:
      manager->TranslatePage(source_, target_, /*triggered_from_menu=*/true);
      break;
    case kAlwaysTranslate:
      if (prefs->IsLanguagePairOnAlwaysTranslateList(source_, target_)) {
        prefs->RemoveLanguagePairFromAlwaysTranslateList(source_);
      } else {
        prefs->AddLanguagePairToAlwaysTranslateList(source_, target_);
      }
      break;
    case kNeverTranslateLanguage:
    case kNeverTranslateSite:
      if (command_id == kNeverTranslateLanguage) {
        prefs->BlockLanguage(source_);
      } else {
        prefs->AddSiteToNeverPromptList(
            web_contents_->GetLastCommittedURL().host());
      }
      if (translated) {
        manager->RevertTranslation();
      }
      if (TranslationTabState* state =
              TranslationTabState::FromWebContents(web_contents_.get())) {
        state->Dismiss();
      }
      break;
  }
}

}  // namespace compass
