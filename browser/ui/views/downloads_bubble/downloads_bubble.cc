// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "base/containers/flat_set.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/download/download_ui_model.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/views/controls/rich_hover_button.h"
#include "chrome/browser/ui/views/download/bubble/download_bubble_row_view.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/toolbar_button_provider.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions.h"
#include "chrome/grit/generated_resources.h"
#include "compass/browser/ui/views/downloads_bubble/downloads_bubble_hooks.h"
#include "compass/browser/ui/views/downloads_bubble/downloads_library_view.h"
#include "compass/browser/ui/views/sidebar/sidebar_tint.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/base/class_property.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/image_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/layout/flex_layout_view.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_tracker.h"
#include "ui/views/widget/widget.h"

namespace compass::downloads_bubble {

namespace {

// The header's margins: the tabs line up with the cards below them.
constexpr gfx::Insets kHeaderInsets = gfx::Insets::TLBR(4, 12, 8, 12);

// The rows' width: upstream's is 320. The bubble is wider by the cards'
// margins (downloads_library_view.cc, kCardInset): 420.
constexpr int kDialogWidth = 396;

// A click on the downloads button closes an open bubble first (it loses
// focus), then presses the button: within this, the press doesn't open it
// again.
constexpr base::TimeDelta kReopenGuard = base::Milliseconds(300);

// The bubble shown without upstream's rows, per window. The window owns it
// (CLIENT_OWNS_WIDGET): the widget goes before its delegate.
struct NoDownloadsBubble {
  std::unique_ptr<views::BubbleDialogDelegate> delegate;
  std::unique_ptr<views::Widget> widget;
  views::ViewTracker library;
  base::TimeTicks closed_at;
};

}  // namespace
}  // namespace compass::downloads_bubble

DEFINE_UI_CLASS_PROPERTY_TYPE(compass::downloads_bubble::NoDownloadsBubble*)

namespace compass::downloads_bubble {

namespace {

DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(NoDownloadsBubble, kNoDownloadsBubbleKey)

// The bubble closed: forget it, and delete it once its close is done.
void OnBubbleClosed(views::ViewTracker* browser_view_tracker,
                    views::Widget::ClosedReason) {
  views::View* browser_view = browser_view_tracker->view();
  NoDownloadsBubble* state =
      browser_view ? browser_view->GetProperty(kNoDownloadsBubbleKey) : nullptr;
  if (!state) {
    return;
  }
  state->closed_at = base::TimeTicks::Now();
  state->library.SetView(nullptr);
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::unique_ptr<views::Widget> widget,
             std::unique_ptr<views::BubbleDialogDelegate> delegate) {
            widget.reset();
            delegate.reset();
          },
          std::move(state->widget), std::move(state->delegate)));
}

// The window's space's color, which the bubble takes: none for a grey
// space, or off the record (as the sidebar's tint).
std::optional<SkColor> SpaceTint(BrowserWindowInterface* browser) {
  if (browser->GetProfile()->IsOffTheRecord()) {
    return std::nullopt;
  }
  WindowFeatures* features = WindowFeatures::From(browser);
  if (!features) {
    return std::nullopt;
  }
  std::optional<SkColor> tint = features->space_model().color().tint();
  if (!tint) {
    return std::nullopt;
  }
  return ToTintColor(*tint, TintSurface::kPane);
}

// The files upstream's rows show.
base::flat_set<base::FilePath> RowPaths(views::ViewTracker* row_list) {
  base::flat_set<base::FilePath> paths;
  if (!row_list->view()) {
    return paths;
  }
  for (views::View* child : row_list->view()->children()) {
    auto* row = views::AsViewClass<DownloadBubbleRowView>(child);
    if (row && row->model()) {
      paths.insert(row->model()->GetTargetFilePath());
    }
  }
  return paths;
}

// Puts the tabs where the header's title was: first, taking the room the
// title had, with the close button still at the end.
void AddTabStrip(views::View* header, DownloadsLibraryView* library) {
  for (views::View* child : header->children()) {
    if (views::IsViewClass<views::Label>(child)) {
      child->SetVisible(false);
      break;
    }
  }
  header->SetBorder(views::CreateEmptyBorder(kHeaderInsets));
  views::View* strip = header->AddChildViewAt(library->CreateTabStrip(), 0);
  strip->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::MinimumFlexSizeRule::kScaleToMinimum,
                               views::MaximumFlexSizeRule::kUnbounded));
  strip->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 0, 0, 8));
}

std::unique_ptr<views::View> MakeShowAllDownloadsButton(
    BrowserWindowInterface* browser) {
  auto button = std::make_unique<RichHoverButton>(
      base::BindRepeating(
          [](base::WeakPtr<BrowserWindowInterface> browser) {
            if (browser) {
              chrome::ShowDownloads(browser.get());
            }
          },
          browser->GetWeakPtr()),
      /*icon=*/ui::ImageModel(),
      l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_FOOTER_LABEL),
      /*subtitle_text=*/std::u16string(),
      ui::ImageModel::FromVectorIcon(
          vector_icons::kOpenInNewFlippableIcon,
          kColorDownloadBubbleShowAllDownloadsIcon,
          GetLayoutConstant(LayoutConstant::kDownloadIconSize)));
  button->SetTooltipText(
      l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_FOOTER_TOOLTIP_LABEL));
  button->SetBorder(views::CreateEmptyBorder(GetLayoutInsets(DOWNLOAD_ROW)));
  return button;
}

void ShowBubble(views::ViewTracker* browser_view_tracker,
                BubbleAnchorResult anchor) {
  auto* browser_view =
      static_cast<BrowserView*>(browser_view_tracker->view());
  if (!browser_view) {
    return;
  }
  BrowserWindowInterface* browser = browser_view->browser();
  if (!anchor.has_value()) {
    chrome::ShowDownloads(browser);
    return;
  }

  auto delegate = std::make_unique<views::BubbleDialogDelegate>(
      anchor.value(), views::BubbleBorder::TOP_RIGHT,
      views::BubbleBorder::DIALOG_SHADOW, /*autosize=*/true);
  delegate->SetTitle(
      l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_HEADER_LABEL));
  delegate->SetShowTitle(false);
  delegate->SetShowCloseButton(false);
  delegate->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
  delegate->set_margins(gfx::Insets::VH(
      views::LayoutProvider::Get()->GetDistanceMetric(
          views::DISTANCE_RELATED_CONTROL_VERTICAL),
      0));
  delegate->SetEnableArrowKeyTraversal(true);

  auto contents = std::make_unique<views::FlexLayoutView>();
  contents->SetOrientation(views::LayoutOrientation::kVertical);
  auto* header =
      contents->AddChildView(std::make_unique<views::FlexLayoutView>());
  header->SetOrientation(views::LayoutOrientation::kHorizontal);
  header->SetCrossAxisAlignment(views::LayoutAlignment::kCenter);
  header->SetBorder(views::CreateEmptyBorder(kHeaderInsets));
  auto* library = contents->AddChildView(std::make_unique<DownloadsLibraryView>(
      browser->GetProfile(), SpaceTint(browser), /*browser_rows=*/nullptr,
      DownloadsLibraryView::RowPathsCallback(),
      MakeShowAllDownloadsButton(browser)));
  views::View* strip = header->AddChildView(library->CreateTabStrip());
  strip->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::MinimumFlexSizeRule::kScaleToMinimum,
                               views::MaximumFlexSizeRule::kUnbounded));
  strip->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 0, 0, 8));
  views::BubbleDialogDelegate* delegate_ptr = delegate.get();
  auto* close = header->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
      base::BindRepeating(
          [](views::BubbleDialogDelegate* delegate) {
            delegate->GetWidget()->CloseWithReason(
                views::Widget::ClosedReason::kCloseButtonClicked);
          },
          delegate_ptr),
      vector_icons::kCloseIcon,
      GetLayoutConstant(LayoutConstant::kDownloadIconSize)));
  views::InstallCircleHighlightPathGenerator(close);
  close->SetTooltipText(l10n_util::GetStringUTF16(IDS_APP_CLOSE));
  close->SetBorder(nullptr);
  delegate->SetInitiallyFocusedView(close);
  delegate->SetContentsView(std::move(contents));

  NoDownloadsBubble* state = browser_view->GetProperty(kNoDownloadsBubbleKey);
  if (!state) {
    state = browser_view->SetProperty(kNoDownloadsBubbleKey,
                                      std::make_unique<NoDownloadsBubble>());
  }
  state->library.SetView(library);
  state->delegate = std::move(delegate);
  state->widget = views::BubbleDialogDelegate::CreateBubble(
      state->delegate.get(),
      base::BindOnce(&OnBubbleClosed,
                     base::Owned(std::make_unique<views::ViewTracker>(
                         browser_view))));
  state->widget->Show();
}

}  // namespace

int DialogWidth() {
  return kDialogWidth;
}

void ExtendDownloadDialog(views::View* dialog,
                          views::ScrollView* rows,
                          BrowserWindowInterface* browser) {
  if (!dialog || !rows || !browser || dialog->children().size() < 3) {
    return;
  }
  // Upstream's order: the header, maybe the incognito notice, the rows, the
  // footer. Anything else, and it's left as upstream made it.
  views::View* header = dialog->children().front();
  views::View* footer = dialog->children().back();
  if (rows->parent() != dialog || header == rows || footer == rows) {
    return;
  }

  // The rows are as tall as they are, in the Downloads tab's own scroller
  // with the folder's files after them.
  rows->ClipHeightTo(0, std::numeric_limits<int>::max());
  rows->SetVerticalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
  auto row_list = std::make_unique<views::ViewTracker>(rows->contents());
  DownloadsLibraryView::RowPathsCallback row_paths =
      base::BindRepeating(&RowPaths, base::Owned(std::move(row_list)));

  std::unique_ptr<views::View> owned_rows = dialog->RemoveChildViewT(rows);
  std::unique_ptr<views::View> owned_footer = dialog->RemoveChildViewT(footer);
  auto* library = dialog->AddChildView(std::make_unique<DownloadsLibraryView>(
      browser->GetProfile(), SpaceTint(browser), std::move(owned_rows),
      std::move(row_paths),
      std::move(owned_footer)));
  AddTabStrip(header, library);
}

bool ShowWithoutRecentDownloads(BrowserView* browser_view) {
  if (!browser_view) {
    return false;
  }
  if (NoDownloadsBubble* state =
          browser_view->GetProperty(kNoDownloadsBubbleKey)) {
    if (state->widget && !state->widget->IsClosed()) {
      state->widget->Close();
      return true;
    }
    if (base::TimeTicks::Now() - state->closed_at < kReopenGuard) {
      return true;
    }
  }
  ToolbarButtonProvider* provider = browser_view->toolbar_button_provider();
  PinnedToolbarActions* actions =
      provider ? provider->GetPinnedToolbarActions() : nullptr;
  if (!actions) {
    return false;
  }
  actions->GetBubbleAnchorAsync(
      kActionShowDownloads,
      base::BindOnce(&ShowBubble, base::Owned(std::make_unique<views::ViewTracker>(
                                      browser_view))));
  return true;
}

DownloadsLibraryView* GetBubbleLibraryForTesting(BrowserView* browser_view) {
  NoDownloadsBubble* state = browser_view->GetProperty(kNoDownloadsBubbleKey);
  return state ? views::AsViewClass<DownloadsLibraryView>(state->library.view())
               : nullptr;
}

}  // namespace compass::downloads_bubble
