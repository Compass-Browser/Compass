// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_FAVICON_PNG_H_
#define COMPASS_BROWSER_SYNC_FAVICON_PNG_H_

#include <optional>
#include <string>

#include "base/containers/span.h"

namespace compass::sync {

// A favicon as COMPASS_SPACES carries it (docs/design/sync.md, "Favicons").
struct SyncedFaviconPng {
  // At most kFaviconMaxPixels a side and kFaviconMaxBytes.
  std::string png;
  // The source's larger side in px, before scaling.
  int source_size = 0;
};

// |source| (a PNG, as FaviconService stores it) as a synced icon: as is if it
// fits, else scaled down to 64, 48 or 32 px a side, the first that encodes
// within the byte limit. Nullopt if it doesn't decode or never fits.
std::optional<SyncedFaviconPng> MakeSyncedFaviconPng(
    base::span<const uint8_t> source);

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_FAVICON_PNG_H_
