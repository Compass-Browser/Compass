// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Language packs come from Mozilla, through Compass's installer, not from
// Google's component updater (docs/design/translation.md, "Packs on the
// device"). Upstream's installer is unchanged except for the two calls that
// would register or uninstall a pack component; it still reads what's
// installed from its prefs, which Compass's installer sets. (The library
// itself is upstream's --translate-kit-binary-path, so Init() never
// registers Google's TranslateKit component either.)

// Declared first, under their own names, before the renaming below.
#include "chrome/browser/component_updater/translate_kit_language_pack_component_installer.h"
#include "compass/browser/translate/language_pack_installer_shim.h"

#define RegisterTranslateKitLanguagePackComponent \
  CompassRegisterTranslateKitLanguagePackComponent
#define UninstallTranslateKitLanguagePackComponent \
  CompassUninstallTranslateKitLanguagePackComponent
#include "src/chrome/browser/on_device_translation/installer_impl.cc"
#undef UninstallTranslateKitLanguagePackComponent
#undef RegisterTranslateKitLanguagePackComponent
