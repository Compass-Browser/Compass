// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/strings/strcat.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/easels/easel_capture.h"
#include "compass/browser/ui/easels/easel_capture_core.h"
#include "compass/browser/ui/easels/easel_refresh.h"
#include "compass/browser/window_sync/window_features.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "url/gurl.h"

namespace compass::easels {
namespace {

using ObjectResult = base::expected<sync_pb::CompassEaselObject, std::string>;

// Close enough to |expected| for a JPEG.
bool IsColor(SkColor actual, SkColor expected) {
  auto near = [](int a, int b) { return std::abs(a - b) < 24; };
  return near(SkColorGetR(actual), SkColorGetR(expected)) &&
         near(SkColorGetG(actual), SkColorGetG(expected)) &&
         near(SkColorGetB(actual), SkColorGetB(expected));
}

class EaselCaptureBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    // /color is red the first time and blue after: Refresh sees the change.
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        [](std::atomic<int>* loads,
           const net::test_server::HttpRequest& request)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          if (request.relative_url != "/color") {
            return nullptr;
          }
          const bool first = loads->fetch_add(1) == 0;
          auto response =
              std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_content_type("text/html");
          response->set_content(base::StrCat(
              {"<!doctype html><title>Color</title><body style='margin:0;"
               "height:3000px;background:",
               first ? "rgb(255,0,0)" : "rgb(0,0,255)", "'></body>"}));
          return response;
        },
        &loads_));
    ASSERT_TRUE(embedded_test_server()->Start());
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("/color")));
    base::RunLoop loop;
    store()->WhenLoaded(loop.QuitClosure());
    loop.Run();
  }

  content::WebContents* tab() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  EaselStore* store() { return EaselStore::Get(browser()->GetProfile()); }

  PageView Probe() {
    base::test::TestFuture<base::Value> future;
    RunPageScript(tab(), "__compassEasel.probe()", future.GetCallback());
    std::optional<PageView> view = PageViewFromValue(future.Take());
    EXPECT_TRUE(view.has_value());
    return view.value_or(PageView());
  }

  SkBitmap ImageOf(const sync_pb::CompassEaselObject& object) {
    const sync_pb::CompassEaselAsset* asset =
        store()->FindAsset(object.capture().image_asset());
    if (!asset) {
      return SkBitmap();
    }
    const auto bytes = base::as_byte_span(asset->data());
    return asset->mime_type() == "image/png" ? gfx::PNGCodec::Decode(bytes)
                                             : gfx::JPEGCodec::Decode(bytes);
  }

  float Scale() {
    return tab()->GetRenderWidgetHostView()->GetDeviceScaleFactor();
  }

  std::atomic<int> loads_{0};
};

IN_PROC_BROWSER_TEST_F(EaselCaptureBrowserTest, AddsARegionToABoard) {
  auto easel = store()->CreateEasel("Colors");
  ASSERT_TRUE(easel.has_value());
  CaptureRequest request;
  request.easel_uuid = easel->uuid();
  request.region = gfx::RectF(10, 20, 120, 60);
  request.view = Probe();
  base::test::TestFuture<ObjectResult> future;
  AddCaptureToEasel(tab(), request, future.GetCallback());
  const ObjectResult saved = future.Take();
  ASSERT_TRUE(saved.has_value()) << saved.error();

  // At its CSS size, at the edge of the empty board.
  EXPECT_EQ(40, saved->x());
  EXPECT_EQ(40, saved->y());
  EXPECT_EQ(120, saved->width());
  EXPECT_EQ(60, saved->height());
  const sync_pb::CompassEaselCapture& capture = saved->capture();
  EXPECT_EQ(embedded_test_server()->GetURL("/color").spec(), capture.url());
  EXPECT_EQ("Color", capture.title());
  EXPECT_EQ(10, capture.region_x());
  EXPECT_EQ(20, capture.region_y());
  EXPECT_GT(capture.viewport_width(), 0);
  EXPECT_GT(capture.capture_time(), 0);
  EXPECT_EQ(easel->uuid(), GetLastUsedEasel(browser()->GetProfile()));

  // The region in device pixels, of the page's color.
  const SkBitmap image = ImageOf(*saved);
  ASSERT_FALSE(image.drawsNothing());
  EXPECT_NEAR(120 * Scale(), image.width(), 1);
  EXPECT_NEAR(60 * Scale(), image.height(), 1);
  EXPECT_TRUE(IsColor(image.getColor(image.width() / 2, image.height() / 2),
                      SK_ColorRED));

  // The next one goes right of it.
  base::test::TestFuture<ObjectResult> second;
  AddCaptureToEasel(tab(), request, second.GetCallback());
  const ObjectResult next = second.Take();
  ASSERT_TRUE(next.has_value()) << next.error();
  EXPECT_EQ(40 + 120 + 40, next->x());
  EXPECT_EQ(40, next->y());
  EXPECT_GT(next->z(), saved->z());
}

IN_PROC_BROWSER_TEST_F(EaselCaptureBrowserTest, NewBoardOnlyOnceTheImageIsIn) {
  const size_t before = store()->Easels().size();
  CaptureRequest request;
  request.region = gfx::RectF(0, 0, 50, 50);
  request.view = Probe();
  base::test::TestFuture<ObjectResult> future;
  AddCaptureToEasel(tab(), request, future.GetCallback());
  const ObjectResult saved = future.Take();
  ASSERT_TRUE(saved.has_value()) << saved.error();
  ASSERT_EQ(before + 1, store()->Easels().size());
  const sync_pb::CompassEasel* easel = store()->FindEasel(saved->easel_uuid());
  ASSERT_TRUE(easel);
  EXPECT_EQ("Untitled Easel", easel->title());

  // Outside the page: no board, no object.
  request.region = gfx::RectF(-500, -500, 10, 10);
  base::test::TestFuture<ObjectResult> outside;
  AddCaptureToEasel(tab(), request, outside.GetCallback());
  EXPECT_FALSE(outside.Take().has_value());
  EXPECT_EQ(before + 1, store()->Easels().size());
}

IN_PROC_BROWSER_TEST_F(EaselCaptureBrowserTest, RefreshTakesItAgain) {
  auto easel = store()->CreateEasel("Colors");
  ASSERT_TRUE(easel.has_value());
  // Scrolled down, so Refresh has to scroll too.
  ASSERT_TRUE(content::ExecJs(tab(), "window.scrollTo(0, 500)"));
  CaptureRequest request;
  request.easel_uuid = easel->uuid();
  request.region = gfx::RectF(20, 20, 80, 40);
  request.view = Probe();
  EXPECT_EQ(500, request.view.scroll_y);
  base::test::TestFuture<ObjectResult> future;
  AddCaptureToEasel(tab(), request, future.GetCallback());
  const ObjectResult saved = future.Take();
  ASSERT_TRUE(saved.has_value()) << saved.error();
  EXPECT_EQ(500, saved->capture().scroll_y());

  base::test::TestFuture<std::string> refreshed;
  RefreshCapture(browser()->GetProfile(), saved->uuid(), refreshed.GetCallback());
  EXPECT_EQ("", refreshed.Take());

  const sync_pb::CompassEaselObject* object = store()->FindObject(saved->uuid());
  ASSERT_TRUE(object);
  EXPECT_NE(saved->capture().image_asset(), object->capture().image_asset());
  EXPECT_GE(object->capture().capture_time(), saved->capture().capture_time());
  // Where it was, the same size, now blue.
  EXPECT_EQ(saved->x(), object->x());
  const SkBitmap image = ImageOf(*object);
  ASSERT_FALSE(image.drawsNothing());
  EXPECT_TRUE(IsColor(image.getColor(image.width() / 2, image.height() / 2),
                      SK_ColorBLUE));
}

IN_PROC_BROWSER_TEST_F(EaselCaptureBrowserTest, RefreshOfNothingSaysSo) {
  base::test::TestFuture<std::string> refreshed;
  RefreshCapture(browser()->GetProfile(), "no-such-object",
                 refreshed.GetCallback());
  EXPECT_FALSE(refreshed.Take().empty());
}

IN_PROC_BROWSER_TEST_F(EaselCaptureBrowserTest, OverlayDragAndPick) {
  auto easel = store()->CreateEasel("Picked");
  ASSERT_TRUE(easel.has_value());
  SetLastUsedEasel(browser()->GetProfile(), easel->uuid());
  ASSERT_TRUE(CanCaptureToEasel(tab()));
  StartCaptureToEasel(tab());
  // The overlay's host is in the page, its insides in a closed shadow root.
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(tab(), "!!document.querySelector('[popover]')")
        .ExtractBool();
  }));

  using blink::WebInputEvent;
  content::SimulateMouseEvent(tab(), WebInputEvent::Type::kMouseDown,
                              blink::WebMouseEvent::Button::kLeft,
                              gfx::Point(30, 40));
  content::SimulateMouseEvent(tab(), WebInputEvent::Type::kMouseMove,
                              blink::WebMouseEvent::Button::kLeft,
                              gfx::Point(90, 70));
  content::SimulateMouseEvent(tab(), WebInputEvent::Type::kMouseMove,
                              blink::WebMouseEvent::Button::kLeft,
                              gfx::Point(130, 90));
  content::SimulateMouseEvent(tab(), WebInputEvent::Type::kMouseUp,
                              blink::WebMouseEvent::Button::kLeft,
                              gfx::Point(130, 90));
  // The picker focuses its first board, the last used: Enter picks it.
  content::SimulateKeyPress(tab(), ui::DomKey::ENTER, ui::DomCode::ENTER,
                            ui::VKEY_RETURN, false, false, false, false);
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return store()->ObjectsOf(easel->uuid()).size() == 1; }));
  const sync_pb::CompassEaselObject& object =
      *store()->ObjectsOf(easel->uuid())[0];
  EXPECT_EQ(100, object.width());
  EXPECT_EQ(50, object.height());
  EXPECT_EQ(30, object.capture().region_x());
  EXPECT_EQ(40, object.capture().region_y());
  // The overlay is gone; the confirmation is up for a moment.
  EXPECT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(tab(),
                           "document.querySelectorAll('[popover]').length")
               .ExtractInt() <= 1;
  }));
}

IN_PROC_BROWSER_TEST_F(EaselCaptureBrowserTest, EscapeCancels) {
  const size_t before = store()->Easels().size();
  StartCaptureToEasel(tab());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(tab(), "!!document.querySelector('[popover]')")
        .ExtractBool();
  }));
  content::SimulateKeyPress(tab(), ui::DomKey::ESCAPE, ui::DomCode::ESCAPE,
                            ui::VKEY_ESCAPE, false, false, false, false);
  EXPECT_TRUE(base::test::RunUntil([&] {
    return !content::EvalJs(tab(), "!!document.querySelector('[popover]')")
                .ExtractBool();
  }));
  EXPECT_EQ(before, store()->Easels().size());
}

IN_PROC_BROWSER_TEST_F(EaselCaptureBrowserTest, NewEaselPinsATab) {
  const size_t before = store()->Easels().size();
  NewEaselInWindow(browser());
  ASSERT_EQ(before + 1, store()->Easels().size());
  const std::string uuid = store()->Easels().front()->uuid();
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  ASSERT_FALSE(space.pinned_root()->children().empty());
  const SidebarNode* node = space.pinned_root()->children().back().get();
  EXPECT_EQ(uuid, EaselUuidFromUrl(node->saved_url().spec()));
  EXPECT_EQ(u"Untitled Easel", node->saved_title());
}

}  // namespace
}  // namespace compass::easels
