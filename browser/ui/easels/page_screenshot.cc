// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/easels/page_screenshot.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "compass/browser/claude/devtools_session.h"
#include "compass/browser/downloads_folder/folder_files.h"
#include "compass/strings/grit/compass_strings.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/codec/png_codec.h"

namespace compass::easels {

namespace {

using Result = base::expected<std::string, std::string>;

// Writes |png| to a free name in the screenshot folder (the system's, where
// its own screenshots go, so the downloads bubble's Screenshots tab lists
// both), marked as a screenshot as the system marks its own; the path
// written, or none.
std::optional<base::FilePath> WritePng(base::FilePath name,
                                       std::vector<uint8_t> png) {
  const base::FilePath folder = downloads_folder::ScreenshotFolder();
  if (folder.empty() || !base::CreateDirectory(folder)) {
    return std::nullopt;
  }
  const base::FilePath path = base::GetUniquePath(folder.Append(name));
  if (path.empty() || !base::WriteFile(path, png)) {
    return std::nullopt;
  }
  downloads_folder::MarkAsScreenCapture(path);
  return path;
}

void SavePng(std::vector<uint8_t> png, ScreenshotCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&WritePng, ScreenshotFileName(base::Time::Now()),
                     std::move(png)),
      base::BindOnce(
          [](ScreenshotCallback done, std::optional<base::FilePath> path) {
            if (!path) {
              std::move(done).Run(base::unexpected(l10n_util::GetStringUTF8(
                  IDS_COMPASS_SCREENSHOT_SAVE_FAILED)));
              return;
            }
            // "Saved to Desktop as Screenshot …".
            std::move(done).Run(base::ok(l10n_util::GetStringFUTF8(
                IDS_COMPASS_SCREENSHOT_SAVED,
                path->DirName().BaseName().LossyDisplayName(),
                path->BaseName().LossyDisplayName())));
          },
          std::move(done)));
}

void CopyBitmap(const SkBitmap& bitmap, ScreenshotCallback done) {
  {
    ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
    writer.WriteImage(bitmap);
  }
  std::move(done).Run(base::ok(
      std::string(l10n_util::GetStringUTF8(IDS_COMPASS_SCREENSHOT_COPIED))));
}

// One full-page capture: measures the page, then has DevTools render all
// of it. Deletes itself when done, or when the tab goes (the session's
// pending commands then fail).
class FullPageCapture {
 public:
  FullPageCapture(content::WebContents* contents,
                  ScreenshotAction action,
                  ScreenshotCallback done)
      : contents_(contents->GetWeakPtr()),
        action_(action),
        done_(std::move(done)),
        session_(contents, base::DoNothing(), base::DoNothing()) {}

  void Start() {
    session_.Send("Page.getLayoutMetrics", base::DictValue(),
                  base::BindOnce(&FullPageCapture::OnMetrics,
                                 weak_factory_.GetWeakPtr()));
  }

 private:
  void OnMetrics(claude::DevToolsSession::Result metrics) {
    const base::DictValue* size =
        metrics.has_value() ? metrics->FindDict("cssContentSize") : nullptr;
    const double width = size ? size->FindDouble("width").value_or(0) : 0;
    const double height = size ? size->FindDouble("height").value_or(0) : 0;
    if (width < 1 || height < 1) {
      Fail(l10n_util::GetStringUTF8(IDS_COMPASS_SCREENSHOT_NO_SIZE));
      return;
    }
    clipped_ = height > kMaxFullPageHeight;
    session_.Send(
        "Page.captureScreenshot",
        base::DictValue()
            .Set("format", "png")
            .Set("captureBeyondViewport", true)
            .Set("clip", base::DictValue()
                             .Set("x", 0)
                             .Set("y", 0)
                             .Set("width", width)
                             .Set("height",
                                  std::min<double>(height, kMaxFullPageHeight))
                             .Set("scale", 1)),
        base::BindOnce(&FullPageCapture::OnShot, weak_factory_.GetWeakPtr()));
  }

  void OnShot(claude::DevToolsSession::Result shot) {
    const std::string* data =
        shot.has_value() ? shot->FindString("data") : nullptr;
    std::optional<std::vector<uint8_t>> png =
        data ? base::Base64Decode(*data) : std::nullopt;
    if (!png || !contents_) {
      Fail(l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_CAPTURE_FAILED));
      return;
    }
    ScreenshotCallback done = base::BindOnce(
        [](bool clipped, ScreenshotCallback done, Result result) {
          if (result.has_value() && clipped) {
            result = base::ok(l10n_util::GetStringFUTF8(
                IDS_COMPASS_SCREENSHOT_CLIPPED, base::UTF8ToUTF16(*result),
                base::NumberToString16(kMaxFullPageHeight)));
          }
          std::move(done).Run(std::move(result));
        },
        clipped_, std::move(done_));
    if (action_ == ScreenshotAction::kSave) {
      SavePng(std::move(*png), std::move(done));
    } else {
      SkBitmap bitmap = gfx::PNGCodec::Decode(*png);
      if (bitmap.drawsNothing()) {
        std::move(done).Run(base::unexpected(
            l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_CAPTURE_FAILED)));
      } else {
        CopyBitmap(bitmap, std::move(done));
      }
    }
    End();
  }

  void Fail(std::string_view why) {
    std::move(done_).Run(base::unexpected(std::string(why)));
    End();
  }

  // Later: the session may be in the middle of dispatching the reply.
  void End() {
    weak_factory_.InvalidateWeakPtrs();
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  base::WeakPtr<content::WebContents> contents_;
  const ScreenshotAction action_;
  ScreenshotCallback done_;
  claude::DevToolsSession session_;
  bool clipped_ = false;
  base::WeakPtrFactory<FullPageCapture> weak_factory_{this};
};

}  // namespace

void FinishScreenshot(content::WebContents* contents,
                      ScreenshotAction action,
                      SkBitmap bitmap,
                      ScreenshotCallback done) {
  if (action == ScreenshotAction::kCopy) {
    CopyBitmap(bitmap, std::move(done));
    return;
  }
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  if (!png) {
    std::move(done).Run(base::unexpected(
        l10n_util::GetStringUTF8(IDS_COMPASS_SCREENSHOT_SAVE_FAILED)));
    return;
  }
  SavePng(std::move(*png), std::move(done));
}

void CaptureFullPage(content::WebContents* contents,
                     ScreenshotAction action,
                     ScreenshotCallback done) {
  (new FullPageCapture(contents, action, std::move(done)))->Start();
}

base::FilePath ScreenshotFileName(base::Time time) {
  base::Time::Exploded t;
  time.LocalExplode(&t);
  return base::FilePath::FromUTF8Unsafe(base::StringPrintf(
      "Screenshot %04d-%02d-%02d at %02d.%02d.%02d.png", t.year, t.month,
      t.day_of_month, t.hour, t.minute, t.second));
}

}  // namespace compass::easels
