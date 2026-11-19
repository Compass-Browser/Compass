// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_EASEL_EASEL_CONVERSIONS_H_
#define COMPASS_BROWSER_UI_WEBUI_EASEL_EASEL_CONVERSIONS_H_

#include <string>
#include <string_view>

#include "base/types/expected.h"
#include "compass/browser/ui/webui/easel/easel.mojom.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"

namespace compass::easel {

// Sync times (µs since the Windows epoch) as JS times, and back.
double SyncTimeToJsTime(int64_t sync_time);

mojom::EaselSummaryPtr ToMojo(const sync_pb::CompassEasel& easel,
                              int object_count);
mojom::EaselPtr ToMojoEasel(const sync_pb::CompassEasel& easel);
// With |base| filled in (the whole object, for FromMojo()).
mojom::EaselObjectPtr ToMojo(const sync_pb::CompassEaselObject& object);

// The object the page asks to save, on board |easel_uuid|: starts from
// |existing| (the store's object with that uuid) or else the page's |base|,
// so fields the page doesn't see are kept, then takes the page's box, z
// and content. The page can't change a capture or an unknown kind's
// content, only move, resize and stack them. Not yet checked against the
// limits (EaselStore::SaveObjects() does that).
base::expected<sync_pb::CompassEaselObject, std::string> FromMojo(
    const mojom::EaselObject& object,
    std::string_view easel_uuid,
    const sync_pb::CompassEaselObject* existing);

}  // namespace compass::easel

#endif  // COMPASS_BROWSER_UI_WEBUI_EASEL_EASEL_CONVERSIONS_H_
