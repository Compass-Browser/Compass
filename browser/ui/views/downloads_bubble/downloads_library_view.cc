// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/downloads_bubble/downloads_library_view.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "base/files/file_path_watcher.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/i18n/time_formatting.h"
#include "base/strings/strcat.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/browser/ui/views/controls/hover_button.h"
#include "compass/browser/ui/views/bubble_look.h"
#include "compass/browser/ui/views/downloads_bubble/downloads_bubble_hooks.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/vector_icons/vector_icons.h"
#include "services/data_decoder/public/cpp/decode_image.h"
#include "services/data_decoder/public/mojom/image_decoder.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkPath.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/time_format.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_variant.h"
#include "ui/compositor/paint_recorder.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/drag_controller.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/box_layout_view.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/paint_info.h"
#include "ui/views/style/typography.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_tracker.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"

namespace compass::downloads_bubble {

using downloads_folder::FileKind;
using downloads_folder::FolderFile;

namespace {

// Taller than upstream's 450: the two lists share the bubble.
constexpr int kMaxListHeight = 520;
constexpr int kRowIconSize = 32;
constexpr int kRowIconRadius = 6;
constexpr int kGridColumns = 3;
constexpr int kGridGap = 8;
constexpr int kTileRadius = 10;
// The look of Compass's settings window (settings-window.md, "Visual
// design"): groups as rounded cards on the space's tint, a segmented
// control with a white pill for the tabs.
constexpr int kCardInset = 12;
constexpr int kCardRadius = 12;
constexpr int kTrackRadius = 9;
constexpr int kPillRadius = 7;
constexpr int kTrackPadding = 2;
constexpr int kTabIconSize = 16;
// A row's preview, found again in its HoverButton.
constexpr int kThumbnailViewId = 1;
// How many of each the lists show, newest first.
constexpr size_t kFolderFileLimit = 100;
constexpr size_t kScreenshotLimit = 60;
// Changes come in bursts (a download renames its partial file at the end).
constexpr base::TimeDelta kRefreshDelay = base::Milliseconds(250);

enum Command { kOpen = 1, kShowInFolder, kCopy, kTrash };

using Look = BubbleLook;

// A rounded rectangle with a soft shadow: the selected tab, the pill
// buttons.
class PillBackground : public views::Background {
 public:
  PillBackground(SkColor color, int radius, bool shadow)
      : color_(color), radius_(radius), shadow_(shadow) {}

  void Paint(gfx::Canvas* canvas, views::View* view) const override {
    const gfx::RectF bounds(view->GetLocalBounds());
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    if (shadow_) {
      flags.setLooper(gfx::CreateShadowDrawLooper(
          {gfx::ShadowValue(gfx::Vector2d(0, 1), 3,
                            SkColorSetA(SK_ColorBLACK, 0x26))}));
    }
    flags.setColor(color_);
    canvas->DrawRoundRect(bounds, radius_, flags);
  }

 private:
  const SkColor color_;
  const int radius_;
  const bool shadow_;
};

// A hairline between a card's rows, from where their text starts.
class Hairline : public views::View {
  METADATA_HEADER(Hairline, views::View)

 public:
  explicit Hairline(int start) : start_(start) {
    SetPreferredSize(gfx::Size(0, 1));
  }
  void SetColor(SkColor color) {
    color_ = color;
    SchedulePaint();
  }
  void OnPaint(gfx::Canvas* canvas) override {
    canvas->FillRect(gfx::Rect(start_, 0, width() - start_, 1), color_);
  }

 private:
  const int start_;
  SkColor color_ = SK_ColorTRANSPARENT;
};

BEGIN_METADATA(Hairline)
END_METADATA

#if BUILDFLAG(IS_MAC)
constexpr int kShowInFolderLabel = IDS_COMPASS_DOWNLOADS_SHOW_IN_FINDER;
constexpr int kTrashLabel = IDS_COMPASS_DOWNLOADS_MOVE_TO_TRASH;
#elif BUILDFLAG(IS_WIN)
constexpr int kShowInFolderLabel = IDS_COMPASS_DOWNLOADS_SHOW_IN_FOLDER;
constexpr int kTrashLabel = IDS_COMPASS_DOWNLOADS_MOVE_TO_RECYCLE_BIN;
#else
constexpr int kShowInFolderLabel = IDS_COMPASS_DOWNLOADS_SHOW_IN_FOLDER;
constexpr int kTrashLabel = IDS_COMPASS_DOWNLOADS_MOVE_TO_TRASH;
#endif

// "Just now", "5 mins ago", "3 days ago", then the date.
std::u16string When(base::Time added) {
  const base::TimeDelta age = base::Time::Now() - added;
  if (age < base::Minutes(1)) {
    return l10n_util::GetStringUTF16(IDS_COMPASS_DOWNLOADS_JUST_NOW);
  }
  if (age < base::Days(7)) {
    return ui::TimeFormat::Simple(ui::TimeFormat::FORMAT_ELAPSED,
                                  ui::TimeFormat::LENGTH_SHORT, age);
  }
  return base::TimeFormatShortDate(added);
}

std::u16string Subtitle(const FolderFile& file) {
  return base::StrCat(
      {file.is_directory
           ? l10n_util::GetStringUTF16(IDS_COMPASS_DOWNLOADS_FOLDER)
           : ui::FormatBytes(base::ByteSize(static_cast<uint64_t>(file.size))),
       u" · ", When(file.added)});
}

std::u16string DisplayName(const base::FilePath& path) {
  return path.BaseName().LossyDisplayName();
}

// "~/Downloads" for a folder in the user's home.
std::u16string FolderDisplayName(const base::FilePath& folder) {
  const base::FilePath home = base::GetHomeDir();
  base::FilePath relative;
  if (!home.empty() && home.AppendRelativePath(folder, &relative)) {
    return base::StrCat({u"~/", relative.LossyDisplayName()});
  }
  return folder.LossyDisplayName();
}

// A file's preview, drawn into a rounded rectangle: filling it (pictures,
// videos) or fitting in it (icons, documents).
class ThumbnailView : public views::View {
  METADATA_HEADER(ThumbnailView, views::View)

 public:
  ThumbnailView(gfx::Size size, bool fill, int radius, bool video_badge)
      : fill_(fill), radius_(radius), video_badge_(video_badge) {
    SetPreferredSize(size);
    SetCanProcessEventsWithinSubtree(false);
  }

  void SetImage(const gfx::ImageSkia& image) {
    image_ = image;
    SchedulePaint();
  }
  const gfx::ImageSkia& image() const { return image_; }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::Rect bounds = GetLocalBounds();
    const SkPath clip = SkPath::RRect(gfx::RectToSkRect(bounds), radius_,
                                      radius_);
    gfx::ScopedCanvas scoped(canvas);
    canvas->ClipPath(clip, /*do_anti_alias=*/true);
    if (fill_) {
      canvas->FillRect(bounds, GetColorProvider()->GetColor(
                                   ui::kColorSysNeutralContainer));
    }
    if (image_.isNull() || image_.width() == 0 || image_.height() == 0) {
      return;
    }
    const double x_scale =
        static_cast<double>(bounds.width()) / image_.width();
    const double y_scale =
        static_cast<double>(bounds.height()) / image_.height();
    if (fill_) {
      // Cover the rectangle; crop what's past it, keeping the middle.
      const double scale = std::max(x_scale, y_scale);
      const int source_width = std::round(bounds.width() / scale);
      const int source_height = std::round(bounds.height() / scale);
      canvas->DrawImageInt(image_, (image_.width() - source_width) / 2,
                           (image_.height() - source_height) / 2,
                           source_width, source_height, 0, 0, bounds.width(),
                           bounds.height(), /*filter=*/true);
    } else {
      const double scale = std::min({x_scale, y_scale, 1.0});
      const int width = std::round(image_.width() * scale);
      const int height = std::round(image_.height() * scale);
      canvas->DrawImageInt(image_, 0, 0, image_.width(), image_.height(),
                           (bounds.width() - width) / 2,
                           (bounds.height() - height) / 2, width, height,
                           /*filter=*/true);
    }
    if (video_badge_) {
      constexpr int kBadge = 20;
      const gfx::Rect badge(6, bounds.height() - kBadge - 6, kBadge, kBadge);
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(SkColorSetA(SK_ColorBLACK, 0x99));
      canvas->DrawCircle(gfx::RectF(badge).CenterPoint(), kBadge / 2.0f,
                         flags);
      const gfx::ImageSkia play = gfx::CreateVectorIcon(
          vector_icons::kPlayArrowIcon, 14, SK_ColorWHITE);
      canvas->DrawImageInt(play, badge.x() + (kBadge - 14) / 2,
                           badge.y() + (kBadge - 14) / 2);
    }
  }

 private:
  gfx::ImageSkia image_;
  const bool fill_;
  const int radius_;
  const bool video_badge_;
};

BEGIN_METADATA(ThumbnailView)
END_METADATA

// What a row and a tile do with their file: open it, show it in its folder,
// copy a picture, move it to the Trash, drag it out.
class FileActions : public ui::SimpleMenuModel::Delegate {
 public:
  FileActions(Profile* profile,
              FolderFile file,
              base::RepeatingClosure changed)
      : profile_(profile),
        file_(std::move(file)),
        changed_(std::move(changed)) {}

  const FolderFile& file() const { return file_; }

  // Opens the file and closes the bubble, as upstream's rows do.
  void Open(views::View* from) {
    platform_util::OpenItem(profile_, file_.path,
                            file_.is_directory ? platform_util::OPEN_FOLDER
                                               : platform_util::OPEN_FILE,
                            base::DoNothing());
    if (views::Widget* widget = from ? from->GetWidget() : nullptr) {
      widget->Close();
    }
  }

  void ShowMenu(views::View* source,
                const gfx::Point& point,
                ui::mojom::MenuSourceType source_type) {
    source_.SetView(source);
    menu_model_ = std::make_unique<ui::SimpleMenuModel>(this);
    menu_model_->AddItem(kOpen, l10n_util::GetStringUTF16(IDS_COMPASS_OPEN));
    menu_model_->AddItem(kShowInFolder,
                         l10n_util::GetStringUTF16(kShowInFolderLabel));
    if (file_.kind == FileKind::kImage) {
      menu_model_->AddItem(kCopy, l10n_util::GetStringUTF16(IDS_COMPASS_COPY));
    }
    menu_model_->AddSeparator(ui::NORMAL_SEPARATOR);
    menu_model_->AddItem(kTrash, l10n_util::GetStringUTF16(kTrashLabel));
    menu_runner_ = std::make_unique<views::MenuRunner>(
        menu_model_.get(), views::MenuRunner::CONTEXT_MENU |
                               views::MenuRunner::HAS_MNEMONICS);
    menu_runner_->RunMenuAt(source->GetWidget(), nullptr,
                            gfx::Rect(point, gfx::Size()),
                            views::MenuAnchorPosition::kTopLeft, source_type);
  }

  void WriteDragData(ui::OSExchangeData* data, const gfx::ImageSkia& image) {
    data->SetFilename(file_.path);
    if (!image.isNull()) {
      data->provider().SetDragImage(
          image, gfx::Vector2d(image.width() / 2, image.height() / 2));
    }
  }

  // ui::SimpleMenuModel::Delegate:
  void ExecuteCommand(int command_id, int event_flags) override {
    switch (command_id) {
      case kOpen:
        Open(source_.view());
        break;
      case kShowInFolder:
        platform_util::ShowItemInFolder(profile_, file_.path);
        break;
      case kCopy:
        CopyPicture(file_.path);
        break;
      case kTrash:
        base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE,
            {base::MayBlock(), base::WithBaseSyncPrimitives(),
             base::TaskPriority::USER_VISIBLE},
            base::BindOnce(&downloads_folder::MoveToTrash, file_.path),
            base::BindOnce([](base::RepeatingClosure changed,
                              bool) { changed.Run(); },
                           changed_));
        break;
    }
  }

 private:
  // Reads and decodes the picture, then puts it on the clipboard.
  static void CopyPicture(const base::FilePath& path) {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](const base::FilePath& path) {
              return base::ReadFileToBytes(path);
            },
            path),
        base::BindOnce([](std::optional<std::vector<uint8_t>> bytes) {
          if (!bytes) {
            return;
          }
          data_decoder::DecodeImageIsolated(
              *bytes, data_decoder::mojom::ImageCodec::kDefault,
              /*shrink_to_fit=*/false, data_decoder::kDefaultMaxSizeInBytes,
              gfx::Size(), base::BindOnce([](const SkBitmap& bitmap) {
                if (bitmap.drawsNothing()) {
                  return;
                }
                ui::ScopedClipboardWriter writer(
                    ui::ClipboardBuffer::kCopyPaste);
                writer.WriteImage(bitmap);
              }));
        }));
  }

  const raw_ptr<Profile> profile_;
  const FolderFile file_;
  const base::RepeatingClosure changed_;
  views::ViewTracker source_;
  std::unique_ptr<ui::SimpleMenuModel> menu_model_;
  std::unique_ptr<views::MenuRunner> menu_runner_;
};

std::unique_ptr<ThumbnailView> MakeRowThumbnail(const FolderFile& file) {
  auto thumbnail = std::make_unique<ThumbnailView>(
      gfx::Size(kRowIconSize, kRowIconSize),
      /*fill=*/file.kind != FileKind::kOther && !file.is_directory,
      kRowIconRadius, /*video_badge=*/false);
  thumbnail->SetID(kThumbnailViewId);
  return thumbnail;
}

// A file in the Downloads tab: its preview or icon, its name, its size and
// when it came.
class FolderFileRow : public HoverButton,
                      public views::ContextMenuController,
                      public views::DragController {
  METADATA_HEADER(FolderFileRow, HoverButton)

 public:
  FolderFileRow(Profile* profile,
                FolderFile file,
                FileThumbnails& thumbnails,
                base::RepeatingClosure changed)
      : FolderFileRow(profile,
                      file,
                      thumbnails,
                      std::move(changed),
                      MakeRowThumbnail(file)) {}

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override {
    actions_.ShowMenu(this, point, source_type);
  }

  // views::DragController:
  void WriteDragDataForView(views::View* sender,
                            const gfx::Point& press_pt,
                            ui::OSExchangeData* data) override {
    actions_.WriteDragData(
        data, thumbnail_ ? thumbnail_->image() : gfx::ImageSkia());
  }
  int GetDragOperationsForView(views::View* sender,
                               const gfx::Point& p) override {
    return ui::DragDropTypes::DRAG_COPY | ui::DragDropTypes::DRAG_LINK;
  }
  bool CanStartDragForView(views::View* sender,
                           const gfx::Point& press_pt,
                           const gfx::Point& p) override {
    return true;
  }

 private:
  FolderFileRow(Profile* profile,
                FolderFile file,
                FileThumbnails& thumbnails,
                base::RepeatingClosure changed,
                std::unique_ptr<ThumbnailView> thumbnail)
      : HoverButton(base::BindRepeating(&FolderFileRow::OnPressed,
                                        base::Unretained(this)),
                    std::move(thumbnail),
                    DisplayName(file.path),
                    Subtitle(file)),
        actions_(profile, file, std::move(changed)) {
    thumbnail_ = views::AsViewClass<ThumbnailView>(GetViewByID(kThumbnailViewId));
    title()->SetElideBehavior(gfx::ELIDE_MIDDLE);
    SetTooltipText(DisplayName(file.path));
    SetBorder(views::CreateEmptyBorder(GetLayoutInsets(DOWNLOAD_ROW)));
    set_context_menu_controller(this);
    set_drag_controller(this);
    thumbnails.Load(file, gfx::Size(kRowIconSize, kRowIconSize),
                    base::BindOnce(&FolderFileRow::OnThumbnail,
                                   weak_factory_.GetWeakPtr()));
  }

  void OnPressed() { actions_.Open(this); }

  void OnThumbnail(const gfx::ImageSkia& image) {
    if (thumbnail_) {
      thumbnail_->SetImage(image);
    }
  }

  FileActions actions_;
  raw_ptr<ThumbnailView> thumbnail_ = nullptr;
  base::WeakPtrFactory<FolderFileRow> weak_factory_{this};
};

BEGIN_METADATA(FolderFileRow)
END_METADATA

// A screenshot or a screen recording in the Screenshots tab's grid.
class ScreenshotTile : public views::Button,
                       public views::ContextMenuController,
                       public views::DragController {
  METADATA_HEADER(ScreenshotTile, views::Button)

 public:
  ScreenshotTile(Profile* profile,
                 FolderFile file,
                 gfx::Size size,
                 FileThumbnails& thumbnails,
                 base::RepeatingClosure changed)
      : views::Button(base::BindRepeating(&ScreenshotTile::OnPressed,
                                          base::Unretained(this))),
        actions_(profile, file, std::move(changed)) {
    SetLayoutManager(std::make_unique<views::FillLayout>());
    thumbnail_ = AddChildView(std::make_unique<ThumbnailView>(
        size, /*fill=*/true, kTileRadius,
        /*video_badge=*/file.kind == FileKind::kVideo));
    SetPreferredSize(size);
    SetTooltipText(
        base::StrCat({DisplayName(file.path), u"\n", When(file.added)}));
    GetViewAccessibility().SetName(DisplayName(file.path));
    views::InstallRoundRectHighlightPathGenerator(this, gfx::Insets(),
                                                  kTileRadius);
    views::FocusRing::Install(this);
    set_context_menu_controller(this);
    set_drag_controller(this);
    thumbnails.Load(file, size,
                    base::BindOnce(&ScreenshotTile::OnThumbnail,
                                   weak_factory_.GetWeakPtr()));
  }

  // views::Button:
  void PaintButtonContents(gfx::Canvas* canvas) override {
    // The thumbnail paints over this; the hover tint goes over it below.
  }
  void PaintChildren(const views::PaintInfo& info) override {
    views::Button::PaintChildren(info);
    ui::PaintRecorder recorder(info.context(), size());
    gfx::Canvas* canvas = recorder.canvas();
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    if (GetState() == STATE_HOVERED || GetState() == STATE_PRESSED) {
      flags.setColor(SkColorSetA(SK_ColorBLACK,
                                 GetState() == STATE_PRESSED ? 0x40 : 0x1A));
      canvas->DrawRoundRect(GetLocalBounds(), kTileRadius, flags);
    }
    // A thin frame, so a white page doesn't melt into the background.
    flags.setColor(SkColorSetA(SK_ColorBLACK, 0x1F));
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(1);
    gfx::RectF frame(GetLocalBounds());
    frame.Inset(0.5f);
    canvas->DrawRoundRect(frame, kTileRadius - 0.5f, flags);
  }
  void StateChanged(ButtonState old_state) override {
    views::Button::StateChanged(old_state);
    SchedulePaint();
  }

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override {
    actions_.ShowMenu(this, point, source_type);
  }

  // views::DragController:
  void WriteDragDataForView(views::View* sender,
                            const gfx::Point& press_pt,
                            ui::OSExchangeData* data) override {
    actions_.WriteDragData(data, thumbnail_->image());
  }
  int GetDragOperationsForView(views::View* sender,
                               const gfx::Point& p) override {
    return ui::DragDropTypes::DRAG_COPY | ui::DragDropTypes::DRAG_LINK;
  }
  bool CanStartDragForView(views::View* sender,
                           const gfx::Point& press_pt,
                           const gfx::Point& p) override {
    return true;
  }

 private:
  void OnPressed() { actions_.Open(this); }
  void OnThumbnail(const gfx::ImageSkia& image) { thumbnail_->SetImage(image); }

  FileActions actions_;
  raw_ptr<ThumbnailView> thumbnail_ = nullptr;
  base::WeakPtrFactory<ScreenshotTile> weak_factory_{this};
};

BEGIN_METADATA(ScreenshotTile)
END_METADATA

// The bubble's two tabs, as the settings window's: a segmented track with
// the selected tab a white pill, its icon in the space's color.
class TabStrip : public views::BoxLayoutView {
  METADATA_HEADER(TabStrip, views::BoxLayoutView)

 public:
  using Tab = DownloadsLibraryView::Tab;

  TabStrip(base::WeakPtr<DownloadsLibraryView> library,
           Tab selected,
           std::optional<SkColor> tint)
      : tint_(tint), selected_(selected) {
    SetOrientation(views::BoxLayout::Orientation::kHorizontal);
    SetInsideBorderInsets(gfx::Insets(kTrackPadding));
    SetBetweenChildSpacing(kTrackPadding);
    SetCrossAxisAlignment(views::BoxLayout::CrossAxisAlignment::kStretch);
    GetViewAccessibility().SetRole(ax::mojom::Role::kTabList);
    downloads_ =
        AddTab(library, Tab::kDownloads,
               l10n_util::GetStringUTF16(IDS_COMPASS_DOWNLOADS_TAB_DOWNLOADS));
    screenshots_ = AddTab(
        library, Tab::kScreenshots,
        l10n_util::GetStringUTF16(IDS_COMPASS_DOWNLOADS_TAB_SCREENSHOTS));
  }

  void Select(Tab tab) {
    selected_ = tab;
    Restyle();
  }

  // views::View:
  void OnThemeChanged() override {
    views::BoxLayoutView::OnThemeChanged();
    Restyle();
  }

 private:
  views::LabelButton* AddTab(base::WeakPtr<DownloadsLibraryView> library,
                             Tab tab,
                             const std::u16string& text) {
    auto* button = AddChildView(std::make_unique<views::LabelButton>(
        base::BindRepeating(
            [](base::WeakPtr<DownloadsLibraryView> library, Tab tab) {
              if (library) {
                library->SelectTab(tab);
              }
            },
            library, tab),
        text));
    button->SetLabelStyle(views::style::STYLE_BODY_3_MEDIUM);
    button->SetHorizontalAlignment(gfx::ALIGN_CENTER);
    button->SetImageLabelSpacing(6);
    button->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(4, 10)));
    button->GetViewAccessibility().SetRole(ax::mojom::Role::kTab);
    views::InstallRoundRectHighlightPathGenerator(button, gfx::Insets(),
                                                  kPillRadius);
    SetFlexForView(button, 1);
    return button;
  }

  void Restyle() {
    if (!GetColorProvider()) {
      return;
    }
    const Look look = ComputeBubbleLook(GetColorProvider(), tint_);
    SetBackground(views::CreateRoundedRectBackground(ui::ColorVariant(look.track),
                                                     kTrackRadius));
    Style(downloads_, vector_icons::kDownloadIcon,
          selected_ == Tab::kDownloads, look);
    Style(screenshots_, kScreenshotRegionIcon, selected_ == Tab::kScreenshots,
          look);
  }

  static void Style(views::LabelButton* button,
                    const gfx::VectorIcon& icon,
                    bool selected,
                    const Look& look) {
    if (selected) {
      button->SetBackground(
          std::make_unique<PillBackground>(look.pill, kPillRadius,
                                           /*shadow=*/!look.dark));
    } else {
      button->SetBackground(nullptr);
    }
    button->SetEnabledTextColors(
        ui::ColorVariant(selected ? look.text : look.text_dim));
    button->SetImageModel(
        views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(
            icon, ui::ColorVariant(selected ? look.accent : look.text_dim),
            kTabIconSize));
    button->GetViewAccessibility().SetIsSelected(selected);
  }

  const std::optional<SkColor> tint_;
  Tab selected_;
  raw_ptr<views::LabelButton> downloads_ = nullptr;
  raw_ptr<views::LabelButton> screenshots_ = nullptr;

 public:
  base::CallbackListSubscription subscription_;
};

BEGIN_METADATA(TabStrip)
END_METADATA

views::Label* AddEmptyLabel(views::View* parent, const std::u16string& text) {
  auto* label = parent->AddChildView(std::make_unique<views::Label>(
      text, views::style::CONTEXT_DIALOG_BODY_TEXT,
      views::style::STYLE_SECONDARY));
  label->SetMultiLine(true);
  label->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(24, 16)));
  label->SetVisible(false);
  return label;
}

// A group's caption, over its card, as the settings window's.
views::Label* AddCaption(views::View* parent, const std::u16string& text) {
  auto* label = parent->AddChildView(std::make_unique<views::Label>(
      text, views::style::CONTEXT_LABEL, views::style::STYLE_BODY_5_MEDIUM));
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  label->SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(10, 6, 6, 6)));
  label->SetVisible(false);
  return label;
}

views::BoxLayoutView* AddCard(views::View* parent) {
  auto* card = parent->AddChildView(std::make_unique<views::BoxLayoutView>());
  card->SetOrientation(views::BoxLayout::Orientation::kVertical);
  card->SetCrossAxisAlignment(views::BoxLayout::CrossAxisAlignment::kStretch);
  card->SetInsideBorderInsets(gfx::Insets::VH(4, 0));
  card->SetVisible(false);
  return card;
}

std::unique_ptr<views::ScrollView> MakeScroll(
    std::unique_ptr<views::View> contents) {
  auto scroll = std::make_unique<views::ScrollView>();
  scroll->SetContents(std::move(contents));
  scroll->ClipHeightTo(0, kMaxListHeight);
  scroll->SetHorizontalScrollBarMode(
      views::ScrollView::ScrollBarMode::kDisabled);
  return scroll;
}

}  // namespace

// Follows the two folders on a blocking sequence; |changed| runs on the
// bubble's.
class FolderWatch {
 public:
  FolderWatch(std::vector<base::FilePath> folders,
              base::RepeatingClosure changed) {
    for (const base::FilePath& folder : folders) {
      auto watcher = std::make_unique<base::FilePathWatcher>();
      if (watcher->Watch(folder, base::FilePathWatcher::Type::kNonRecursive,
                         base::BindRepeating(
                             [](base::RepeatingClosure changed,
                                const base::FilePath&, bool) {
                               changed.Run();
                             },
                             changed))) {
        watchers_.push_back(std::move(watcher));
      }
    }
  }

 private:
  std::vector<std::unique_ptr<base::FilePathWatcher>> watchers_;
};

struct DownloadsLibraryView::Listing {
  base::FilePath download_folder;
  base::FilePath screenshot_folder;
  bool same_folder = false;
  std::vector<FolderFile> downloads;
  std::vector<FolderFile> screenshots;
};

DownloadsLibraryView::DownloadsLibraryView(
    Profile* profile,
    std::optional<SkColor> tint,
    std::unique_ptr<views::View> browser_rows,
    RowPathsCallback row_paths,
    std::unique_ptr<views::View> downloads_footer)
    : profile_(profile), row_paths_(std::move(row_paths)), tint_(tint) {
  SetLayoutManager(std::make_unique<views::BoxLayout>(
                       views::BoxLayout::Orientation::kVertical))
      ->set_cross_axis_alignment(
          views::BoxLayout::CrossAxisAlignment::kStretch);

  // Downloads: two captioned cards.
  auto* downloads_page =
      AddChildView(std::make_unique<views::BoxLayoutView>());
  downloads_page->SetOrientation(views::BoxLayout::Orientation::kVertical);
  downloads_page->SetCrossAxisAlignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);
  downloads_page_ = downloads_page;
  auto list = std::make_unique<views::BoxLayoutView>();
  list->SetOrientation(views::BoxLayout::Orientation::kVertical);
  list->SetCrossAxisAlignment(views::BoxLayout::CrossAxisAlignment::kStretch);
  list->SetInsideBorderInsets(gfx::Insets::TLBR(0, kCardInset, 8, kCardInset));
  recent_caption_ = AddCaption(
      list.get(), l10n_util::GetStringUTF16(IDS_COMPASS_DOWNLOADS_RECENT));
  recent_card_ = AddCard(list.get());
  if (browser_rows) {
    browser_rows_ = recent_card_->AddChildView(std::move(browser_rows));
  }
  folder_caption_ = AddCaption(
      list.get(), l10n_util::GetStringUTF16(IDS_COMPASS_DOWNLOADS_IN_FOLDER));
  auto* folder_card = AddCard(list.get());
  folder_rows_ = folder_card;
  downloads_empty_ = AddEmptyLabel(
      list.get(),
      l10n_util::GetStringUTF16(IDS_COMPASS_DOWNLOADS_FOLDER_EMPTY));
  downloads_scroll_ =
      downloads_page->AddChildView(MakeScroll(std::move(list)));
  if (downloads_footer) {
    downloads_page->AddChildView(std::move(downloads_footer));
  }

  // Screenshots: where they're saved, then the grid.
  auto* screenshots_page =
      AddChildView(std::make_unique<views::BoxLayoutView>());
  screenshots_page->SetOrientation(views::BoxLayout::Orientation::kVertical);
  screenshots_page->SetCrossAxisAlignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);
  screenshots_page_ = screenshots_page;
  auto* folder_row =
      screenshots_page->AddChildView(std::make_unique<views::BoxLayoutView>());
  folder_row->SetOrientation(views::BoxLayout::Orientation::kHorizontal);
  folder_row->SetCrossAxisAlignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);
  folder_row->SetInsideBorderInsets(
      gfx::Insets::TLBR(4, kCardInset, 6, kCardInset));
  screenshots_caption_ = AddCaption(folder_row, std::u16string());
  screenshots_caption_->SetBorder(
      views::CreateEmptyBorder(gfx::Insets::VH(0, 6)));
  screenshots_caption_->SetElideBehavior(gfx::ELIDE_HEAD);
  screenshots_caption_->SetVisible(true);
  folder_row->SetFlexForView(screenshots_caption_, 1);
  open_folder_ = folder_row->AddChildView(std::make_unique<views::LabelButton>(
      base::BindRepeating(&DownloadsLibraryView::OpenScreenshotFolder,
                          base::Unretained(this)),
      l10n_util::GetStringUTF16(
#if BUILDFLAG(IS_MAC)
          IDS_COMPASS_DOWNLOADS_SHOW_IN_FINDER
#else
          IDS_COMPASS_DOWNLOADS_OPEN_FOLDER
#endif
          )));
  open_folder_->SetLabelStyle(views::style::STYLE_BODY_4_MEDIUM);
  open_folder_->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(4, 10)));
  views::InstallRoundRectHighlightPathGenerator(open_folder_, gfx::Insets(),
                                                kPillRadius);

  auto grid = std::make_unique<views::BoxLayoutView>();
  grid->SetOrientation(views::BoxLayout::Orientation::kVertical);
  grid->SetBetweenChildSpacing(kGridGap);
  grid->SetInsideBorderInsets(
      gfx::Insets::TLBR(0, kCardInset, kCardInset, kCardInset));
  grid_ = grid.get();
  auto grid_page = std::make_unique<views::BoxLayoutView>();
  grid_page->SetOrientation(views::BoxLayout::Orientation::kVertical);
  grid_page->SetCrossAxisAlignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);
  grid_page->AddChildView(std::move(grid));
  screenshots_empty_ = AddEmptyLabel(
      grid_page.get(),
      l10n_util::GetStringUTF16(IDS_COMPASS_DOWNLOADS_NO_SCREENSHOTS));
  screenshots_scroll_ =
      screenshots_page->AddChildView(MakeScroll(std::move(grid_page)));
  screenshots_page->SetVisible(false);

  Refresh();
}

DownloadsLibraryView::~DownloadsLibraryView() = default;

gfx::Size DownloadsLibraryView::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  // Fixed, as upstream's rows are: long names elide rather than widen it.
  // The cards hold rows of DialogWidth(), with the tint around them.
  const int width = DialogWidth() + 2 * kCardInset;
  return gfx::Size(width,
                   GetLayoutManager()->GetPreferredHeightForWidth(this, width));
}

void DownloadsLibraryView::OnThemeChanged() {
  views::View::OnThemeChanged();
  ApplyLook();
}

void DownloadsLibraryView::AddedToWidget() {
  views::View::AddedToWidget();
  ApplyLook();
}

void DownloadsLibraryView::ApplyLook() {
  if (!GetColorProvider()) {
    return;
  }
  const Look look = ComputeBubbleLook(GetColorProvider(), tint_);
  SetBackground(views::CreateSolidBackground(look.background));
  // Scrolling contents paint on a layer that must be opaque.
  downloads_scroll_->SetBackgroundColor(ui::ColorVariant(look.background));
  screenshots_scroll_->SetBackgroundColor(ui::ColorVariant(look.background));
  for (views::View* card : {recent_card_.get(), folder_rows_.get()}) {
    card->SetBackground(views::CreateRoundedRectBackground(
        ui::ColorVariant(look.card), kCardRadius));
  }
  if (auto* rows = views::AsViewClass<views::ScrollView>(browser_rows_)) {
    rows->SetBackgroundColor(ui::ColorVariant(look.card));
  }
  for (views::View* child : folder_rows_->children()) {
    if (auto* hairline = views::AsViewClass<Hairline>(child)) {
      hairline->SetColor(look.hairline);
    }
  }
  for (views::Label* caption : {recent_caption_.get(), folder_caption_.get(),
                                screenshots_caption_.get()}) {
    caption->SetEnabledColor(look.text_dim);
  }
  open_folder_->SetBackground(std::make_unique<PillBackground>(
      look.pill, kPillRadius, /*shadow=*/!look.dark));
  open_folder_->SetEnabledTextColors(ui::ColorVariant(look.accent));
  // The frame around the contents: the bubble's own background.
  if (views::Widget* widget = GetWidget()) {
    if (views::WidgetDelegate* delegate = widget->widget_delegate()) {
      if (views::BubbleDialogDelegate* bubble =
              delegate->AsBubbleDialogDelegate()) {
        bubble->SetBackgroundColor(ui::ColorVariant(look.background));
      }
    }
  }
}

std::unique_ptr<views::View> DownloadsLibraryView::CreateTabStrip() {
  auto strip =
      std::make_unique<TabStrip>(weak_factory_.GetWeakPtr(), tab_, tint_);
  strip->subscription_ = tab_callbacks_.Add(base::BindRepeating(
      &TabStrip::Select, base::Unretained(strip.get())));
  return strip;
}

void DownloadsLibraryView::SelectTab(Tab tab) {
  if (tab == tab_) {
    return;
  }
  tab_ = tab;
  downloads_page_->SetVisible(tab == Tab::kDownloads);
  screenshots_page_->SetVisible(tab == Tab::kScreenshots);
  tab_callbacks_.Notify(tab);
  PreferredSizeChanged();
}

void DownloadsLibraryView::Refresh() {
  if (listing_) {
    list_again_ = true;
    return;
  }
  listing_ = true;
  const base::FilePath download_folder =
      DownloadPrefs::FromBrowserContext(profile_)->DownloadPath();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](const base::FilePath& download_folder) {
            auto listing = std::make_unique<Listing>();
            listing->download_folder = download_folder;
            listing->screenshot_folder = downloads_folder::ScreenshotFolder();
            const base::FilePath real_downloads =
                base::MakeAbsoluteFilePath(download_folder);
            const base::FilePath real_screenshots =
                base::MakeAbsoluteFilePath(listing->screenshot_folder);
            listing->same_folder =
                !real_downloads.empty() && real_downloads == real_screenshots;
            listing->downloads = downloads_folder::ListFolder(download_folder);
            listing->screenshots =
                listing->same_folder
                    ? listing->downloads
                    : downloads_folder::ListFolder(listing->screenshot_folder);
            return listing;
          },
          download_folder),
      base::BindOnce(&DownloadsLibraryView::OnListed,
                     weak_factory_.GetWeakPtr()));
}

void DownloadsLibraryView::RefreshSoon() {
  refresh_timer_.Start(FROM_HERE, kRefreshDelay,
                       base::BindOnce(&DownloadsLibraryView::Refresh,
                                      base::Unretained(this)));
}

void DownloadsLibraryView::OnListed(std::unique_ptr<Listing> listing) {
  listing_ = false;
  if (listing->download_folder != download_folder_ ||
      listing->screenshot_folder != screenshot_folder_) {
    download_folder_ = listing->download_folder;
    screenshot_folder_ = listing->screenshot_folder;
    std::vector<base::FilePath> folders = {download_folder_};
    if (!listing->same_folder) {
      folders.push_back(screenshot_folder_);
    }
    watch_.Reset();
    watch_ = base::SequenceBound<FolderWatch>(
        base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE}),
        std::move(folders),
        base::BindPostTask(
            base::SequencedTaskRunner::GetCurrentDefault(),
            base::BindRepeating(&DownloadsLibraryView::RefreshSoon,
                                weak_factory_.GetWeakPtr())));
  }
  same_folder_ = listing->same_folder;
  downloads_files_ = std::move(listing->downloads);
  screenshot_files_ = downloads_folder::ScreenshotsTabFiles(
      std::move(listing->screenshots), kScreenshotLimit);
  RebuildDownloads();
  RebuildScreenshots();
  PreferredSizeChanged();
  if (on_refreshed_for_testing_) {
    on_refreshed_for_testing_.Run();
  }
  if (list_again_) {
    list_again_ = false;
    Refresh();
  }
}

void DownloadsLibraryView::RebuildDownloads() {
  const base::flat_set<base::FilePath> browser_paths =
      row_paths_ ? row_paths_.Run() : base::flat_set<base::FilePath>();
  std::vector<FolderFile> files = downloads_folder::DownloadsTabFiles(
      downloads_files_, browser_paths,
      /*hide_screen_captures=*/same_folder_, kFolderFileLimit);
  const bool has_browser_rows = browser_rows_ && !browser_paths.empty();

  folder_rows_->RemoveAllChildViews();
  const base::RepeatingClosure changed = base::BindRepeating(
      &DownloadsLibraryView::Refresh, weak_factory_.GetWeakPtr());
  // Hairlines start where the rows' names do.
  const int text_start =
      GetLayoutInsets(DOWNLOAD_ROW).left() + kRowIconSize +
      ChromeLayoutProvider::Get()->GetDistanceMetric(
          views::DISTANCE_RELATED_LABEL_HORIZONTAL);
  for (FolderFile& file : files) {
    if (!folder_rows_->children().empty()) {
      folder_rows_->AddChildView(std::make_unique<Hairline>(text_start));
    }
    folder_rows_->AddChildView(std::make_unique<FolderFileRow>(
        profile_, std::move(file), thumbnails_, changed));
  }
  recent_caption_->SetVisible(has_browser_rows);
  recent_card_->SetVisible(has_browser_rows);
  folder_caption_->SetVisible(!files.empty());
  folder_rows_->SetVisible(!files.empty());
  downloads_empty_->SetVisible(!has_browser_rows && files.empty());
  ApplyLook();
}

void DownloadsLibraryView::RebuildScreenshots() {
  grid_->RemoveAllChildViews();
  const int tile_width =
      (DialogWidth() - (kGridColumns - 1) * kGridGap) / kGridColumns;
  const gfx::Size tile_size(tile_width, tile_width * 10 / 16);
  const base::RepeatingClosure changed = base::BindRepeating(
      &DownloadsLibraryView::Refresh, weak_factory_.GetWeakPtr());

  views::BoxLayoutView* row = nullptr;
  for (size_t i = 0; i < screenshot_files_.size(); ++i) {
    if (i % kGridColumns == 0) {
      row = grid_->AddChildView(std::make_unique<views::BoxLayoutView>());
      row->SetOrientation(views::BoxLayout::Orientation::kHorizontal);
      row->SetBetweenChildSpacing(kGridGap);
    }
    row->AddChildView(std::make_unique<ScreenshotTile>(
        profile_, screenshot_files_[i], tile_size, thumbnails_, changed));
  }
  grid_->SetVisible(!screenshot_files_.empty());
  screenshots_caption_->SetText(
      l10n_util::GetStringFUTF16(IDS_COMPASS_DOWNLOADS_SCREENSHOTS_IN,
                                 FolderDisplayName(screenshot_folder_)));
  screenshots_empty_->SetText(
      l10n_util::GetStringFUTF16(IDS_COMPASS_DOWNLOADS_NO_SCREENSHOTS_SAVED_TO,
                                 FolderDisplayName(screenshot_folder_)));
  screenshots_empty_->SetVisible(screenshot_files_.empty());
}

void DownloadsLibraryView::OpenScreenshotFolder() {
  if (screenshot_folder_.empty()) {
    return;
  }
  platform_util::OpenItem(profile_, screenshot_folder_,
                          platform_util::OPEN_FOLDER, base::DoNothing());
  if (GetWidget()) {
    GetWidget()->Close();
  }
}

size_t DownloadsLibraryView::folder_row_count_for_testing() const {
  return std::ranges::count_if(folder_rows_->children(), [](views::View* v) {
    return views::IsViewClass<FolderFileRow>(v);
  });
}

size_t DownloadsLibraryView::screenshot_count_for_testing() const {
  return screenshot_files_.size();
}

BEGIN_METADATA(DownloadsLibraryView)
END_METADATA

}  // namespace compass::downloads_bubble
