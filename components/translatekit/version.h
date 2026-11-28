// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_TRANSLATEKIT_VERSION_H_
#define COMPASS_COMPONENTS_TRANSLATEKIT_VERSION_H_

namespace compass::translatekit {

// What GetTranslateKitVersion() reports. The service wants four numbers,
// at least 2025.1.10.0 (components/on_device_translation/features.cc), in
// at most 14 characters. Raise the third when the ABI or the package layout
// changes.
inline constexpr char kVersion[] = "2026.11.1.0";

}  // namespace compass::translatekit

#endif  // COMPASS_COMPONENTS_TRANSLATEKIT_VERSION_H_
