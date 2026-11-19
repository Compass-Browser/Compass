// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/easels/easel_refresh.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "compass/browser/easels/easel_images.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/ui/easels/easel_capture_core.h"
#include "compass/browser/ui/easels/easel_geometry.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/page_transition_types.h"
#include "ui/gfx/geometry/rect.h"
#include "url/gurl.h"

#if defined(USE_AURA)
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "ui/aura/window.h"
#include "ui/base/base_window.h"
#endif

namespace compass::easels {

namespace {

// After the load: the spec's "load plus 1.5 s" for late content.
constexpr base::TimeDelta kSettleDelay = base::Milliseconds(1500);
// After the scroll, for a frame or two at the new offset.
constexpr base::TimeDelta kScrollDelay = base::Milliseconds(300);
// A page whose load never ends (a stream, a stuck request) is taken anyway
// after this.
constexpr base::TimeDelta kLoadCap = base::Seconds(20);
// The whole refresh.
constexpr base::TimeDelta kTimeout = base::Seconds(45);

// A capture without a saved viewport (Arc's, say) gets a common window's.
constexpr int kDefaultViewportWidth = 1280;
constexpr int kDefaultViewportHeight = 800;
constexpr int kMinViewport = 200;
constexpr int kMaxViewport = 4096;

int ViewportSide(double saved, double region_end, int fallback) {
  int side = saved > 0 ? static_cast<int>(saved + 0.5) : fallback;
  // The region must fit in it.
  side = std::max(side, static_cast<int>(region_end + 0.5));
  return std::clamp(side, kMinViewport, kMaxViewport);
}

#if defined(USE_AURA)
// Aura draws a page only in a window tree (as OffscreenTab does): the page
// goes into a browser window's root window, below everything and outside
// its bounds, so it never shows.
void AdoptOffscreen(content::WebContents* contents, const gfx::Size& size) {
  aura::Window* window = contents->GetNativeView();
  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  ui::BaseWindow* base = browser ? browser->GetWindow() : nullptr;
  aura::Window* native = base ? base->GetNativeWindow() : nullptr;
  aura::Window* root = native ? native->GetRootWindow() : nullptr;
  if (!window || !root) {
    return;
  }
  root->AddChild(window);
  root->StackChildAtBottom(window);
  window->SetBounds(gfx::Rect(
      gfx::Point(-size.width() - 100, -size.height() - 100), size));
}
#endif

// One refresh: its page, and the steps through to the new image. Deletes
// itself when done.
class RefreshJob : public content::WebContentsObserver,
                   public ProfileObserver {
 public:
  using Done = base::OnceCallback<void(std::string)>;

  RefreshJob(Profile* profile,
             EaselStore* store,
             sync_pb::CompassEaselObject object,
             Done done)
      : profile_(profile),
        store_(store->GetWeakPtr()),
        object_(std::move(object)),
        done_(std::move(done)) {
    profile_observation_.Observe(profile);
  }
  RefreshJob(const RefreshJob&) = delete;
  RefreshJob& operator=(const RefreshJob&) = delete;
  ~RefreshJob() override = default;

  void Start() {
    const sync_pb::CompassEaselCapture& capture = object_.capture();
    const gfx::Size size(
        ViewportSide(capture.viewport_width(),
                     capture.region_x() + capture.region_width(),
                     kDefaultViewportWidth),
        ViewportSide(capture.viewport_height(),
                     capture.region_y() + capture.region_height(),
                     kDefaultViewportHeight));

    content::WebContents::CreateParams params(profile_);
    params.initially_hidden = true;
    contents_ = content::WebContents::Create(params);
    contents_->SetOwnerLocationForDebug(FROM_HERE);
    contents_->SetAudioMuted(true);
    contents_->Resize(gfx::Rect(size));
#if defined(USE_AURA)
    AdoptOffscreen(contents_.get(), size);
#endif
    Observe(contents_.get());
    // Frames come although no window shows the page; told it's visible, the
    // page renders as for the user (animation frames, lazy images).
    capture_ = contents_->IncrementCapturerCount(
        gfx::Size(), /*stay_hidden=*/false, /*stay_awake=*/false,
        /*is_activity=*/false);

    timeout_.Start(FROM_HERE, kTimeout,
                   base::BindOnce(&RefreshJob::Finish, base::Unretained(this),
                                  "The page took too long to load."));
    load_cap_.Start(
        FROM_HERE, kLoadCap,
        base::BindOnce(&RefreshJob::Loaded, base::Unretained(this)));
    content::NavigationController::LoadURLParams load(GURL(capture.url()));
    load.transition_type = ui::PAGE_TRANSITION_AUTO_TOPLEVEL;
    contents_->GetController().LoadURLWithParams(load);
  }

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (handle->IsInPrimaryMainFrame() && handle->HasCommitted() &&
        handle->IsErrorPage()) {
      Finish("The page couldn't be loaded.");
    }
  }

  void DocumentOnLoadCompletedInPrimaryMainFrame() override { Loaded(); }

  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override {
    Finish("The page stopped working while it loaded.");
  }

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override {
    // The page can't outlive its profile.
    Finish("The profile was closed.");
    Observe(nullptr);
    capture_.RunAndReset();
    contents_.reset();
    profile_observation_.Reset();
  }

 private:
  void Loaded() {
    if (loaded_ || finished_) {
      return;
    }
    loaded_ = true;
    load_cap_.Stop();
    step_.Start(FROM_HERE, kSettleDelay,
                base::BindOnce(&RefreshJob::Scroll, base::Unretained(this)));
  }

  void Scroll() {
    const sync_pb::CompassEaselCapture& capture = object_.capture();
    RunPageScript(
        contents_.get(),
        base::StrCat({"__compassEasel.scrollTo(",
                      base::NumberToString(capture.scroll_x()), ", ",
                      base::NumberToString(capture.scroll_y()), ")"}),
        base::BindOnce(&RefreshJob::OnScrolled, weak_factory_.GetWeakPtr()));
  }

  void OnScrolled(base::Value) {
    step_.Start(FROM_HERE, kScrollDelay,
                base::BindOnce(&RefreshJob::Probe, base::Unretained(this)));
  }

  void Probe() {
    RunPageScript(
        contents_.get(), "__compassEasel.probe()",
        base::BindOnce(&RefreshJob::OnProbed, weak_factory_.GetWeakPtr()));
  }

  void OnProbed(base::Value value) {
    const std::optional<PageView> view = PageViewFromValue(value);
    if (!view) {
      Finish("The page couldn't be read.");
      return;
    }
    const sync_pb::CompassEaselCapture& capture = object_.capture();
    // Where the page actually scrolled to, which may fall short of the
    // saved offset.
    const gfx::RectF region = ShiftForScroll(
        ToRectF(capture.region_x(), capture.region_y(),
                capture.region_width(), capture.region_height()),
        ToVector2dF(capture.scroll_x(), capture.scroll_y()),
        ToVector2dF(view->scroll_x, view->scroll_y));
    CopyPageRegion(
        contents_.get(), region, *view,
        base::BindOnce(&RefreshJob::OnCopied, weak_factory_.GetWeakPtr()));
  }

  void OnCopied(base::expected<SkBitmap, std::string> bitmap) {
    if (!bitmap.has_value()) {
      Finish(bitmap.error());
      return;
    }
    // The page has done its part.
    Observe(nullptr);
    capture_.RunAndReset();
    contents_.reset();
    PrepareBitmap(*bitmap, base::BindOnce(&RefreshJob::OnPrepared,
                                          weak_factory_.GetWeakPtr()));
  }

  void OnPrepared(base::expected<PreparedImage, std::string> prepared) {
    if (!prepared.has_value()) {
      Finish(prepared.error());
      return;
    }
    EaselStore* store = store_.get();
    if (!store) {
      Finish("The profile was closed.");
      return;
    }
    base::expected<std::string, std::string> sha = store->AddAsset(
        std::move(prepared->data), std::move(prepared->mime_type));
    if (!sha.has_value()) {
      Finish(sha.error());
      return;
    }
    // As it is now: it may have moved or been edited meanwhile.
    const sync_pb::CompassEaselObject* current =
        store->FindObject(object_.uuid());
    if (!current || !current->has_capture()) {
      Finish("The capture was deleted.");
      return;
    }
    sync_pb::CompassEaselObject updated = *current;
    updated.mutable_capture()->set_image_asset(*sha);
    updated.mutable_capture()->set_capture_time(
        ToSyncTime(base::Time::Now()));
    base::expected<sync_pb::CompassEaselObject, std::string> saved =
        store->SaveObject(std::move(updated));
    Finish(saved.has_value() ? std::string() : saved.error());
  }

  void Finish(std::string error) {
    if (finished_) {
      return;
    }
    finished_ = true;
    timeout_.Stop();
    load_cap_.Stop();
    step_.Stop();
    std::move(done_).Run(std::move(error));
    // Not now: this may be inside the page's own notifications.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                               this);
  }

  raw_ptr<Profile> profile_;
  base::WeakPtr<EaselStore> store_;
  sync_pb::CompassEaselObject object_;
  Done done_;
  std::unique_ptr<content::WebContents> contents_;
  base::ScopedClosureRunner capture_;
  base::OneShotTimer timeout_;
  base::OneShotTimer load_cap_;
  base::OneShotTimer step_;
  bool loaded_ = false;
  bool finished_ = false;
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{
      this};
  base::WeakPtrFactory<RefreshJob> weak_factory_{this};
};

}  // namespace

void RefreshCapture(Profile* profile,
                    const std::string& object_uuid,
                    base::OnceCallback<void(std::string error)> done) {
  EaselStore* store = profile ? EaselStore::Get(profile) : nullptr;
  if (!store) {
    std::move(done).Run("Easels aren't available in this window.");
    return;
  }
  const sync_pb::CompassEaselObject* object = store->FindObject(object_uuid);
  if (!object || !object->has_capture()) {
    std::move(done).Run("There's no such capture.");
    return;
  }
  const GURL url(object->capture().url());
  if (!url.is_valid() || !(url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile())) {
    std::move(done).Run("This capture's page can't be loaded again.");
    return;
  }
  // Owned by itself until it's done.
  (new RefreshJob(profile, store, *object, std::move(done)))->Start();
}

}  // namespace compass::easels
