// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/translate/language_pack_installer_shim.h"

#include <utility>

#include "compass/browser/translate/translation_pack_installer.h"

namespace component_updater {

void CompassRegisterTranslateKitLanguagePackComponent(
    ComponentUpdateService* cus,
    PrefService* pref_service,
    on_device_translation::LanguagePackKey language_pack_key,
    base::OnceClosure registered_callback,
    base::RepeatingClosure on_ready_callback) {
  compass::translate::PackInstaller::Get().Install(
      language_pack_key, pref_service, std::move(on_ready_callback));
}

void CompassUninstallTranslateKitLanguagePackComponent(
    ComponentUpdateService* cus,
    PrefService* pref_service,
    on_device_translation::LanguagePackKey language_pack_key) {
  compass::translate::PackInstaller::Get().Uninstall(language_pack_key,
                                                     pref_service);
}

}  // namespace component_updater
