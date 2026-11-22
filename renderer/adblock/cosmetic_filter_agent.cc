// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/renderer/adblock/cosmetic_filter_agent.h"

#include <string>
#include <vector>

#include "base/no_destructor.h"
#include "compass/renderer/adblock/cosmetic_css.h"
#include "components/subresource_filter/content/renderer/subresource_filter_agent.h"
#include "components/subresource_filter/content/renderer/unverified_ruleset_dealer.h"
#include "components/subresource_filter/content/renderer/web_document_subresource_filter_impl.h"
#include "components/subresource_filter/core/common/flat/indexed_ruleset_generated.h"
#include "components/subresource_filter/core/common/memory_mapped_ruleset.h"
#include "components/subresource_filter/core/mojom/subresource_filter.mojom.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "url/origin.h"

namespace compass::adblock {

namespace {

// The CSS for every site, built once per ruleset in the renderer process
// (the main thread's): most sites have no exception, so their frames share
// it.
struct GenericCssCache {
  uint64_t ruleset_id = 0;
  scoped_refptr<const subresource_filter::MemoryMappedRuleset> ruleset;
  // Points into `ruleset`'s mapping.
  GenericSelectors selectors;
  blink::WebString css;
};

GenericCssCache& GetCache() {
  static base::NoDestructor<GenericCssCache> cache;
  return *cache;
}

}  // namespace

// static
void CosmeticFilterAgent::Create(
    content::RenderFrame* render_frame,
    subresource_filter::UnverifiedRulesetDealer* dealer) {
  // Deletes itself with the frame (OnDestruct()).
  new CosmeticFilterAgent(render_frame, dealer);
}

CosmeticFilterAgent::CosmeticFilterAgent(
    content::RenderFrame* render_frame,
    subresource_filter::UnverifiedRulesetDealer* dealer)
    : content::RenderFrameObserver(render_frame), dealer_(dealer) {}

CosmeticFilterAgent::~CosmeticFilterAgent() = default;

void CosmeticFilterAgent::OnDestruct() {
  delete this;
}

void CosmeticFilterAgent::DidCreateDocumentElement() {
  auto* subresource_filter_agent =
      subresource_filter::SubresourceFilterAgent::Get(render_frame());
  const subresource_filter::WebDocumentSubresourceFilterImpl* filter =
      subresource_filter_agent
          ? subresource_filter_agent->filter_for_current_document()
          : nullptr;
  if (!filter) {
    return;
  }
  const subresource_filter::mojom::ActivationState& activation =
      filter->activation_state();
  if (activation.activation_level !=
          subresource_filter::mojom::ActivationLevel::kEnabled ||
      activation.filtering_disabled_for_document) {
    return;
  }

  blink::WebDocument document = render_frame()->GetWebFrame()->GetDocument();
  const url::Origin origin = document.GetSecurityOrigin();
  const std::string host = origin.GetTupleOrPrecursorTupleIfOpaque().host();

  std::string css;
  // The rules for every site, from the ruleset the filter uses.
  scoped_refptr<const subresource_filter::MemoryMappedRuleset> ruleset =
      dealer_ ? dealer_->GetRuleset() : nullptr;
  const subresource_filter::flat::StyleRuleIndex* index = nullptr;
  if (ruleset) {
    const auto* root =
        subresource_filter::flat::GetIndexedRuleset(ruleset->data().data());
    if (root && root->ruleset_id() == filter->GetRulesetId()) {
      index = root->style_rule_index();
    }
  }
  blink::WebString generic_css;
  if (index) {
    GenericCssCache& cache = GetCache();
    if (cache.ruleset != ruleset) {
      cache.ruleset = ruleset;
      cache.ruleset_id = filter->GetRulesetId();
      cache.selectors = CollectGenericSelectors(index);
      cache.css =
          blink::WebString::FromUtf8(BuildHidingCss(cache.selectors, {}));
    }
    std::vector<uint16_t> excluded = GetExcludedIndices(index, host);
    generic_css = excluded.empty()
                      ? cache.css
                      : blink::WebString::FromUtf8(
                            BuildHidingCss(cache.selectors, excluded));
  }

  // The rules for the document's site.
  std::vector<std::string_view> domain_selectors;
  filter->filter().GetDomainSelectors(domain_selectors);

  if (!generic_css.IsEmpty()) {
    document.InsertStyleSheet(generic_css, nullptr,
                              blink::WebCssOrigin::kUser);
  }
  if (!domain_selectors.empty()) {
    document.InsertStyleSheet(
        blink::WebString::FromUtf8(BuildHidingCss(domain_selectors)), nullptr,
        blink::WebCssOrigin::kUser);
  }
}

}  // namespace compass::adblock
