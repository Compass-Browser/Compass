// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_SIDEBAR_SPACES_H_
#define COMPASS_BROWSER_UI_CLAUDE_SIDEBAR_SPACES_H_

#include "build/build_config.h"

// Where Claude's sidebar services (LinkFolderNamer, TodayTidier,
// ShortTitler) find a profile's spaces: the desktop's SpaceService, or on
// Android SpacesAndroid (docs/design/android.md, P12), which offer the same
// Get(), GetWeakPtr(), RunWhenLoaded() and space_list().
#if BUILDFLAG(IS_ANDROID)
#include "compass/browser/android/spaces_android.h"
#else
#include "compass/browser/window_sync/space_service.h"
#endif

namespace compass {

#if BUILDFLAG(IS_ANDROID)
using SidebarSpaces = SpacesAndroid;
#else
using SidebarSpaces = SpaceService;
#endif

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_CLAUDE_SIDEBAR_SPACES_H_
