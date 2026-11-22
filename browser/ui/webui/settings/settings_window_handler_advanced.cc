// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings window's Advanced pane (phase 5,
// docs/design/settings-window.md): About Compass (its version and updates,
// through compass::updates' Updater, as the about page's
// version_updater_mac.mm patch uses it) and resetting upstream's settings
// (ProfileResetter, as chrome://settings/reset does).

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profile_resetter/brandcoded_default_settings.h"
#include "chrome/browser/profile_resetter/profile_resetter.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/ui/webui/settings/settings_window_handler.h"
#include "compass/browser/updates/update_hooks.h"
#include "components/version_info/version_info.h"

namespace compass {

namespace {

sidebar::mojom::UpdateState ToMojo(updates::CheckStatus status) {
  switch (status) {
    case updates::CheckStatus::kChecking:
      return sidebar::mojom::UpdateState::kChecking;
    case updates::CheckStatus::kUpdating:
      return sidebar::mojom::UpdateState::kUpdating;
    case updates::CheckStatus::kNearlyUpdated:
      return sidebar::mojom::UpdateState::kReady;
    case updates::CheckStatus::kUpToDate:
      return sidebar::mojom::UpdateState::kUpToDate;
    case updates::CheckStatus::kFailed:
      return sidebar::mojom::UpdateState::kFailed;
  }
  return sidebar::mojom::UpdateState::kFailed;
}

}  // namespace

void SettingsWindowHandler::GetAbout(GetAboutCallback callback) {
  std::optional<std::string> pending;
  if (std::optional<base::Version> version = updates::PendingVersion()) {
    pending = version->GetString();
  }
  std::move(callback).Run(sidebar::mojom::AboutInfo::New(
      updates::CompassVersion(), std::string(version_info::GetVersionNumber()),
      !!updates::GetUpdater(), pending));
}

void SettingsWindowHandler::CheckForUpdates() {
  updates::Updater* updater = updates::GetUpdater();
  if (!updater) {
    page_->UpdateStatusChanged(sidebar::mojom::UpdateState::kFailed, -1,
                               "This build doesn't update itself.");
    return;
  }
  updater->CheckNow(base::BindRepeating(
      [](base::WeakPtr<SettingsWindowHandler> handler,
         updates::CheckStatus status, int progress,
         const std::u16string& message) {
        if (handler) {
          handler->page_->UpdateStatusChanged(ToMojo(status), progress,
                                              base::UTF16ToUTF8(message));
        }
      },
      weak_factory_.GetWeakPtr()));
}

void SettingsWindowHandler::InstallUpdate() {
  if (updates::Updater* updater = updates::GetUpdater()) {
    updater->InstallAndRelaunch();
  }
}

void SettingsWindowHandler::ResetSettings(ResetSettingsCallback callback) {
  if (resetter_) {
    // One reset at a time.
    std::move(callback).Run();
    return;
  }
  resetter_ = std::make_unique<ProfileResetter>(profile_->GetOriginalProfile());
  resetter_->ResetSettings(
      ProfileResetter::ALL, std::make_unique<BrandcodedDefaultSettings>(),
      base::BindOnce(
          [](base::WeakPtr<SettingsWindowHandler> handler,
             ResetSettingsCallback callback) {
            if (handler) {
              handler->resetter_.reset();
            }
            std::move(callback).Run();
          },
          weak_factory_.GetWeakPtr(), std::move(callback)));
}

}  // namespace compass
