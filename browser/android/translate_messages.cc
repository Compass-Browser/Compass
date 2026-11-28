// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Page translation's question and download progress on Android
// (docs/design/translation.md, "Android"): the question as a tab-modal
// dialog, the progress as one of Chrome's messages (the bar upstream's own
// translate message uses). The text is the desktop's.

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/i18n/number_formatting.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "compass/browser/translate/page_translator_host.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/messages/android/message_dispatcher_bridge.h"
#include "components/messages/android/message_enums.h"
#include "components/messages/android/message_wrapper.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/android/modal_dialog_wrapper.h"
#include "ui/android/window_android.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/dialog_model.h"

namespace compass::translate {

namespace {

// The progress stays until the download ends.
constexpr long kProgressMs = 10 * 60 * 1000;

// A tab's messages: the question while it's asked, the progress while a
// language downloads.
class TranslateMessages
    : public content::WebContentsUserData<TranslateMessages> {
 public:
  // Not upstream's message: its client may be gone with the tab.
  ~TranslateMessages() override { DismissOwnProgress(); }

  void Ask(base::OnceCallback<void(bool)> callback) {
    // One question for every request waiting (the page sends batches in
    // parallel), all answered alike. A dialog, not a message: messages
    // stack, and upstream's "Translating page" would sit in front of it.
    answers_.push_back(std::move(callback));
    if (asking_) {
      return;
    }
    ui::WindowAndroid* window = GetWebContents().GetTopLevelNativeWindow();
    if (!window || !window->GetModalDialogManagerBridge()) {
      LOG(WARNING) << "Compass translate: no window to ask in";
      Answer(false);
      return;
    }
    asking_ = true;
    auto answer = [](base::WeakPtr<TranslateMessages> self, bool allowed) {
      if (self) {
        self->Answer(allowed);
      }
    };
    ui::DialogModel::Builder builder;
    builder
        .SetTitle(
            l10n_util::GetStringUTF16(IDS_COMPASS_TRANSLATE_DOWNLOAD_TITLE))
        .AddParagraph(ui::DialogModelLabel(
            l10n_util::GetStringUTF16(IDS_COMPASS_TRANSLATE_DOWNLOAD_MESSAGE)))
        .AddOkButton(
            base::BindOnce(answer, weak_factory_.GetWeakPtr(), true),
            ui::DialogModel::Button::Params().SetLabel(
                l10n_util::GetStringUTF16(
                    IDS_COMPASS_TRANSLATE_DOWNLOAD_ACCEPT)))
        .AddCancelButton(
            base::BindOnce(answer, weak_factory_.GetWeakPtr(), false),
            ui::DialogModel::Button::Params().SetLabel(
                l10n_util::GetStringUTF16(
                    IDS_COMPASS_TRANSLATE_DOWNLOAD_CANCEL)))
        .SetCloseActionCallback(
            base::BindOnce(answer, weak_factory_.GetWeakPtr(), false))
        .SetDialogDestroyingCallback(
            base::BindOnce(answer, weak_factory_.GetWeakPtr(), false));
    ui::ModalDialogWrapper::ShowTabModal(builder.Build(), window);
  }

  void Progress(int64_t received, int64_t total) {
    const int64_t mb = 1000 * 1000;
    const std::u16string got = base::FormatNumber(received / mb);
    const std::u16string all =
        base::FormatNumber(std::max<int64_t>(1, (total + mb / 2) / mb));
    const std::u16string text = l10n_util::GetStringFUTF16(
        IDS_COMPASS_SIDEBAR_HEADER_TRANSLATE_PROGRESS, got, all);
    // In upstream's "Translating page" message when it shows ("German to
    // English · 12/45 MB"): a message of our own would stack behind it.
    if (ChromeTranslateClient* client =
            ChromeTranslateClient::FromWebContents(&GetWebContents());
        client && client->ShowCompassDownloadProgress(text)) {
      in_translate_message_ = true;
      DismissOwnProgress();
      return;
    }
    if (progress_) {
      progress_->SetDescription(text);
      return;
    }
    progress_ = std::make_unique<messages::MessageWrapper>(
        messages::MessageIdentifier::DOWNLOAD_PROGRESS, base::OnceClosure(),
        base::BindOnce(&TranslateMessages::OnProgressDismissed,
                       base::Unretained(this)));
    progress_->SetTitle(
        l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_HEADER_TRANSLATING));
    progress_->SetDescription(text);
    progress_->SetDuration(kProgressMs);
    if (!messages::MessageDispatcherBridge::Get()->EnqueueMessage(
            progress_.get(), &GetWebContents(),
            messages::MessageScopeType::NAVIGATION,
            messages::MessagePriority::kUrgent)) {
      progress_.reset();
    }
  }

  void DismissProgress() {
    if (in_translate_message_) {
      in_translate_message_ = false;
      if (ChromeTranslateClient* client =
              ChromeTranslateClient::FromWebContents(&GetWebContents())) {
        client->ShowCompassDownloadProgress(std::u16string());
      }
    }
    DismissOwnProgress();
  }

  void DismissOwnProgress() {
    if (progress_) {
      messages::MessageDispatcherBridge::Get()->DismissMessage(
          progress_.get(), messages::DismissReason::UNKNOWN);
    }
  }

 private:
  friend class content::WebContentsUserData<TranslateMessages>;
  explicit TranslateMessages(content::WebContents* web_contents)
      : content::WebContentsUserData<TranslateMessages>(*web_contents) {}

  void Answer(bool allowed) {
    asking_ = false;
    std::vector<base::OnceCallback<void(bool)>> answers = std::move(answers_);
    answers_.clear();
    for (auto& answer : answers) {
      std::move(answer).Run(allowed);
    }
  }

  void OnProgressDismissed(messages::DismissReason reason) {
    progress_.reset();
  }

  std::vector<base::OnceCallback<void(bool)>> answers_;
  bool asking_ = false;
  // The progress shows in upstream's translate message.
  bool in_translate_message_ = false;
  std::unique_ptr<messages::MessageWrapper> progress_;

  base::WeakPtrFactory<TranslateMessages> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(TranslateMessages);

}  // namespace

void AskForModelDownloads(content::WebContents* web_contents,
                          base::OnceCallback<void(bool)> callback) {
  if (!web_contents ||
      !messages::MessageDispatcherBridge::Get()
           ->IsMessagesEnabledForEmbedder()) {
    LOG(WARNING) << "Compass translate: no messages to ask with";
    std::move(callback).Run(false);
    return;
  }
  TranslateMessages::CreateForWebContents(web_contents);
  TranslateMessages::FromWebContents(web_contents)->Ask(std::move(callback));
}

void ShowModelDownloadProgress(content::WebContents* web_contents,
                               int64_t received,
                               int64_t total) {
  if (!web_contents) {
    return;
  }
  TranslateMessages::CreateForWebContents(web_contents);
  TranslateMessages::FromWebContents(web_contents)->Progress(received, total);
}

void ModelDownloadFinished(content::WebContents* web_contents) {
  if (TranslateMessages* messages =
          web_contents ? TranslateMessages::FromWebContents(web_contents)
                       : nullptr) {
    messages->DismissProgress();
  }
}

}  // namespace compass::translate
