// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/easels/easel_capture_core.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "compass/browser/easels/easel_images.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/ui/easels/easel_page_js.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/image/image.h"

namespace compass::easels {

namespace {

// How long viz may take to answer a copy.
constexpr base::TimeDelta kCopyTimeout = base::Seconds(5);

constexpr char kLastUsedKey[] = "compass_easels_last_used";

struct LastUsed : public base::SupportsUserData::Data {
  std::string easel_uuid;
};

// A capture on its way to the board.
struct CaptureState {
  // The profile's: while it's there, so is the profile.
  base::WeakPtr<EaselStore> store;
  raw_ptr<Profile> profile = nullptr;
  CaptureRequest request;
  std::string url;
  std::string title;
  SkBitmap favicon;
  std::string image_sha;
  ObjectCallback done;
};

void Fail(std::unique_ptr<CaptureState> state, std::string error) {
  std::move(state->done).Run(base::unexpected(std::move(error)));
}

void Save(std::unique_ptr<CaptureState> state, std::string favicon_sha) {
  EaselStore* store = state->store.get();
  if (!store) {
    return Fail(std::move(state),
                l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_PROFILE_CLOSED));
  }
  std::string easel_uuid = state->request.easel_uuid;
  if (easel_uuid.empty()) {
    base::expected<sync_pb::CompassEasel, std::string> easel =
        store->CreateEasel(NewEaselTitle());
    if (!easel.has_value()) {
      return Fail(std::move(state), easel.error());
    }
    easel_uuid = easel->uuid();
  } else if (!store->FindEasel(easel_uuid)) {
    return Fail(std::move(state),
                l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_DELETED));
  }

  std::vector<gfx::RectF> boxes;
  double z = 0;
  for (const sync_pb::CompassEaselObject* object :
       store->ObjectsOf(easel_uuid)) {
    boxes.push_back(
        ToRectF(object->x(), object->y(), object->width(), object->height()));
    z = std::max(z, object->z() + 1);
  }
  const gfx::PointF origin = PlaceNewObject(boxes);
  const gfx::RectF& region = state->request.region;
  const PageView& view = state->request.view;

  sync_pb::CompassEaselObject object;
  object.set_easel_uuid(easel_uuid);
  object.set_x(origin.x());
  object.set_y(origin.y());
  object.set_width(region.width());
  object.set_height(region.height());
  object.set_z(z);
  sync_pb::CompassEaselCapture& capture = *object.mutable_capture();
  capture.set_url(state->url);
  capture.set_title(state->title);
  capture.set_image_asset(state->image_sha);
  if (!favicon_sha.empty()) {
    capture.set_favicon_asset(favicon_sha);
  }
  capture.set_region_x(region.x());
  capture.set_region_y(region.y());
  capture.set_region_width(region.width());
  capture.set_region_height(region.height());
  capture.set_scroll_x(view.scroll_x);
  capture.set_scroll_y(view.scroll_y);
  capture.set_viewport_width(view.viewport_width);
  capture.set_viewport_height(view.viewport_height);
  capture.set_capture_time(ToSyncTime(base::Time::Now()));

  base::expected<sync_pb::CompassEaselObject, std::string> saved =
      store->SaveObject(std::move(object));
  if (saved.has_value()) {
    SetLastUsedEasel(state->profile, easel_uuid);
  }
  std::move(state->done).Run(std::move(saved));
}

void OnFaviconPrepared(std::unique_ptr<CaptureState> state,
                       base::expected<PreparedImage, std::string> prepared) {
  std::string sha;
  // A capture without its icon is still worth keeping.
  if (prepared.has_value() && state->store) {
    sha = state->store->AddAsset(std::move(prepared->data),
                                 std::move(prepared->mime_type))
              .value_or(std::string());
  }
  Save(std::move(state), std::move(sha));
}

void OnImagePrepared(std::unique_ptr<CaptureState> state,
                     base::expected<PreparedImage, std::string> prepared) {
  if (!prepared.has_value()) {
    return Fail(std::move(state), prepared.error());
  }
  if (!state->store) {
    return Fail(std::move(state),
                l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_PROFILE_CLOSED));
  }
  base::expected<std::string, std::string> sha = state->store->AddAsset(
      std::move(prepared->data), std::move(prepared->mime_type));
  if (!sha.has_value()) {
    return Fail(std::move(state), sha.error());
  }
  state->image_sha = std::move(*sha);
  if (state->favicon.drawsNothing()) {
    return Save(std::move(state), std::string());
  }
  const SkBitmap favicon = state->favicon;
  PrepareBitmap(favicon,
                base::BindOnce(&OnFaviconPrepared, std::move(state)));
}

void OnCopied(std::unique_ptr<CaptureState> state,
              base::expected<SkBitmap, std::string> bitmap) {
  if (!bitmap.has_value()) {
    return Fail(std::move(state), bitmap.error());
  }
  PrepareBitmap(*bitmap, base::BindOnce(&OnImagePrepared, std::move(state)));
}

}  // namespace

void RunPageScript(content::WebContents* contents,
                   std::string_view call,
                   base::OnceCallback<void(base::Value)> callback,
                   bool define) {
  content::RenderFrameHost* frame =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (!frame || !frame->IsRenderFrameLive()) {
    std::move(callback).Run(base::Value());
    return;
  }
  const std::string script =
      define ? base::StrCat({assets::kEaselPageJs, "\n;\n", call})
             : base::StrCat({"window.__compassEasel ? ", call,
                             " : ({state: 'gone'})"});
  // The isolated world: the page's scripts can't see or change ours, and
  // its CSP doesn't apply.
  frame->ExecuteJavaScriptInIsolatedWorld(base::UTF8ToUTF16(script),
                                          std::move(callback),
                                          ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

void CopyPageRegion(content::WebContents* contents,
                    const gfx::RectF& css_region,
                    const PageView& view,
                    BitmapCallback callback) {
  content::RenderWidgetHostView* widget_view =
      contents ? contents->GetRenderWidgetHostView() : nullptr;
  if (!widget_view || !widget_view->IsSurfaceAvailableForCopy()) {
    std::move(callback).Run(base::unexpected(
        l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_NOTHING_TO_CAPTURE)));
    return;
  }
  const gfx::Rect rect =
      RegionToViewRect(css_region, view, widget_view->GetDeviceScaleFactor(),
                       widget_view->GetViewBounds().size());
  if (rect.IsEmpty()) {
    std::move(callback).Run(base::unexpected(
        l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_REGION_OUTSIDE)));
    return;
  }
  // An empty output size: the copy keeps the surface's device pixels.
  widget_view->CopyFromSurface(
      rect, gfx::Size(), kCopyTimeout,
      base::BindOnce(
          [](BitmapCallback callback,
             const content::CopyFromSurfaceResult& result) {
            if (!result.has_value() || result->bitmap.drawsNothing()) {
              std::move(callback).Run(base::unexpected(
                  l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_CAPTURE_FAILED)));
              return;
            }
            std::move(callback).Run(result->bitmap);
          },
          std::move(callback)));
}

void AddCaptureToEasel(content::WebContents* contents,
                       CaptureRequest request,
                       ObjectCallback done) {
  Profile* profile =
      Profile::FromBrowserContext(contents->GetBrowserContext());
  EaselStore* store = EaselStore::Get(profile);
  if (!store) {
    std::move(done).Run(base::unexpected(
        l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_UNAVAILABLE)));
    return;
  }
  if (!store->loaded()) {
    std::move(done).Run(base::unexpected(
        l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_STILL_LOADING)));
    return;
  }
  auto state = std::make_unique<CaptureState>();
  state->store = store->GetWeakPtr();
  state->profile = profile;
  state->request = std::move(request);
  state->url = contents->GetLastCommittedURL().spec();
  state->title = base::UTF16ToUTF8(contents->GetTitle());
  state->done = std::move(done);
  if (auto* driver =
          favicon::ContentFaviconDriver::FromWebContents(contents)) {
    const gfx::Image icon = driver->GetFavicon();
    if (!icon.IsEmpty()) {
      state->favicon = icon.AsBitmap();
    }
  }
  const gfx::RectF region = state->request.region;
  const PageView view = state->request.view;
  CopyPageRegion(contents, region, view,
                 base::BindOnce(&OnCopied, std::move(state)));
}

std::string GetLastUsedEasel(Profile* profile) {
  auto* data = static_cast<LastUsed*>(profile->GetUserData(kLastUsedKey));
  return data ? data->easel_uuid : std::string();
}

void SetLastUsedEasel(Profile* profile, std::string_view easel_uuid) {
  auto data = std::make_unique<LastUsed>();
  data->easel_uuid = std::string(easel_uuid);
  profile->SetUserData(kLastUsedKey, std::move(data));
}

std::vector<const sync_pb::CompassEasel*> EaselsForPicker(Profile* profile) {
  EaselStore* store = EaselStore::Get(profile);
  if (!store) {
    return {};
  }
  std::vector<const sync_pb::CompassEasel*> easels = store->Easels();
  const std::string last = GetLastUsedEasel(profile);
  auto it = std::ranges::find_if(
      easels, [&last](const sync_pb::CompassEasel* easel) {
        return easel->uuid() == last;
      });
  if (it != easels.end()) {
    std::rotate(easels.begin(), it, it + 1);
  }
  return easels;
}

std::string EaselTitleOf(const sync_pb::CompassEasel& easel) {
  return easel.title().empty() ? NewEaselTitle() : easel.title();
}

}  // namespace compass::easels
