// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/dev_mode/dev_mode_page_info.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "compass/browser/dev_mode/dev_mode.h"
#include "components/prefs/pref_service.h"
#include "components/vector_icons/vector_icons.h"
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

// "Development mode" with a switch, laid out as the popup's other rows
// (RichHoverButton): icon, title over a subtitle, the control at the end.
class DevModePageInfoRow : public views::View {
  METADATA_HEADER(DevModePageInfoRow, views::View)

 public:
  DevModePageInfoRow(PrefService* prefs, const GURL& url)
      : prefs_(prefs), url_(url) {
    ChromeLayoutProvider* layout = ChromeLayoutProvider::Get();
    SetBorder(views::CreateEmptyBorder(
        layout->GetInsetsMetric(INSETS_PAGE_INFO_HOVER_BUTTON)));
    auto* box = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
        layout->GetDistanceMetric(DISTANCE_RICH_HOVER_BUTTON_ICON_HORIZONTAL)));
    box->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    AddChildView(std::make_unique<views::ImageView>(
        ui::ImageModel::FromVectorIcon(vector_icons::kCodeIcon,
                                       ui::kColorIcon, kIconSize)));

    auto* labels = AddChildView(std::make_unique<views::View>());
    labels->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical));
    box->SetFlexForView(labels, 1);
    auto* title = labels->AddChildView(std::make_unique<views::Label>(
        u"Development mode", views::style::CONTEXT_LABEL,
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
        base::BindRepeating(&DevModePageInfoRow::OnToggled,
                            base::Unretained(this))));
    toggle_->GetViewAccessibility().SetName(u"Development mode");
    Update();
  }

 private:
  void OnToggled() {
    dev_mode::SetEnabledFor(prefs_, url_, toggle_->GetIsOn());
    Update();
  }

  void Update() {
    const bool on = dev_mode::IsEnabledFor(prefs_, url_);
    toggle_->SetIsOn(on);
    subtitle_->SetText(on ? u"Reload skips the cache; DevTools bar is shown"
                          : u"Tools for building this site");
  }

  raw_ptr<PrefService> prefs_;
  const GURL url_;
  raw_ptr<views::Label> subtitle_ = nullptr;
  raw_ptr<views::ToggleButton> toggle_ = nullptr;
};

BEGIN_METADATA(DevModePageInfoRow)
END_METADATA

}  // namespace

void AddDevModePageInfoRow(views::View* container,
                           content::WebContents* contents) {
  if (!container || !contents) {
    return;
  }
  const GURL& url = contents->GetLastCommittedURL();
  if (!dev_mode::CanEnable(url)) {
    return;
  }
  container->AddChildView(std::make_unique<DevModePageInfoRow>(
      Profile::FromBrowserContext(contents->GetBrowserContext())->GetPrefs(),
      url));
}

}  // namespace compass
