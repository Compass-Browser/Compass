// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_TRANSLATE_PAGE_TRANSLATOR_HOST_H_
#define COMPASS_BROWSER_TRANSLATE_PAGE_TRANSLATOR_HOST_H_

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/self_deleting.h"
#include "base/memory/weak_ptr.h"
#include "compass/common/translate/page_translator.mojom.h"
#include "components/on_device_translation/public/mojom/translator.mojom.h"
#include "content/public/browser/document_service.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace compass::translate {

// The browser's end of compassTranslate.send() (renderer/translate/
// page_translator_bindings.cc), one per document: makes sure the languages'
// packs are installed (asking the user first, once for the browser), then
// translates on the device through upstream's on-device translation service,
// HTML behind kHtmlMarker (docs/design/translation.md, "The engine from the
// page").
class PageTranslatorHost
    : public content::DocumentService<mojom::PageTranslator> {
 public:
  static void Create(content::RenderFrameHost* render_frame_host,
                     mojo::PendingReceiver<mojom::PageTranslator> receiver);

  PageTranslatorHost(const PageTranslatorHost&) = delete;
  PageTranslatorHost& operator=(const PageTranslatorHost&) = delete;

  // mojom::PageTranslator:
  void Translate(const std::string& source_language,
                 const std::string& target_language,
                 std::vector<mojom::TranslationSegmentPtr> segments,
                 TranslateCallback callback) override;

 private:
  using Pair = std::pair<std::string, std::string>;
  struct Request;

  friend class base::internal::MakeSelfDeletingImpl;

  PageTranslatorHost(content::RenderFrameHost& render_frame_host,
                     mojo::PendingReceiver<mojom::PageTranslator> receiver,
                     base::SelfDeletingPassKey key);
  ~PageTranslatorHost() override;

  void EnsurePacks(std::unique_ptr<Request> request);
  void OnConsent(std::unique_ptr<Request> request, bool allowed);
  void InstallNext(std::unique_ptr<Request> request);
  void OnPackInstalled(std::unique_ptr<Request> request, bool ok);
  void WithTranslator(std::unique_ptr<Request> request);
  void TranslateSegments(std::unique_ptr<Request> request);

  std::map<Pair, mojo::Remote<on_device_translation::mojom::OnDeviceTranslator>>
      translators_;
  base::WeakPtrFactory<PageTranslatorHost> weak_factory_{this};
};

// Asks the user whether to download translation languages (docs/design/
// translation.md, "Asking first"). Defined by the UI layer
// (browser/ui/translate/); runs `callback` with the answer, false when
// nothing can be shown.
void AskForModelDownloads(content::WebContents* web_contents,
                          base::OnceCallback<void(bool)> callback);

}  // namespace compass::translate

#endif  // COMPASS_BROWSER_TRANSLATE_PAGE_TRANSLATOR_HOST_H_
