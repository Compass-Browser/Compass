// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/dev_mode/dev_mode_page_info.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/browser/ui/views/controls/rich_controls_container_view.h"
#include "compass/browser/dev_mode/dev_mode.h"
#include "components/prefs/pref_service.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/web_contents.h"
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

// "Development mode" with a switch, laid out as the popup's permission rows
// (RichControlsContainerView): icon, title over a subtitle, the control at
// the end.
class DevModePageInfoRow : public views::View {
  METADATA_HEADER(DevModePageInfoRow, views::View)

 public:
  DevModePageInfoRow(PrefService* prefs, const GURL& url)
      : prefs_(prefs), url_(url) {
    // RichControlsContainerView, as the permission switches use: it measures
    // the subtitle at the width left beside the icon and the switch, so a
    // subtitle that wraps grows the popup instead of cropping its bottom.
    SetUseDefaultFillLayout(true);
    auto* row = AddChildView(std::make_unique<RichControlsContainerView>());
    row->SetIcon(ui::ImageModel::FromVectorIcon(vector_icons::kCodeIcon,
                                                ui::kColorIcon, kIconSize));
    row->SetTitle(u"Development mode");
    subtitle_ = row->AddSecondaryLabel(std::u16string());

    auto toggle = std::make_unique<views::ToggleButton>(base::BindRepeating(
        &DevModePageInfoRow::OnToggled, base::Unretained(this)));
    toggle->SetPreferredSize(gfx::Size(toggle->GetPreferredSize().width(),
                                       row->GetFirstLineHeight()));
    toggle->SetProperty(
        views::kMarginsKey,
        gfx::Insets::TLBR(0,
                          ChromeLayoutProvider::Get()->GetDistanceMetric(
                              views::DISTANCE_RELATED_LABEL_HORIZONTAL),
                          0, 0));
    toggle_ = row->AddControl(std::move(toggle));
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
