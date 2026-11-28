// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/translate/page_translator_host.h"

#include <set>
#include <utility>

#include "base/barrier_callback.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/on_device_translation/service_controller_manager_factory.h"
#include "compass/browser/translate/page_translation.h"
#include "compass/browser/translate/translation_pack_installer.h"
#include "compass/components/translatekit/html_marker.h"
#include "components/on_device_translation/component_manager.h"
#include "components/on_device_translation/public/language_pack.h"
#include "components/on_device_translation/public/supported_languages.h"
#include "components/on_device_translation/service_controller_manager.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace compass::translate {

namespace {

using on_device_translation::ComponentManager;
using on_device_translation::LanguagePackKey;
using on_device_translation::OnDeviceTranslationController;

// Page translation's own service, shared by every page of a profile, so a
// language's models load once (upstream starts one per origin).
url::Origin TranslationOrigin() {
  return url::Origin::Create(GURL("chrome://compass-translate"));
}

std::optional<LanguagePackKey> PackFor(const std::string& language) {
  std::optional<on_device_translation::SupportedLanguage> supported =
      on_device_translation::ToSupportedLanguage(language);
  if (!supported ||
      *supported == on_device_translation::SupportedLanguage::kEn) {
    return std::nullopt;
  }
  for (const auto& [key, config] :
       on_device_translation::kLanguagePackComponentConfigMap) {
    if (on_device_translation::NonEnglishSupportedLanguageFromLanguagePackKey(
            key) == *supported) {
      return key;
    }
  }
  return std::nullopt;
}

}  // namespace

struct PageTranslatorHost::Request {
  Pair pair;
  std::vector<mojom::TranslationSegmentPtr> segments;
  TranslateCallback callback;
  std::vector<LanguagePackKey> missing;

  void Fail(mojom::PageTranslationResult result) {
    std::move(callback).Run(result, {});
  }
};

// static
void PageTranslatorHost::Create(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<mojom::PageTranslator> receiver) {
  // Web pages only: not the browser's own.
  const GURL& url = render_frame_host->GetLastCommittedURL();
  if (!url.SchemeIsHTTPOrHTTPS() && !url.SchemeIsFile()) {
    return;
  }
  base::MakeSelfDeleting<PageTranslatorHost>(*render_frame_host,
                                             std::move(receiver));
}

PageTranslatorHost::PageTranslatorHost(
    content::RenderFrameHost& render_frame_host,
    mojo::PendingReceiver<mojom::PageTranslator> receiver,
    base::SelfDeletingPassKey key)
    : DocumentService(render_frame_host, std::move(receiver), key) {}

PageTranslatorHost::~PageTranslatorHost() = default;

void PageTranslatorHost::Translate(
    const std::string& source_language,
    const std::string& target_language,
    std::vector<mojom::TranslationSegmentPtr> segments,
    TranslateCallback callback) {
  auto request = std::make_unique<Request>();
  request->callback = std::move(callback);
  std::optional<std::string> source = ToOnDeviceLanguage(source_language);
  std::optional<std::string> target = ToOnDeviceLanguage(target_language);
  if (!source || !target || *source == *target) {
    request->Fail(mojom::PageTranslationResult::kUnsupportedLanguage);
    return;
  }
  request->pair = {*source, *target};
  request->segments = std::move(segments);
  if (translators_.contains(request->pair) &&
      translators_[request->pair].is_connected()) {
    TranslateSegments(std::move(request));
    return;
  }
  EnsurePacks(std::move(request));
}

void PageTranslatorHost::EnsurePacks(std::unique_ptr<Request> request) {
  std::set<LanguagePackKey> installed =
      ComponentManager::GetInstalledLanguagePacks();
  for (const std::string& language :
       {request->pair.first, request->pair.second}) {
    std::optional<LanguagePackKey> key = PackFor(language);
    if (key && !installed.contains(*key)) {
      request->missing.push_back(*key);
    }
  }
  if (request->missing.empty()) {
    WithTranslator(std::move(request));
    return;
  }
  PrefService* local_state = g_browser_process->local_state();
  if (local_state->GetBoolean(kModelDownloadsAllowed)) {
    InstallNext(std::move(request));
    return;
  }
  content::WebContents* web_contents =
      content::WebContents::FromRenderFrameHost(&render_frame_host());
  AskForModelDownloads(
      web_contents,
      base::BindOnce(&PageTranslatorHost::OnConsent, weak_factory_.GetWeakPtr(),
                     std::move(request)));
}

void PageTranslatorHost::OnConsent(std::unique_ptr<Request> request,
                                   bool allowed) {
  if (!allowed) {
    request->Fail(mojom::PageTranslationResult::kDeclined);
    return;
  }
  g_browser_process->local_state()->SetBoolean(kModelDownloadsAllowed, true);
  InstallNext(std::move(request));
}

void PageTranslatorHost::InstallNext(std::unique_ptr<Request> request) {
  if (request->missing.empty()) {
    WithTranslator(std::move(request));
    return;
  }
  const LanguagePackKey key = request->missing.back();
  // The UI shows how far the download is (the desktop's pill, Android's
  // message).
  PackInstaller::Get().OnProgress(
      key, base::BindRepeating(
               [](base::WeakPtr<PageTranslatorHost> self, int64_t received,
                  int64_t total) {
                 if (self) {
                   ShowModelDownloadProgress(
                       content::WebContents::FromRenderFrameHost(
                           &self->render_frame_host()),
                       received, total);
                 }
               },
               weak_factory_.GetWeakPtr()));
  // Through upstream's installer (and so Compass's, chromium_src/.../
  // installer_impl.cc), so upstream's service hears of the new pack.
  PackInstaller::Get().OnInstalled(
      key, base::BindOnce(&PageTranslatorHost::OnPackInstalled,
                          weak_factory_.GetWeakPtr(), std::move(request)));
  ComponentManager::GetInstance().RegisterTranslateKitLanguagePackComponent(
      key);
}

void PageTranslatorHost::OnPackInstalled(std::unique_ptr<Request> request,
                                         bool ok) {
  ModelDownloadFinished(
      content::WebContents::FromRenderFrameHost(&render_frame_host()));
  if (!ok) {
    request->Fail(mojom::PageTranslationResult::kFailed);
    return;
  }
  request->missing.pop_back();
  InstallNext(std::move(request));
}

void PageTranslatorHost::WithTranslator(std::unique_ptr<Request> request) {
  on_device_translation::ServiceControllerManager* manager =
      on_device_translation::ServiceControllerManagerFactory::GetInstance()
          ->Get(render_frame_host().GetBrowserContext());
  if (!manager) {
    request->Fail(mojom::PageTranslationResult::kFailed);
    return;
  }
  const Pair pair = request->pair;
  manager->CreateTranslator(
      TranslationOrigin(), pair.first, pair.second,
      base::BindOnce(
          [](base::WeakPtr<PageTranslatorHost> self,
             std::unique_ptr<Request> request,
             base::expected<
                 mojo::PendingRemote<
                     on_device_translation::mojom::OnDeviceTranslator>,
                 OnDeviceTranslationController::CreateTranslatorError> result) {
            if (!self) {
              return;
            }
            if (!result.has_value()) {
              request->Fail(
                  result.error() ==
                          OnDeviceTranslationController::CreateTranslatorError::
                              kFailedToCreateTranslator
                      ? mojom::PageTranslationResult::kUnsupportedLanguage
                      : mojom::PageTranslationResult::kFailed);
              return;
            }
            self->translators_[request->pair].reset();
            self->translators_[request->pair].Bind(std::move(*result));
            self->TranslateSegments(std::move(request));
          },
          weak_factory_.GetWeakPtr(), std::move(request)));
}

void PageTranslatorHost::TranslateSegments(std::unique_ptr<Request> request) {
  auto& translator = translators_[request->pair];
  const size_t count = request->segments.size();
  if (!count) {
    std::move(request->callback).Run(mojom::PageTranslationResult::kOk, {});
    return;
  }
  TranslateCallback callback = std::move(request->callback);
  auto done =
      base::BarrierCallback<std::pair<size_t, std::optional<std::string>>>(
          count,
          base::BindOnce(
              [](TranslateCallback callback, size_t count,
                 std::vector<std::pair<size_t, std::optional<std::string>>>
                     results) {
                std::vector<std::optional<std::string>> translations(count);
                for (auto& [index, text] : results) {
                  translations[index] = std::move(text);
                }
                std::move(callback).Run(mojom::PageTranslationResult::kOk,
                                        std::move(translations));
              },
              std::move(callback), count));
  for (size_t i = 0; i < count; ++i) {
    const mojom::TranslationSegmentPtr& segment = request->segments[i];
    std::string input =
        segment->html ? base::StrCat({translatekit::kHtmlMarker, segment->text})
                      : segment->text;
    // A service that goes away (idle, or crashed) drops its callbacks: they
    // answer null instead, so the page never waits forever.
    translator->Translate(
        input,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](size_t index,
                   base::OnceCallback<void(
                       std::pair<size_t, std::optional<std::string>>)> done,
                   const std::optional<std::string>& output) {
                  std::move(done).Run({index, output});
                },
                i, done),
            std::nullopt));
  }
}

}  // namespace compass::translate
