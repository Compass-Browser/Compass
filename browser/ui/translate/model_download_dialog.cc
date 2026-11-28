// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The question before a page translation's first download
// (docs/design/translation.md, "Asking first"): a tab-modal dialog on the
// page being translated.

#include <memory>
#include <utility>

#include "chrome/browser/ui/tab_modal_confirm_dialog.h"
#include "chrome/browser/ui/tab_modal_confirm_dialog_delegate.h"
#include "compass/browser/translate/page_translator_host.h"
#include "compass/browser/translate/translation_tab_state.h"
#include "compass/strings/grit/compass_strings.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/l10n/l10n_util.h"

namespace compass::translate {

namespace {

class ModelDownloadDialog : public TabModalConfirmDialogDelegate {
 public:
  ModelDownloadDialog(content::WebContents* web_contents,
                      base::OnceCallback<void(bool)> callback)
      : TabModalConfirmDialogDelegate(web_contents),
        callback_(std::move(callback)) {}
  ~ModelDownloadDialog() override { Answer(false); }

  std::u16string GetTitle() override {
    return l10n_util::GetStringUTF16(IDS_COMPASS_TRANSLATE_DOWNLOAD_TITLE);
  }
  std::u16string GetDialogMessage() override {
    return l10n_util::GetStringUTF16(IDS_COMPASS_TRANSLATE_DOWNLOAD_MESSAGE);
  }
  std::u16string GetAcceptButtonTitle() override {
    return l10n_util::GetStringUTF16(IDS_COMPASS_TRANSLATE_DOWNLOAD_ACCEPT);
  }
  std::u16string GetCancelButtonTitle() override {
    return l10n_util::GetStringUTF16(IDS_COMPASS_TRANSLATE_DOWNLOAD_CANCEL);
  }

 private:
  void OnAccepted() override { Answer(true); }
  void OnCanceled() override { Answer(false); }
  void OnClosed() override { Answer(false); }

  void Answer(bool allowed) {
    if (callback_) {
      std::move(callback_).Run(allowed);
    }
  }

  base::OnceCallback<void(bool)> callback_;
};

}  // namespace

void AskForModelDownloads(content::WebContents* web_contents,
                          base::OnceCallback<void(bool)> callback) {
  if (!web_contents) {
    std::move(callback).Run(false);
    return;
  }
  TabModalConfirmDialog::Create(
      std::make_unique<ModelDownloadDialog>(web_contents, std::move(callback)),
      web_contents);
}

void ShowModelDownloadProgress(content::WebContents* web_contents,
                               int64_t received,
                               int64_t total) {
  if (web_contents) {
    TranslationTabState::GetOrCreateForWebContents(web_contents)
        ->SetDownloading(received, total);
  }
}

void ModelDownloadFinished(content::WebContents* web_contents) {
  if (TranslationTabState* state =
          web_contents ? TranslationTabState::FromWebContents(web_contents)
                       : nullptr) {
    state->DownloadFinished();
  }
}

}  // namespace compass::translate
