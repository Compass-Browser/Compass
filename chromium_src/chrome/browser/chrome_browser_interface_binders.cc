// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Upstream's document-scoped binders, plus Compass's:
// compass.mojom.PageTranslator, page translation's engine for a frame's
// translate world (docs/design/translation.md, "The engine from the page").

// Declared first, under its own name, before the renaming below.
#include "chrome/browser/chrome_browser_interface_binders.h"

#define PopulateChromeFrameBinders PopulateChromeFrameBinders_ChromiumImpl
#include "src/chrome/browser/chrome_browser_interface_binders.cc"
#undef PopulateChromeFrameBinders

#include "components/on_device_translation/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_ON_DEVICE_TRANSLATION)
#include "compass/browser/translate/page_translator_host.h"
#endif

namespace chrome::internal {

void PopulateChromeFrameBinders(
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map,
    content::RenderFrameHost* render_frame_host) {
  PopulateChromeFrameBinders_ChromiumImpl(map, render_frame_host);
#if BUILDFLAG(ENABLE_ON_DEVICE_TRANSLATION)
  map->Add<compass::mojom::PageTranslator>(
      &compass::translate::PageTranslatorHost::Create);
#endif
}

}  // namespace chrome::internal
