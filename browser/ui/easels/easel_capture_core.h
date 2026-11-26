// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_EASELS_EASEL_CAPTURE_CORE_H_
#define COMPASS_BROWSER_UI_EASELS_EASEL_CAPTURE_CORE_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "compass/browser/ui/easels/easel_geometry.h"
#include "compass/browser/ui/easels/easel_titles.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"
#include "ui/gfx/geometry/rect_f.h"

class Profile;
class SkBitmap;

namespace content {
class WebContents;
}  // namespace content

// What every platform's Capture to Easel and Refresh share
// (docs/design/easels.md, "Captures"): the page script, copying a region of
// a page, and adding a capture to a board. Desktop and Android.
namespace compass::easels {

// Runs |call| (an expression on easel_page.js's __compassEasel, such as
// "__compassEasel.probe()") in the isolated world of |contents|' main frame,
// defining the script there first unless |define| is false (a poll, after
// it was: without it, the value is {state: 'gone'}). |callback| gets its
// value, or none when the page can't run it.
void RunPageScript(content::WebContents* contents,
                   std::string_view call,
                   base::OnceCallback<void(base::Value)> callback,
                   bool define = true);

// Copies |css_region| of what |contents| shows (as RegionToViewRect() maps
// it with |view|) in device pixels.
using BitmapCallback =
    base::OnceCallback<void(base::expected<SkBitmap, std::string>)>;
void CopyPageRegion(content::WebContents* contents,
                    const gfx::RectF& css_region,
                    const PageView& view,
                    BitmapCallback callback);

// A region the user picked on a page, and the board it goes to.
struct CaptureRequest {
  // Empty: a new board (NewEaselTitle()), made once the image is ready, so
  // a capture that fails leaves no empty board behind.
  std::string easel_uuid;
  // CSS pixels of the layout viewport, as |view| was.
  gfx::RectF region;
  PageView view;
};

// Copies the region, keeps it and the page's icon as images, and adds a
// capture of it to the board, at its CSS size, right of what's on the
// board (PlaceNewObject()). |done| gets the saved object, or why not.
using ObjectCallback = base::OnceCallback<void(
    base::expected<sync_pb::CompassEaselObject, std::string>)>;
void AddCaptureToEasel(content::WebContents* contents,
                       CaptureRequest request,
                       ObjectCallback done);

// The board captures last went to in |profile|, this session ("" for
// none), and the boards in the order the picker lists them: that one
// first, then the store's order (newest first).
std::string GetLastUsedEasel(Profile* profile);
void SetLastUsedEasel(Profile* profile, std::string_view easel_uuid);
std::vector<const sync_pb::CompassEasel*> EaselsForPicker(Profile* profile);

// A board's title for messages: its own, or NewEaselTitle().
std::string EaselTitleOf(const sync_pb::CompassEasel& easel);

}  // namespace compass::easels

#endif  // COMPASS_BROWSER_UI_EASELS_EASEL_CAPTURE_CORE_H_
