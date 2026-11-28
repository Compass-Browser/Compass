// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_TRANSLATE_LANGUAGE_PACK_INSTALLER_SHIM_H_
#define COMPASS_BROWSER_TRANSLATE_LANGUAGE_PACK_INSTALLER_SHIM_H_

#include "base/functional/callback.h"
#include "components/on_device_translation/public/language_pack.h"

class PrefService;

// Compass's stand-ins for the component updater's language pack functions
// (chrome/browser/component_updater/
// translate_kit_language_pack_component_installer.h), with their
// signatures. Upstream's installer calls these instead
// (chromium_src/chrome/browser/on_device_translation/installer_impl.cc), so
// packs come from Mozilla through compass::translate::PackInstaller, never
// from Google's component updater.

namespace component_updater {

class ComponentUpdateService;

// `registered_callback` (upstream's on-demand component update) is dropped.
void CompassRegisterTranslateKitLanguagePackComponent(
    ComponentUpdateService* cus,
    PrefService* pref_service,
    on_device_translation::LanguagePackKey language_pack_key,
    base::OnceClosure registered_callback,
    base::RepeatingClosure on_ready_callback);

void CompassUninstallTranslateKitLanguagePackComponent(
    ComponentUpdateService* cus,
    PrefService* pref_service,
    on_device_translation::LanguagePackKey language_pack_key);

}  // namespace component_updater

#endif  // COMPASS_BROWSER_TRANSLATE_LANGUAGE_PACK_INSTALLER_SHIM_H_
