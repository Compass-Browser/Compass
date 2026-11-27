// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sidebar/update_notice.h"

#include <optional>
#include <string>
#include <utility>

#include "base/no_destructor.h"
#include "base/version.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "compass/browser/updates/update_hooks.h"
#include "components/prefs/pref_service.h"

namespace compass {
namespace {

// The update whose card was closed, for every window: its version, or empty
// when the build state has none (it couldn't be read).
struct Dismissed {
  std::optional<std::string> version;
  base::RepeatingClosureList changed;
};

Dismissed& GetDismissed() {
  static base::NoDestructor<Dismissed> dismissed;
  return *dismissed;
}

// A string, as an invalid base::Version can't be compared.
std::string PendingVersionOf(const BuildState& build_state) {
  const std::optional<base::Version>& version =
      build_state.installed_version();
  return version && version->IsValid() ? version->GetString() : std::string();
}

}  // namespace

UpdateNotice::UpdateNotice(base::RepeatingClosure on_changed)
    : on_changed_(std::move(on_changed)) {
  if (PrefService* local_state = g_browser_process->local_state()) {
    local_prefs_.Init(local_state);
    local_prefs_.Add(updates::kUpdateSidebarNoticePref, on_changed_);
  }
  if (BuildState* build_state = g_browser_process->GetBuildState()) {
    build_state_.Observe(build_state);
  }
  dismissed_subscription_ = GetDismissed().changed.Add(on_changed_);
}

UpdateNotice::~UpdateNotice() = default;

bool UpdateNotice::ShouldShow() const {
  if (!build_state_.IsObserving()) {
    return false;
  }
  const BuildState& build_state = *build_state_.GetSource();
  // Only a newer version: rollbacks are an administrator's.
  if (build_state.update_type() != BuildState::UpdateType::kNormalUpdate) {
    return false;
  }
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state ||
      !local_state->GetBoolean(updates::kUpdateSidebarNoticePref)) {
    return false;
  }
  const std::optional<std::string>& dismissed = GetDismissed().version;
  return !dismissed || *dismissed != PendingVersionOf(build_state);
}

void UpdateNotice::Dismiss() {
  if (!build_state_.IsObserving()) {
    return;
  }
  GetDismissed().version = PendingVersionOf(*build_state_.GetSource());
  GetDismissed().changed.Notify();
}

// static
void UpdateNotice::Relaunch() {
  // On macOS the restart goes through Sparkle, which installs the update
  // first (patches/chrome-browser-lifetime-application_lifetime_desktop.cc
  // .patch); on Windows the relaunch swaps in new_compass.exe.
  chrome::AttemptRestart();
}

void UpdateNotice::OnUpdate(const BuildState* build_state) {
  on_changed_.Run();
}

}  // namespace compass
