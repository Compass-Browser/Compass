// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_RENDERER_LEGACY_EME_H_
#define COMPASS_MEDIA_AVF_RENDERER_LEGACY_EME_H_

namespace blink {
class WebLocalFrame;
}

namespace compass::avf {

// Safari's legacy prefixed EME (WebKitMediaKeys, "webkitneedkey"), built on
// the standard com.apple.fps (legacy_eme.js, docs/design/fairplay.md
// "Legacy API"). Called by ChromeContentRendererClient::
// RunScriptsAtDocumentStart() (patches/chrome-renderer-chrome_content_
// renderer_client.cc.patch) before the page's scripts run; adds the API to
// `frame`'s main world if FairPlay is on and the frame presents as Safari.
// Runs script: `frame` may be gone afterwards.
void MaybeInjectLegacyEme(blink::WebLocalFrame* frame);

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_RENDERER_LEGACY_EME_H_
