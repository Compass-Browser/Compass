// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UPDATES_SPARKLE_UPDATER_MAC_H_
#define COMPASS_BROWSER_UPDATES_SPARKLE_UPDATER_MAC_H_

namespace compass::updates {

// Starts Sparkle, if this build has it: the release tool puts
// Sparkle.framework in Contents/Frameworks and the feed URL and public key in
// Info.plist (docs/design/updates.md). Dev builds have neither, so nothing
// starts. Call once, on the UI thread, after Local State is loaded.
void StartSparkleUpdater();

}  // namespace compass::updates

#endif  // COMPASS_BROWSER_UPDATES_SPARKLE_UPDATER_MAC_H_
