// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UPDATES_UPDATE_HOOKS_H_
#define COMPASS_BROWSER_UPDATES_UPDATE_HOOKS_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/version.h"

// The seams through which upstream code reaches Compass's updater
// (docs/design/updates.md). Only //base, so upstream targets that the
// updater itself depends on (lifetime, the About page, the installed-version
// poller) can depend on this without a cycle.
namespace compass::updates {

// The update channels, from the most to the least tested. Each one also gets
// the releases of the channels before it. The pref holds their names.
inline constexpr char kChannelStable[] = "stable";
inline constexpr char kChannelBeta[] = "beta";
inline constexpr char kChannelCanary[] = "canary";

// Local State: the channel the user picked (one of the above).
inline constexpr char kUpdateChannelPref[] = "compass.update_channel";

// What the About page shows.
enum class CheckStatus {
  kChecking,
  kUpdating,        // Downloading or extracting; |progress| is 0-100 or -1.
  kNearlyUpdated,   // Downloaded, installs on relaunch.
  kUpToDate,
  kFailed,          // |message| says why.
};
using CheckCallback = base::RepeatingCallback<
    void(CheckStatus status, int progress, const std::u16string& message)>;

class Updater {
 public:
  virtual ~Updater() = default;

  // Checks now, reporting progress to |callback| until the check ends.
  virtual void CheckNow(CheckCallback callback) = 0;

  // When an update is downloaded: installs it and relaunches the browser,
  // which quits by itself. Returns false when there's nothing to install.
  virtual bool InstallAndRelaunch() = 0;
};

// The running updater, or null (dev builds, other platforms). UI thread.
Updater* GetUpdater();
void SetUpdater(Updater* updater);

// The version that a relaunch would run, once an update is downloaded.
// Upstream's installed-version poller reads it (any thread).
std::optional<base::Version> PendingVersion();
void SetPendingVersion(const base::Version& version);

// Compass's own version (compass_version in GN, e.g. "20261103.0215"), or
// empty in dev builds.
std::string CompassVersion();

}  // namespace compass::updates

#endif  // COMPASS_BROWSER_UPDATES_UPDATE_HOOKS_H_
