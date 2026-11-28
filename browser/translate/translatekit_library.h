// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_TRANSLATE_TRANSLATEKIT_LIBRARY_H_
#define COMPASS_BROWSER_TRANSLATE_TRANSLATEKIT_LIBRARY_H_

#include "base/files/file_path.h"

namespace base {
class CommandLine;
}

namespace compass {

// libcompass_translatekit as Compass ships it: in the framework's Libraries
// on a Mac, next to chrome.dll on Windows, next to the binary on Linux; or,
// in builds that don't bundle it (tests), in the out dir. Empty when it isn't
// there (docs/design/translation.md, "Where the library lives").
base::FilePath GetBundledTranslateKitLibrary();

// Points upstream's on-device translation service at the bundled library:
// sets --translate-kit-binary-path on `command_line`, the browser's, unless
// it's already there. Every upstream consumer (the installer, the service
// launcher, the sandbox) reads that switch first, so Google's TranslateKit
// component is never registered or downloaded.
void UseBundledTranslateKit(base::CommandLine& command_line);

}  // namespace compass

#endif  // COMPASS_BROWSER_TRANSLATE_TRANSLATEKIT_LIBRARY_H_
