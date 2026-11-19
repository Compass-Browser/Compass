// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compass keeps Manifest V2 extensions working. Upstream still loads and runs
// MV2 (extension.cc accepts manifest versions 2 to 3, and MV2-only APIs such
// as webRequestBlocking are still there), but ManifestV2Handler blocks
// installing and enabling them, and disables any it finds at startup.
//
// Every one of those decisions goes through IsExtensionAffected(): the
// install and enable checks, the startup pass that disables extensions, and
// the extensions page's "no longer supported" banners. With nothing affected:
// - MV2 extensions install (unpacked, or a .crx dropped on chrome://extensions)
//   and enable;
// - extensions upstream disabled with DISABLE_UNSUPPORTED_MANIFEST_VERSION are
//   re-enabled on the next startup (ManifestV2Handler::CheckDisabledExtensions);
// - the extensions page shows no deprecation notices.

#include "extensions/browser/manifest_v2_util.h"

#include "extensions/buildflags/buildflags.h"

static_assert(BUILDFLAG(ENABLE_EXTENSIONS_CORE));

namespace extensions::manifest_v2_util {

bool IsExtensionAffected(const Extension& extension) {
  return false;
}

bool IsExtensionAffected(int manifest_version,
                         Manifest::Type manifest_type,
                         mojom::ManifestLocation manifest_location) {
  return false;
}

}  // namespace extensions::manifest_v2_util
