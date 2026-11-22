// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/adblock/adblock_page_info.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "compass/browser/adblock/adblock_prefs.h"
#include "compass/browser/adblock/page_activation_throttle.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/prefs/pref_service.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/toggle_button.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"
#include "url/gurl.h"

namespace compass {

namespace {

constexpr int kIconSize = 20;

// "Ad blocker" with a switch, laid out as the popup's other rows
// (RichHoverButton) and development mode's: icon, title over a subtitle,
// the control at the end. Turning it off allows ads on the site (its Ads
// site setting) and reloads the page.
class AdBlockPageInfoRow : public views::View {
  METADATA_HEADER(AdBlockPageInfoRow, views::View)

 public:
  AdBlockPageInfoRow(content::WebContents* contents,
                     HostContentSettingsMap* settings,
                     const GURL& url)
      : contents_(contents->GetWeakPtr()), settings_(settings), url_(url) {
    ChromeLayoutProvider* layout = ChromeLayoutProvider::Get();
    SetBorder(views::CreateEmptyBorder(
        layout->GetInsetsMetric(INSETS_PAGE_INFO_HOVER_BUTTON)));
    auto* box = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
        layout->GetDistanceMetric(DISTANCE_RICH_HOVER_BUTTON_ICON_HORIZONTAL)));
    box->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    AddChildView(std::make_unique<views::ImageView>(
        ui::ImageModel::FromVectorIcon(vector_icons::kAdOffIcon,
                                       ui::kColorIcon, kIconSize)));

    auto* labels = AddChildView(std::make_unique<views::View>());
    labels->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical));
    box->SetFlexForView(labels, 1);
    auto* title = labels->AddChildView(std::make_unique<views::Label>(
        u"Ad blocker", views::style::CONTEXT_LABEL,
        views::style::STYLE_BODY_3_MEDIUM));
    title->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    title->SetEnabledColor(kColorPageInfoForeground);
    subtitle_ = labels->AddChildView(std::make_unique<views::Label>(
        std::u16string(), views::style::CONTEXT_LABEL,
        views::style::STYLE_BODY_4));
    subtitle_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    subtitle_->SetEnabledColor(kColorPageInfoSubtitleForeground);
    subtitle_->SetMultiLine(true);

    toggle_ = AddChildView(std::make_unique<views::ToggleButton>(
        base::BindRepeating(&AdBlockPageInfoRow::OnToggled,
                            base::Unretained(this))));
    toggle_->GetViewAccessibility().SetName(u"Ad blocker");
    Update();
  }

 private:
  void OnToggled() {
    adblock::SetAdsAllowedOn(settings_, url_, !toggle_->GetIsOn());
    Update();
    // The page's requests went out with the old choice.
    if (contents_) {
      contents_->GetController().Reload(content::ReloadType::NORMAL,
                                        /*check_for_repost=*/true);
    }
  }

  void Update() {
    const bool on = !adblock::AdsAllowedOn(settings_, url_);
    toggle_->SetIsOn(on);
    subtitle_->SetText(on ? u"Blocking ads and trackers on this site"
                          : u"Off for this site");
  }

  base::WeakPtr<content::WebContents> contents_;
  raw_ptr<HostContentSettingsMap> settings_;
  const GURL url_;
  raw_ptr<views::Label> subtitle_ = nullptr;
  raw_ptr<views::ToggleButton> toggle_ = nullptr;
};

BEGIN_METADATA(AdBlockPageInfoRow)
END_METADATA

}  // namespace

void AddAdBlockPageInfoRow(views::View* container,
                           content::WebContents* contents) {
  if (!container || !contents) {
    return;
  }
  const GURL& url = contents->GetLastCommittedURL();
  if (!url.SchemeIsHTTPOrHTTPS() ||
      !g_browser_process->local_state()->GetBoolean(
          adblock::prefs::kEnabled)) {
    return;
  }
  HostContentSettingsMap* settings =
      HostContentSettingsMapFactory::GetForProfile(
          Profile::FromBrowserContext(contents->GetBrowserContext()));
  if (!settings) {
    return;
  }
  container->AddChildView(
      std::make_unique<AdBlockPageInfoRow>(contents, settings, url));
}

}  // namespace compass
