// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/prefs/copy_link.h"

#include "compass/browser/prefs/compass_prefs.h"
#include "compass/common/clean_url.h"
#include "compass/common/compass_url.h"
#include "components/prefs/pref_service.h"
#include "url/gurl.h"

namespace compass {

GURL LinkToCopy(const PrefService* prefs, const GURL& url) {
  const GURL link = ToCompassURL(url);
  if (!prefs || !prefs->GetBoolean(prefs::kRemoveTrackingFromCopiedLinks)) {
    return link;
  }
  return RemoveTrackingParams(link);
}

}  // namespace compass
