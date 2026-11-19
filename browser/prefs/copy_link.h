// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_PREFS_COPY_LINK_H_
#define COMPASS_BROWSER_PREFS_COPY_LINK_H_

class GURL;
class PrefService;

namespace compass {

// The link to put on the clipboard for |url|, a page's: chrome:// as
// compass://, and without tracking parameters unless the profile turned
// that off (prefs::kRemoveTrackingFromCopiedLinks; docs/design/shortcuts.md
// "Clean links"). |prefs| is the profile's; null keeps the parameters.
GURL LinkToCopy(const PrefService* prefs, const GURL& url);

}  // namespace compass

#endif  // COMPASS_BROWSER_PREFS_COPY_LINK_H_
