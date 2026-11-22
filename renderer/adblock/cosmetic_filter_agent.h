// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_RENDERER_ADBLOCK_COSMETIC_FILTER_AGENT_H_
#define COMPASS_RENDERER_ADBLOCK_COSMETIC_FILTER_AGENT_H_

#include "base/memory/raw_ptr.h"
#include "content/public/renderer/render_frame_observer.h"

namespace subresource_filter {
class UnverifiedRulesetDealer;
}  // namespace subresource_filter

namespace compass::adblock {

// Hides what the ad blocker's element hiding rules match in a frame
// (docs/design/ad-blocker.md "Element hiding"): when upstream's subresource
// filter blocks in the frame's document (not dry run, no "@@...$document"
// exception), a user stylesheet with the rules for every site and the ones
// for the document's site. Created next to upstream's SubresourceFilterAgent
// (patches/chrome-renderer-chrome_content_renderer_client.cc.patch).
class CosmeticFilterAgent : public content::RenderFrameObserver {
 public:
  static void Create(content::RenderFrame* render_frame,
                     subresource_filter::UnverifiedRulesetDealer* dealer);

  CosmeticFilterAgent(const CosmeticFilterAgent&) = delete;
  CosmeticFilterAgent& operator=(const CosmeticFilterAgent&) = delete;

 private:
  CosmeticFilterAgent(content::RenderFrame* render_frame,
                      subresource_filter::UnverifiedRulesetDealer* dealer);
  ~CosmeticFilterAgent() override;

  // content::RenderFrameObserver:
  void DidCreateDocumentElement() override;
  void OnDestruct() override;

  raw_ptr<subresource_filter::UnverifiedRulesetDealer> dealer_;
};

}  // namespace compass::adblock

#endif  // COMPASS_RENDERER_ADBLOCK_COSMETIC_FILTER_AGENT_H_
