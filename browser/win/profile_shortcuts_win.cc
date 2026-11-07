// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/win/profile_shortcuts_win.h"

#include <windows.h>

#include <shlobj.h>

#include <string>

#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/strcat_win.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/win/shortcut.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/installer/util/install_util.h"
#include "chrome/installer/util/shell_util.h"
#include "chrome/installer/util/util_constants.h"

namespace compass::win {
namespace {

// Whether |shortcut| starts |chrome_exe| in a given profile.
bool IsProfileShortcut(const base::FilePath& shortcut,
                       const base::FilePath& chrome_exe) {
  base::FilePath target;
  std::wstring arguments;
  if (!base::win::ResolveShortcut(shortcut, &target, &arguments) ||
      !base::FilePath::CompareEqualIgnoreCase(
          base::MakeLongFilePath(target).value(),
          base::MakeLongFilePath(chrome_exe).value())) {
    return false;
  }
  return base::CommandLine::FromString(L"unused_program " + arguments)
      .HasSwitch(switches::kProfileDirectory);
}

void RemoveOnDesktop(const base::FilePath& chrome_exe) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  base::FilePath desktop;
  if (!ShellUtil::GetShortcutPath(ShellUtil::SHORTCUT_LOCATION_DESKTOP,
                                  ShellUtil::CURRENT_USER, &desktop)) {
    return;
  }
  const std::wstring plain_name =
      base::StrCat({InstallUtil::GetShortcutName(), installer::kLnkExt});
  bool removed = false;
  base::FileEnumerator files(desktop, /*recursive=*/false,
                             base::FileEnumerator::FILES,
                             base::StrCat({L"*", installer::kLnkExt}));
  for (base::FilePath shortcut = files.Next(); !shortcut.empty();
       shortcut = files.Next()) {
    // Upstream's names: "<profile> - <product>.lnk", or "... (2).lnk".
    const std::wstring name = shortcut.BaseName().value();
    if (name == plain_name || name.find(L" - ") == std::wstring::npos ||
        !IsProfileShortcut(shortcut, chrome_exe)) {
      continue;
    }
    if (base::DeleteFile(shortcut)) {
      VLOG(1) << "Compass: removed the profile shortcut " << shortcut;
      SHChangeNotify(SHCNE_DELETE, SHCNF_PATH, shortcut.value().c_str(),
                     nullptr);
      removed = true;
    }
  }
  if (!removed || base::PathExists(desktop.Append(plain_name))) {
    return;
  }
  // The one the installer made, which the first profile shortcut was.
  ShellUtil::ShortcutProperties properties(ShellUtil::CURRENT_USER);
  ShellUtil::AddDefaultShortcutProperties(chrome_exe, &properties);
  properties.set_arguments(
      base::StrCat({L"--", base::ASCIIToWide(switches::kSourceShortcutLocation),
                    L"=",
                    base::ASCIIToWide(
                        switches::kSourceShortcutLocationDesktop)}));
  if (!ShellUtil::CreateOrUpdateShortcut(
          ShellUtil::SHORTCUT_LOCATION_DESKTOP, properties,
          ShellUtil::SHELL_SHORTCUT_CREATE_IF_NO_SYSTEM_LEVEL)) {
    LOG(WARNING) << "Compass: couldn't make the desktop shortcut again";
  }
}

}  // namespace

void RemoveProfileDesktopShortcuts() {
  base::FilePath chrome_exe;
  if (!base::PathService::Get(base::FILE_EXE, &chrome_exe)) {
    return;
  }
  // Shortcuts are COM objects.
  base::ThreadPool::CreateCOMSTATaskRunner(
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})
      ->PostTask(FROM_HERE, base::BindOnce(&RemoveOnDesktop, chrome_exe));
}

}  // namespace compass::win
