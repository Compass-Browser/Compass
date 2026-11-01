// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/window_sync/startup_space.h"

#include <set>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/window_features.h"

namespace compass {

namespace {

// SessionStartupPref::kPrefValueLast and kPrefValueLastAndURLs
// (chrome/browser/prefs/session_startup_pref.h), which this can't depend on.
constexpr int kRestoreLast = 1;
constexpr int kRestoreLastAndURLs = 6;

// How long to wait for the listed profiles. One that doesn't load keeps the
// directory from being complete; the window then stays as it is.
constexpr base::TimeDelta kTimeout = base::Seconds(10);

bool g_considered = false;

// Waits for the directory, then switches the window once and goes.
class StartupSpaceSwitcher : public SpaceDirectory::Observer {
 public:
  StartupSpaceSwitcher(SpaceDirectory* directory,
                       BrowserWindowInterface* browser)
      : browser_(browser->GetWeakPtr()) {
    directory->AddObserver(this);
    timeout_.Start(FROM_HERE, kTimeout,
                   base::BindOnce(&StartupSpaceSwitcher::Done,
                                  base::Unretained(this)));
  }

  // Deletes itself when done.
  void Start() { OnDirectoryChanged(); }

  // SpaceDirectory::Observer:
  void OnDirectoryChanged() override {
    SpaceDirectory* directory = SpaceDirectory::GetIfExists();
    if (!directory || !browser_) {
      Done();
      return;
    }
    if (!directory->IsLoaded()) {
      return;
    }
    // Stop listening first: switching changes the directory.
    directory->RemoveObserver(this);
    observing_ = false;
    ShowFirstSpace(directory);
    Done();
  }

 private:
  void ShowFirstSpace(SpaceDirectory* directory) {
    std::vector<SpaceDirectory::Entry> spaces = directory->GetSpaces();
    std::set<Profile*> profiles;
    for (const SpaceDirectory::Entry& entry : spaces) {
      profiles.insert(entry.profile.get());
    }
    // With one profile, the window keeps the space used last.
    if (profiles.size() < 2) {
      return;
    }
    WindowFeatures* features = WindowFeatures::From(browser_.get());
    if (!features || features->is_switching()) {
      return;
    }
    const SpaceDirectory::Entry& first = spaces.front();
    if (&features->space_model() == first.space.get()) {
      return;
    }
    SwitchWindowToSpace(browser_.get(), first.profile, *first.space);
  }

  void Done() {
    // The directory goes with the profile manager, at shutdown.
    if (SpaceDirectory* directory = SpaceDirectory::GetIfExists();
        directory && observing_) {
      directory->RemoveObserver(this);
    }
    delete this;
  }

  base::WeakPtr<BrowserWindowInterface> browser_;
  base::OneShotTimer timeout_;
  bool observing_ = true;
};

}  // namespace

void MaybeShowFirstSpaceAtStartup(BrowserWindowInterface* browser) {
  if (g_considered) {
    return;
  }
  WindowFeatures* features = WindowFeatures::From(browser);
  if (browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      !SpaceDirectory::IsListable(browser->GetProfile()) || !features ||
      features->is_parking_window() || IsCreatingSwapWindow()) {
    return;
  }
  // The first window the user sees decides, whatever happens next.
  g_considered = true;
  const int restore =
      browser->GetProfile()->GetPrefs()->GetInteger(prefs::kRestoreOnStartup);
  if (restore == kRestoreLast || restore == kRestoreLastAndURLs) {
    return;
  }
  // Once the window is built.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<BrowserWindowInterface> browser) {
                       SpaceDirectory* directory = SpaceDirectory::Get();
                       if (browser && directory) {
                         (new StartupSpaceSwitcher(directory, browser.get()))
                             ->Start();
                       }
                     },
                     browser->GetWeakPtr()));
}

void SetShowFirstSpaceAtStartupForTesting(bool enabled) {
  g_considered = !enabled;
}

}  // namespace compass
