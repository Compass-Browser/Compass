// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The script Chromium injects into a page to translate it is Compass's, not
// Google's element.js (docs/design/translation.md, "The script"): the
// fetcher TranslateScript asks hands back page_translator.js and
// desktop_service.js at once. Upstream's prelude and translate.js stay, and
// nothing goes to the network.

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "components/translate/core/browser/translate_url_fetcher.h"
// Declares TranslateURLFetcherImpl under its own name, before the renaming.
#include "net/http/http_request_headers.h"

#if !BUILDFLAG(IS_IOS) && !BUILDFLAG(IS_ANDROID)
#include "compass/browser/translate/page_translation.h"

namespace translate {
namespace {

class CompassTranslateScriptFetcher : public TranslateUrlFetcher {
 public:
  explicit CompassTranslateScriptFetcher(const net::HttpRequestHeaders&) {}

  bool Request(const GURL& url, Callback callback, bool is_incognito) override {
    state_ = REQUESTING;
    // Later, not now: TranslateScript deletes its fetcher in the callback,
    // which mustn't happen inside this call.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<CompassTranslateScriptFetcher> self,
                          Callback callback) {
                         if (self) {
                           self->state_ = COMPLETED;
                         }
                         std::move(callback).Run(
                             true, compass::translate::PageTranslateScript());
                       },
                       weak_factory_.GetWeakPtr(), std::move(callback)));
    return true;
  }

  State state() const override { return state_; }

 private:
  State state_ = IDLE;
  base::WeakPtrFactory<CompassTranslateScriptFetcher> weak_factory_{this};
};

}  // namespace
}  // namespace translate

#define TranslateURLFetcherImpl CompassTranslateScriptFetcher
#endif  // !BUILDFLAG(IS_IOS) && !BUILDFLAG(IS_ANDROID)

#include "src/components/translate/core/browser/translate_script.cc"

#undef TranslateURLFetcherImpl
