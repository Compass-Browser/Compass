// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/sidebar/sidebar_hover_card.h"

#include <memory>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/browser/ui/views/sidebar/tab_preview_view.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/tabs/hovercard/hover_card_anchor_target.h"
#include "chrome/browser/ui/views/tabs/hovercard/tab_hover_card_bubble_view.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/bubble/bubble_anchor.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/widget/widget.h"

namespace compass {

namespace {

// Upstream's rule (TabHoverCardController::AreHoverCardImagesEnabled()): the
// feature, which is off by default on macOS, and the user's setting. Without
// the feature, tabs have no ThumbnailTabHelper, so no images.
bool ArePreviewsEnabled() {
  return base::FeatureList::IsEnabled(features::kTabHoverCardImages) &&
         g_browser_process->local_state()->GetBoolean(
             prefs::kHoverCardImagesEnabled);
}

}  // namespace

// The hover card's anchor: an empty view the width of the sidebar, over the
// hovered row, so the card opens to the sidebar's right.
class SidebarHoverCardTarget : public views::View,
                               public HoverCardAnchorTarget {
  METADATA_HEADER(SidebarHoverCardTarget, views::View)

 public:
  SidebarHoverCardTarget() : HoverCardAnchorTarget(this) {
    SetCanProcessEventsWithinSubtree(false);
  }

  void SetData(const tabs::TabData& data, bool show_preview) {
    SetHoverCardDataFrom(data);
    show_preview_ = show_preview;
  }

  // HoverCardAnchorTarget:
  bool NeedsToShowThumbnail() const override { return show_preview_; }
  bool IsValidHoverCardTarget() const override { return true; }
  views::BubbleAnchor GetAnchor() override { return views::BubbleAnchor(this); }
  views::BubbleBorder::Arrow GetAnchorPosition() const override {
    return views::BubbleBorder::Arrow::LEFT_TOP;
  }

 private:
  bool show_preview_ = false;
};

BEGIN_METADATA(SidebarHoverCardTarget)
END_METADATA

SidebarHoverCard::SidebarHoverCard(content::WebContents* contents,
                                   views::View* sidebar)
    : content::WebContentsUserData<SidebarHoverCard>(*contents),
      sidebar_(sidebar) {
  thumbnail_subscription_ = thumbnail_observer_.AddCallback(
      base::BindRepeating(&SidebarHoverCard::OnPreviewImageAvailable,
                          base::Unretained(this)));
}

SidebarHoverCard::~SidebarHoverCard() {
  // The sidebar destroys its WebContents, and with it this, before its child
  // views.
  Hide();
}

void SidebarHoverCard::Show(const tabs::TabData& data,
                            int row_top,
                            int row_height,
                            bool show_preview,
                            content::WebContents* tab_contents,
                            bool is_active_tab) {
  if (!target_) {
    target_ =
        sidebar_->AddChildView(std::make_unique<SidebarHoverCardTarget>());
  }
  // A moved anchor moves the card.
  target_->SetBounds(0, row_top, sidebar_->width(), row_height);
  ++plugin_request_;

  // A plugin that previews the page shows it where the screenshot goes
  // (docs/design/live-folders.md, "Tab previews").
  LiveFolderService* service = LiveFolderService::Get(
      Profile::FromBrowserContext(GetWebContents().GetBrowserContext()));
  const GURL& url = data.last_committed_url.is_valid() ? data.last_committed_url
                                                       : data.visible_url;
  if (service && !is_active_tab && service->previews().FindPlugin(url)) {
    target_->SetData(data, /*show_preview=*/false);
    ShowThumbnail(data, /*show_preview=*/false);
    TabPreviewService& previews = service->previews();
    if (std::optional<TabPreview> cached = previews.GetCached(url)) {
      SetPluginPreview(
          std::make_unique<TabPreviewView>(*cached, base::Time::Now()));
      return;
    }
    SetPluginPreview(std::make_unique<TabPreviewView>());
    previews.Fetch(tab_contents, url,
                   base::BindOnce(&SidebarHoverCard::OnPluginPreview,
                                  weak_factory_.GetWeakPtr(), plugin_request_,
                                  data, show_preview));
    return;
  }
  SetPluginPreview(nullptr);
  ShowThumbnail(data, show_preview);
}

void SidebarHoverCard::OnPluginPreview(int request,
                                       tabs::TabData data,
                                       bool show_preview,
                                       std::optional<TabPreview> preview) {
  // The card moved on to another row, or closed.
  if (request != plugin_request_ || !bubble_ ||
      bubble_->GetWidget()->IsClosed()) {
    return;
  }
  if (preview) {
    SetPluginPreview(
        std::make_unique<TabPreviewView>(*preview, base::Time::Now()));
    return;
  }
  // Nothing to show: the screenshot, as without a plugin.
  SetPluginPreview(nullptr);
  ShowThumbnail(data, show_preview);
}

void SidebarHoverCard::SetPluginPreview(std::unique_ptr<views::View> view) {
  if (!bubble_ || bubble_->GetWidget()->IsClosed() ||
      (!view && !has_plugin_preview_)) {
    return;
  }
  has_plugin_preview_ = !!view;
  bubble_->SetCustomPreview(std::move(view));
  bubble_->SizeToContents();
}

void SidebarHoverCard::ShowThumbnail(const tabs::TabData& data,
                                     bool show_preview) {
  // Only a card made with previews has room for one.
  const bool previews_enabled = bubble_ && !bubble_->GetWidget()->IsClosed()
                                    ? bubble_has_previews_
                                    : ArePreviewsEnabled();
  show_preview = show_preview && previews_enabled && data.thumbnail;
  target_->SetData(data, show_preview);

  // Close is asynchronous; a closing card can't be reused.
  if (bubble_ && bubble_->GetWidget()->IsClosed()) {
    bubble_observation_.Reset();
    bubble_ = nullptr;
  }
  const bool create = !bubble_;
  if (create) {
    has_plugin_preview_ = false;
    TabHoverCardBubbleView::InitParams params;
    params.use_animation = false;
    params.show_memory_usage = false;
    params.show_image_preview = previews_enabled;
    bubble_has_previews_ = previews_enabled;
    bubble_ = new TabHoverCardBubbleView(target_, params);
    bubble_observation_.Observe(bubble_);
  }
  // A new card starts empty, as upstream's controller knows. Titles wrap
  // differently, so the card's height follows its content.
  bubble_->UpdateCardContent(target_);
  bubble_->SizeToContents();
  if (create) {
    bubble_->GetWidget()->ShowInactive();
  }

  const scoped_refptr<ThumbnailImage>& thumbnail = data.thumbnail;
  if (!show_preview || !thumbnail) {
    thumbnail_observer_.Observe(nullptr);
    return;
  }
  if (thumbnail != thumbnail_observer_.current_image()) {
    // Until the new tab's image arrives.
    bubble_->SetPlaceholderImage();
    thumbnail_observer_.Observe(thumbnail);
  }
}

void SidebarHoverCard::Hide() {
  thumbnail_observer_.Observe(nullptr);
  ++plugin_request_;
  has_plugin_preview_ = false;
  if (!bubble_) {
    return;
  }
  views::Widget* widget = bubble_->GetWidget();
  bubble_observation_.Reset();
  bubble_ = nullptr;
  widget->Close();
}

void SidebarHoverCard::OnViewIsDeleting(views::View* observed_view) {
  bubble_observation_.Reset();
  bubble_ = nullptr;
  has_plugin_preview_ = false;
  thumbnail_observer_.Observe(nullptr);
}

void SidebarHoverCard::OnPreviewImageAvailable(
    TabHoverCardThumbnailObserver* observer,
    gfx::ImageSkia preview) {
  if (bubble_) {
    bubble_->SetTargetTabImage(preview);
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(SidebarHoverCard);

}  // namespace compass
