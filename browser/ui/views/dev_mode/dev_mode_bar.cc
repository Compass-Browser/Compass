// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/dev_mode/dev_mode_bar.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "cc/paint/paint_flags.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/devtools/devtools_toggle_action.h"
#include "chrome/browser/devtools/devtools_window.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "compass/browser/dev_mode/devtools_frontend_hook.h"
#include "compass/browser/ui/views/frame/content_frame.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/devtools_manager_delegate.h"
#include "components/url_formatter/url_fixer.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/range/range.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/base/page_transition_types.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/views/controls/textfield/textfield.h"
#include "url/url_constants.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view_utils.h"

namespace compass {

namespace {

constexpr int kIconSize = 16;
constexpr int kButtonSpacing = 2;
constexpr gfx::Insets kInsets = gfx::Insets::TLBR(0, 10, 0, 6);
// How long the copy button shows its check.
constexpr base::TimeDelta kCopiedDuration = base::Milliseconds(1500);

// Opens DevTools for |contents| on |panel|. A window that's already open
// only comes to the front, so its frontend is told to switch.
void OpenPanel(content::WebContents* contents,
               const DevToolsToggleAction& action,
               DevToolsOpenedByAction opened_by,
               const char* panel) {
  const bool was_open =
      DevToolsWindow::GetInstanceForInspectedWebContents(contents) != nullptr;
  if (was_open) {
    DevToolsWindow::OpenDevToolsWindow(contents, action, opened_by);
    dev_mode::ShowDevToolsPanel(contents, panel);
    return;
  }
  if (action.type() == DevToolsToggleAction::kShow) {
    // Opens straight on |panel|.
    DevToolsWindow::OpenDevToolsWindow(
        contents, /*profile=*/nullptr, opened_by,
        content::DevToolsManagerDelegate::DevToolsOptions(std::string(panel)));
    return;
  }
  DevToolsWindow::OpenDevToolsWindow(contents, action, opened_by);
}

}  // namespace

DevModeBar::DevModeBar(ContentsContainerView* container)
    : container_(container) {
  auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal, kInsets, kButtonSpacing));
  layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  // The full URL, which can be edited: Enter goes there in this pane, Esc
  // or leaving the field puts the page's back.
  auto url = std::make_unique<views::Textfield>();
  url->SetBorder(views::NullBorder());
  url->SetBackgroundEnabled(false);
  url->SetTextColorId(ui::kColorSysOnSurface);
  url->SetTextInputType(ui::TEXT_INPUT_TYPE_URL);
  url->GetViewAccessibility().SetName(u"Page address");
  url->set_controller(this);
  // Plain text on the bar: no hover wash or focus ring, which would draw a
  // box around most of it.
  url->RemoveHoverEffect();
  url_ = AddChildView(std::move(url));
  views::FocusRing::Remove(url_);
  layout->SetFlexForView(url_, 1);
  url_observation_.Observe(url_);

  const auto add_button = [this](void (DevModeBar::*action)(),
                                 std::u16string tooltip) {
    auto button = views::CreateVectorImageButton(
        base::BindRepeating(action, base::Unretained(this)));
    button->SetTooltipText(tooltip);
    button->GetViewAccessibility().SetName(tooltip);
    return AddChildView(std::move(button));
  };
  console_ = add_button(&DevModeBar::OpenConsole, u"Open console");
  network_ = add_button(&DevModeBar::OpenNetwork, u"Open network panel");
  inspect_ = add_button(&DevModeBar::InspectElement, u"Inspect an element");
  copy_ = add_button(&DevModeBar::CopyUrl, u"Copy URL");

  container_observation_.Observe(container_);
  PlaceInContainer();
}

DevModeBar::~DevModeBar() = default;

// static
DevModeBar* DevModeBar::FindIn(const views::View* container) {
  for (views::View* child : container->children()) {
    if (auto* bar = views::AsViewClass<DevModeBar>(child)) {
      return bar;
    }
  }
  return nullptr;
}

void DevModeBar::SetWebContents(content::WebContents* contents) {
  if (contents == web_contents()) {
    UpdateUrl();
    return;
  }
  Observe(contents);
  ShowCopied(false);
  UpdateUrl();
}

void DevModeBar::OpenConsole() {
  if (content::WebContents* contents = web_contents()) {
    OpenPanel(contents, DevToolsToggleAction::ShowConsolePanel(),
              DevToolsOpenedByAction::kConsoleShortcut, "console");
  }
}

void DevModeBar::OpenNetwork() {
  if (content::WebContents* contents = web_contents()) {
    OpenPanel(contents, DevToolsToggleAction::Show(),
              DevToolsOpenedByAction::kMainMenuOrMainShortcut, "network");
  }
}

void DevModeBar::InspectElement() {
  // Opens DevTools if needed and turns on picking: the element clicked next
  // shows in the Elements panel.
  if (content::WebContents* contents = web_contents()) {
    DevToolsWindow::OpenDevToolsWindow(
        contents, DevToolsToggleAction::Inspect(),
        DevToolsOpenedByAction::kInspectorModeShortcut);
  }
}

void DevModeBar::CopyUrl() {
  content::WebContents* contents = web_contents();
  if (!contents) {
    return;
  }
  // The URL as shown: a developer wants it exact, so no tracking parameters
  // are taken out (unlike Copy URL elsewhere, compass::LinkToCopy()).
  ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
  writer.WriteText(base::UTF8ToUTF16(contents->GetVisibleURL().spec()));
  ShowCopied(true);
}

void DevModeBar::OnPaintBackground(gfx::Canvas* canvas) {
  const ui::ColorProvider* provider = GetColorProvider();
  if (!provider) {
    return;
  }
  // The card's top corners, which the bar now holds.
  const SkScalar r = kContentCornerRadius;
  const SkVector radii[4] = {{r, r}, {r, r}, {0, 0}, {0, 0}};
  SkRRect rrect;
  rrect.setRectRadii(gfx::RectFToSkRect(gfx::RectF(GetLocalBounds())), radii);
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(provider->GetColor(ui::kColorSysSurface2));
  canvas->DrawPath(SkPath::RRect(rrect), flags);
  // A hairline between the bar and the page.
  const gfx::Rect bottom(0, height() - 1, width(), 1);
  canvas->FillRect(bottom, provider->GetColor(ui::kColorSysDivider));
}

void DevModeBar::OnThemeChanged() {
  views::View::OnThemeChanged();
  UpdateIcons();
}

void DevModeBar::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  // Also same-document navigations (history.pushState).
  if (navigation_handle->IsInPrimaryMainFrame() &&
      navigation_handle->HasCommitted()) {
    UpdateUrl();
  }
}

void DevModeBar::WebContentsDestroyed() {
  Observe(nullptr);
  UpdateUrl();
}

void DevModeBar::OnViewBoundsChanged(views::View* observed_view) {
  if (observed_view == container_) {
    PlaceInContainer();
  }
}

void DevModeBar::OnViewIsDeleting(views::View* observed_view) {
  if (observed_view == url_) {
    url_observation_.Reset();
    return;
  }
  container_observation_.Reset();
  container_ = nullptr;
}

void DevModeBar::OnViewFocused(views::View* observed_view) {
  // As the address bar: the whole URL, ready to replace. After the click
  // that focused it, which would put the caret where it landed.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<DevModeBar> bar) {
                       if (bar && bar->url_->HasFocus()) {
                         bar->url_->SelectAll(/*reversed=*/false);
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

void DevModeBar::OnViewBlurred(views::View* observed_view) {
  // What was typed and not sent goes.
  UpdateUrl();
}

bool DevModeBar::HandleKeyEvent(views::Textfield* sender,
                                const ui::KeyEvent& key_event) {
  if (key_event.type() != ui::EventType::kKeyPressed) {
    return false;
  }
  if (key_event.key_code() == ui::VKEY_RETURN) {
    Navigate(url_->GetText());
    return true;
  }
  if (key_event.key_code() == ui::VKEY_ESCAPE) {
    if (content::WebContents* contents = web_contents()) {
      contents->Focus();
    }
    UpdateUrl();
    return true;
  }
  return false;
}

void DevModeBar::Navigate(std::u16string_view text) {
  content::WebContents* contents = web_contents();
  if (!contents) {
    return;
  }
  // As the omnibox fixes up what's typed: "localhost:3000" is a URL.
  const GURL url = url_formatter::FixupURL(base::UTF16ToUTF8(text),
                                           std::string());
  if (!url.is_valid() || url.SchemeIs(url::kJavaScriptScheme)) {
    UpdateUrl();
    return;
  }
  content::NavigationController::LoadURLParams params(url);
  params.transition_type = ui::PageTransitionFromInt(
      ui::PAGE_TRANSITION_TYPED | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  contents->GetController().LoadURLWithParams(params);
  contents->Focus();
}

void DevModeBar::UpdateUrl() {
  content::WebContents* contents = web_contents();
  const std::u16string url =
      contents ? base::UTF8ToUTF16(contents->GetVisibleURL().spec())
               : std::u16string();
  // Not under the user's typing.
  if (url_->HasFocus()) {
    return;
  }
  if (url_->GetText() != url) {
    url_->SetText(url);
    // The start of the URL, not wherever the caret was.
    url_->SetSelectedRange(gfx::Range(0));
  }
}

void DevModeBar::UpdateIcons() {
  if (!GetColorProvider()) {
    return;
  }
  const ui::ColorId color = ui::kColorSysOnSurfaceSubtle;
  const views::IconColors colors(color, color);
  views::SetImageFromVectorIconWithColor(console_, vector_icons::kCodeIcon,
                                         kIconSize, colors);
  views::SetImageFromVectorIconWithColor(network_, vector_icons::kGlobeIcon,
                                         kIconSize, colors);
  views::SetImageFromVectorIconWithColor(inspect_, vector_icons::kAdsClickIcon,
                                         kIconSize, colors);
  if (copied_) {
    views::SetImageFromVectorIconWithColor(
        copy_, kCheckIcon, kIconSize,
        views::IconColors(ui::kColorSysPrimary, ui::kColorSysPrimary));
  } else {
    views::SetImageFromVectorIconWithColor(
        copy_, vector_icons::kContentCopyIcon, kIconSize, colors);
  }
}

void DevModeBar::PlaceInContainer() {
  if (!container_) {
    return;
  }
  const gfx::Rect area = container_->GetContentsBounds();
  SetBounds(area.x(), area.y(), area.width(), kHeight);
}

void DevModeBar::ShowCopied(bool copied) {
  copied_timer_.Stop();
  copied_ = copied;
  const std::u16string tooltip = copied ? u"Copied" : u"Copy URL";
  copy_->SetTooltipText(tooltip);
  copy_->GetViewAccessibility().SetName(tooltip);
  if (copied) {
    copied_timer_.Start(FROM_HERE, kCopiedDuration,
                        base::BindOnce(&DevModeBar::ShowCopied,
                                       base::Unretained(this), false));
  }
  UpdateIcons();
}

BEGIN_METADATA(DevModeBar)
END_METADATA

int GetContentsTopBarHeight(const views::View* container) {
  DevModeBar* bar = DevModeBar::FindIn(container);
  return bar && bar->GetVisible() ? DevModeBar::kHeight : 0;
}

}  // namespace compass
