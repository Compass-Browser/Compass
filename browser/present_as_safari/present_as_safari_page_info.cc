// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/present_as_safari/present_as_safari_page_info.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/browser/ui/views/controls/rich_controls_container_view.h"
#include "compass/browser/present_as_safari/present_as_safari.h"
#include "components/prefs/pref_service.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
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
constexpr char16_t kTitle[] = u"Present as Safari";

// The switch, laid out as the popup's permission rows (and development
// mode's): icon, title over a subtitle, the switch at the end.
class PresentAsSafariPageInfoRow : public views::View {
  METADATA_HEADER(PresentAsSafariPageInfoRow, views::View)

 public:
  explicit PresentAsSafariPageInfoRow(content::WebContents* contents)
      : contents_(contents->GetWeakPtr()),
        prefs_(Profile::FromBrowserContext(contents->GetBrowserContext())
                   ->GetPrefs()),
        url_(contents->GetLastCommittedURL()) {
    SetUseDefaultFillLayout(true);
    auto* row = AddChildView(std::make_unique<RichControlsContainerView>());
    row->SetIcon(ui::ImageModel::FromVectorIcon(vector_icons::kDevicesIcon,
                                                ui::kColorIcon, kIconSize));
    row->SetTitle(kTitle);
    subtitle_ = row->AddSecondaryLabel(std::u16string());

    auto toggle = std::make_unique<views::ToggleButton>(base::BindRepeating(
        &PresentAsSafariPageInfoRow::OnToggled, base::Unretained(this)));
    toggle->SetPreferredSize(gfx::Size(toggle->GetPreferredSize().width(),
                                       row->GetFirstLineHeight()));
    toggle->SetProperty(
        views::kMarginsKey,
        gfx::Insets::TLBR(0,
                          ChromeLayoutProvider::Get()->GetDistanceMetric(
                              views::DISTANCE_RELATED_LABEL_HORIZONTAL),
                          0, 0));
    toggle_ = row->AddControl(std::move(toggle));
    toggle_->GetViewAccessibility().SetName(kTitle);
    Update();
  }

 private:
  void OnToggled() {
    const bool on = toggle_->GetIsOn();
    present_as_safari::SetEnabledFor(prefs_, url_, on);
    Update();
    // Load the page again with the other user agent.
    content::WebContents* contents = contents_.get();
    if (!contents) {
      return;
    }
    if (on) {
      contents->SetUserAgentOverride(
          blink::UserAgentOverride::UserAgentOnly(
              present_as_safari::kSafariUserAgent),
          /*override_in_new_tabs=*/false);
    }
    content::NavigationController& controller = contents->GetController();
    if (content::NavigationEntry* entry = controller.GetLastCommittedEntry()) {
      entry->SetIsOverridingUserAgent(on);
    }
    controller.Reload(content::ReloadType::NORMAL,
                      /*check_for_repost=*/true);
  }

  void Update() {
    const bool on = present_as_safari::IsEnabledFor(prefs_, url_);
    toggle_->SetIsOn(on);
    subtitle_->SetText(
        on ? u"This site sees Safari and can use Apple's FairPlay video"
           : u"For video that plays only in Safari");
  }

  base::WeakPtr<content::WebContents> contents_;
  raw_ptr<PrefService> prefs_;
  const GURL url_;
  raw_ptr<views::Label> subtitle_ = nullptr;
  raw_ptr<views::ToggleButton> toggle_ = nullptr;
};

BEGIN_METADATA(PresentAsSafariPageInfoRow)
END_METADATA

}  // namespace

void AddPresentAsSafariPageInfoRow(views::View* container,
                                   content::WebContents* contents) {
  if (!container || !contents || !present_as_safari::IsAvailable() ||
      !present_as_safari::SiteFor(contents->GetLastCommittedURL())) {
    return;
  }
  container->AddChildView(
      std::make_unique<PresentAsSafariPageInfoRow>(contents));
}

}  // namespace compass
