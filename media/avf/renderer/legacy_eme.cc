// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/renderer/legacy_eme.h"

#include "compass/media/avf/common/avf_features.h"
#include "compass/media/avf/renderer/legacy_eme_js.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_local_frame_client.h"
#include "third_party/blink/public/web/web_script_source.h"

namespace compass::avf {

void MaybeInjectLegacyEme(blink::WebLocalFrame* frame) {
  if (!frame || !IsFairPlayEnabled() || !frame->Client()) {
    return;
  }
  // The UA override the frame's navigation committed with: Present as
  // Safari's, for sites the user turned it on for.
  if (!IsSafariUserAgent(frame->Client()->UserAgentOverride().Utf8())) {
    return;
  }
  frame->ExecuteScript(blink::WebScriptSource(
      blink::WebString::FromUtf8(assets::kLegacyEmeJs)));
}

}  // namespace compass::avf
