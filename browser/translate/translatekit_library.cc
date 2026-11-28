// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/translate/translatekit_library.h"

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "build/build_config.h"
#include "components/on_device_translation/features.h"

#if BUILDFLAG(IS_MAC)
#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#endif

namespace compass {

namespace {

#if BUILDFLAG(IS_MAC)
constexpr base::FilePath::CharType kLibraryName[] =
    FILE_PATH_LITERAL("libcompass_translatekit.dylib");
#elif BUILDFLAG(IS_WIN)
constexpr base::FilePath::CharType kLibraryName[] =
    FILE_PATH_LITERAL("compass_translatekit.dll");
#else
constexpr base::FilePath::CharType kLibraryName[] =
    FILE_PATH_LITERAL("libcompass_translatekit.so");
#endif

}  // namespace

base::FilePath GetBundledTranslateKitLibrary() {
#if BUILDFLAG(IS_MAC)
  // Compass Framework.framework/Versions/<v>/Libraries, as Chromium's own
  // dylibs. Unbundled test binaries look in the out dir below.
  if (base::apple::AmIBundled()) {
    base::FilePath library = base::apple::FrameworkBundlePath()
                                 .Append(FILE_PATH_LITERAL("Libraries"))
                                 .Append(kLibraryName);
    if (base::PathExists(library)) {
      return library;
    }
  }
#endif
#if BUILDFLAG(IS_ANDROID)
  // In the APK (compass/android/BUILD.gn's loadable_modules), not on disk:
  // the native library folder's path is what upstream wants (absolute),
  // and the service opens the file by name (patch of
  // translate_kit_client.cc), which the linker finds in the APK.
  base::FilePath native_dir;
  if (base::PathService::Get(base::DIR_MODULE, &native_dir)) {
    return native_dir.Append(kLibraryName);
  }
  return base::FilePath();
#else
  // Windows: the versioned folder with chrome.dll. Linux: next to the
  // binary. Every platform's out dir.
  base::FilePath module_dir;
  if (base::PathService::Get(base::DIR_MODULE, &module_dir)) {
    base::FilePath library = module_dir.Append(kLibraryName);
    if (base::PathExists(library)) {
      return library;
    }
  }
  return base::FilePath();
#endif
}

void UseBundledTranslateKit(base::CommandLine& command_line) {
  if (command_line.HasSwitch(on_device_translation::kTranslateKitBinaryPath)) {
    return;
  }
  base::FilePath library = GetBundledTranslateKitLibrary();
  if (library.empty()) {
    return;
  }
  command_line.AppendSwitchPath(on_device_translation::kTranslateKitBinaryPath,
                                library);
}

}  // namespace compass
