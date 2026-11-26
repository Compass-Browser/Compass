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
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/browser/ui/views/controls/rich_controls_container_view.h"
#include "compass/browser/adblock/adblock_prefs.h"
#include "compass/browser/adblock/page_activation_throttle.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/prefs/pref_service.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/toggle_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/view_class_properties.h"
#include "url/gurl.h"

namespace compass {

namespace {

constexpr int kIconSize = 20;

// "Ad blocker" with a switch, laid out as the popup's permission rows
// (RichControlsContainerView) and development mode's: icon, title over a
// subtitle, the control at the end. Turning it off allows ads on the site (its
// Ads site setting) and reloads the page.
class AdBlockPageInfoRow : public views::View {
  METADATA_HEADER(AdBlockPageInfoRow, views::View)

 public:
  AdBlockPageInfoRow(content::WebContents* contents,
                     HostContentSettingsMap* settings,
                     const GURL& url)
      : contents_(contents->GetWeakPtr()), settings_(settings), url_(url) {
    // RichControlsContainerView, as the permission switches use: it measures
    // the subtitle at the width left beside the icon and the switch, so a
    // subtitle that wraps grows the popup instead of cropping its bottom.
    SetUseDefaultFillLayout(true);
    auto* row = AddChildView(std::make_unique<RichControlsContainerView>());
    row->SetIcon(ui::ImageModel::FromVectorIcon(vector_icons::kAdOffIcon,
                                                ui::kColorIcon, kIconSize));
    row->SetTitle(
        l10n_util::GetStringUTF16(IDS_COMPASS_ADBLOCK_PAGE_INFO_TITLE));
    subtitle_ = row->AddSecondaryLabel(std::u16string());

    auto toggle = std::make_unique<views::ToggleButton>(base::BindRepeating(
        &AdBlockPageInfoRow::OnToggled, base::Unretained(this)));
    toggle->SetPreferredSize(gfx::Size(toggle->GetPreferredSize().width(),
                                       row->GetFirstLineHeight()));
    toggle->SetProperty(
        views::kMarginsKey,
        gfx::Insets::TLBR(0,
                          ChromeLayoutProvider::Get()->GetDistanceMetric(
                              views::DISTANCE_RELATED_LABEL_HORIZONTAL),
                          0, 0));
    toggle_ = row->AddControl(std::move(toggle));
    toggle_->GetViewAccessibility().SetName(
        l10n_util::GetStringUTF16(IDS_COMPASS_ADBLOCK_PAGE_INFO_TITLE));
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
    subtitle_->SetText(
        l10n_util::GetStringUTF16(on ? IDS_COMPASS_ADBLOCK_PAGE_INFO_ON
                                     : IDS_COMPASS_ADBLOCK_PAGE_INFO_OFF));
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
      !g_browser_process->local_state()->GetBoolean(adblock::prefs::kEnabled)) {
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
