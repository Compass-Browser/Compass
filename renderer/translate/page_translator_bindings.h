// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_RENDERER_TRANSLATE_PAGE_TRANSLATOR_BINDINGS_H_
#define COMPASS_RENDERER_TRANSLATE_PAGE_TRANSLATOR_BINDINGS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "compass/common/translate/page_translator.mojom.h"
#include "content/public/renderer/render_frame_observer.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"

namespace gin {
class Arguments;
}

namespace compass::translate {

// compassTranslate.send(source, target, segments, callback) in a frame's
// translate isolated world, and nowhere else: the page translator's engine
// on the desktop (common/translate/desktop_service.js wraps it in a
// promise). It asks the browser's PageTranslator, which translates on the
// device (docs/design/translation.md, "The engine from the page").
class PageTranslatorBindings : public content::RenderFrameObserver {
 public:
  static void Create(content::RenderFrame* render_frame);

  PageTranslatorBindings(const PageTranslatorBindings&) = delete;
  PageTranslatorBindings& operator=(const PageTranslatorBindings&) = delete;

 private:
  explicit PageTranslatorBindings(content::RenderFrame* render_frame);
  ~PageTranslatorBindings() override;

  // content::RenderFrameObserver:
  void DidCreateScriptContext(v8::Local<v8::Context> context,
                              int32_t world_id) override;
  void WillReleaseScriptContext(v8::Local<v8::Context> context,
                                int32_t world_id) override;
  void OnDestruct() override;

  void Send(gin::Arguments* args);
  void OnTranslated(
      v8::Global<v8::Function> callback,
      mojom::PageTranslationResult result,
      const std::vector<std::optional<std::string>>& translations);

  mojom::PageTranslator* translator();

  v8::Global<v8::Context> context_;
  mojo::Remote<mojom::PageTranslator> translator_;
  base::WeakPtrFactory<PageTranslatorBindings> weak_factory_{this};
};

}  // namespace compass::translate

#endif  // COMPASS_RENDERER_TRANSLATE_PAGE_TRANSLATOR_BINDINGS_H_
