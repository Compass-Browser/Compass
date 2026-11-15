// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/installer/win/legacy_chrome_exe.h"

#include <windows.h>

#include <initializer_list>
#include <memory>

#include "base/base_paths.h"
#include "base/base_paths_win.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/win/registry.h"
#include "base/win/shortcut.h"
#include "chrome/install_static/install_util.h"
#include "chrome/installer/util/callback_work_item.h"
#include "chrome/installer/util/conditional_work_item.h"
#include "chrome/installer/util/copy_tree_work_item.h"
#include "chrome/installer/util/registry_util.h"
#include "chrome/installer/util/shell_util.h"
#include "chrome/installer/util/util_constants.h"
#include "chrome/installer/util/work_item.h"
#include "chrome/installer/util/work_item_list.h"

namespace compass::installer {
namespace {

// Software\Clients\StartMenuInternet\<name><suffix>\shell\open\command, the
// key upstream's GetInstallationSpecificSuffix() checks.
std::wstring OpenCommandKey(const std::wstring& suffix) {
  return base::StrCat({ShellUtil::kRegStartMenuInternet, L"\\",
                       install_static::GetBaseAppName(), suffix,
                       ShellUtil::kRegShellOpen});
}

bool IsLegacyExe(const ::installer::ProgramCompare& legacy,
                 const base::FilePath& path) {
  return !path.empty() && legacy.EvaluatePath(path);
}

void DeleteLegacyAppPath(const base::FilePath& legacy_exe) {
  const std::wstring key =
      base::StrCat({ShellUtil::kAppPathsRegistryKey, L"\\", kLegacyChromeExe});
  base::win::RegKey app_path(HKEY_CURRENT_USER, key.c_str(), KEY_QUERY_VALUE);
  std::wstring value;
  if (app_path.ReadValue(L"", &value) != ERROR_SUCCESS) {
    return;
  }
  app_path.Close();
  // Chrome registers the same key name; only Compass's own goes.
  if (!::installer::ProgramCompare(legacy_exe)
           .EvaluatePath(base::FilePath(value))) {
    return;
  }
  base::win::RegKey(HKEY_CURRENT_USER, ShellUtil::kAppPathsRegistryKey,
                    KEY_SET_VALUE | DELETE)
      .DeleteKey(kLegacyChromeExe);
}

bool Migrate(const base::FilePath& target_path,
             const CallbackWorkItem& /*item*/) {
  if (!base::PathExists(target_path.Append(kLegacyChromeExe))) {
    return true;
  }
  MigrateLegacyRegistrations(target_path);
  RetargetLegacyShortcuts(target_path);
  return true;
}

}  // namespace

void AddLegacyChromeExeWorkItems(const base::FilePath& target_path,
                                 const base::FilePath& version_dir,
                                 const base::FilePath& temp_path,
                                 WorkItemList* list) {
  for (const wchar_t* name :
       {kLegacyChromeNewExe, kLegacyChromeOldExe,
        kLegacyVisualElementsManifest}) {
    list->AddDeleteTreeWorkItem(target_path.Append(name), temp_path)
        ->set_best_effort(true);
  }
  const base::FilePath legacy_exe = target_path.Append(kLegacyChromeExe);
  list->AddWorkItem(WorkItem::CreateConditionalWorkItem(
      std::make_unique<ConditionFileExists>(legacy_exe),
      base::WrapUnique<WorkItem>(WorkItem::CreateCopyTreeWorkItem(
          version_dir.Append(kLegacyChromeStub), legacy_exe, temp_path)),
      nullptr));
}

void AddLegacyChromeExeMigrationWorkItem(const base::FilePath& target_path,
                                         bool system_install,
                                         WorkItemList* list) {
  // Compass installs per user only.
  if (system_install) {
    return;
  }
  WorkItem* item = list->AddCallbackWorkItem(
      base::BindOnce(&Migrate, target_path), base::DoNothing());
  item->set_best_effort(true);
  item->set_rollback_enabled(false);
}

bool FindLegacyRegistrationSuffix(const base::FilePath& legacy_exe,
                                  std::wstring* suffix) {
  // The order of ShellUtil::GetCurrentInstallationSuffix(): new-style,
  // old-style, none.
  std::wstring candidates[3];
  ShellUtil::GetUserSpecificRegistrySuffix(&candidates[0]);
  ShellUtil::GetOldUserSpecificRegistrySuffix(&candidates[1]);
  const ::installer::ProgramCompare legacy(legacy_exe);
  for (const std::wstring& candidate : candidates) {
    base::win::RegKey key(HKEY_CURRENT_USER,
                          OpenCommandKey(candidate).c_str(), KEY_QUERY_VALUE);
    std::wstring command;
    if (key.ReadValue(L"", &command) == ERROR_SUCCESS &&
        legacy.Evaluate(command)) {
      *suffix = candidate;
      return true;
    }
  }
  return false;
}

bool MigrateLegacyRegistrations(const base::FilePath& target_path) {
  const base::FilePath legacy_exe = target_path.Append(kLegacyChromeExe);
  const base::FilePath compass_exe =
      target_path.Append(::installer::kChromeExe);
  DeleteLegacyAppPath(legacy_exe);
  std::wstring suffix;
  if (!FindLegacyRegistrationSuffix(legacy_exe, &suffix)) {
    return false;
  }
  // The key that tells which suffix is Compass's first, so that
  // RegisterChromeBrowser() (and the installer's registration after it)
  // keep that suffix for compass.exe, also when it's none.
  base::win::RegKey key(HKEY_CURRENT_USER, OpenCommandKey(suffix).c_str(),
                        KEY_SET_VALUE);
  if (key.WriteValue(L"", base::CommandLine(compass_exe)
                              .GetCommandLineString()
                              .c_str()) != ERROR_SUCCESS) {
    LOG(ERROR) << "Can't move the registration of " << legacy_exe;
    return false;
  }
  ShellUtil::RegisterChromeBrowser(compass_exe, suffix,
                                   /*elevate_if_not_admin=*/false);
  return true;
}

int RetargetShortcutsIn(const base::FilePath& dir,
                        bool recursive,
                        const base::FilePath& legacy_exe,
                        const base::FilePath& new_exe) {
  const ::installer::ProgramCompare legacy(legacy_exe);
  int changed = 0;
  // No pattern: on Windows it would also filter the folders to recurse into.
  base::FileEnumerator shortcuts(dir, recursive, base::FileEnumerator::FILES);
  for (base::FilePath path = shortcuts.Next(); !path.empty();
       path = shortcuts.Next()) {
    if (!path.MatchesExtension(FILE_PATH_LITERAL(".lnk"))) {
      continue;
    }
    base::win::ShortcutProperties old_properties;
    if (!base::win::ResolveShortcutProperties(
            path,
            base::win::ShortcutProperties::PROPERTIES_TARGET |
                base::win::ShortcutProperties::PROPERTIES_ICON,
            &old_properties) ||
        !IsLegacyExe(legacy, old_properties.target)) {
      continue;
    }
    base::win::ShortcutProperties properties;
    properties.set_target(new_exe);
    if (IsLegacyExe(legacy, old_properties.icon)) {
      properties.set_icon(new_exe, old_properties.icon_index);
    }
    if (base::win::CreateOrUpdateShortcutLink(
            path, properties, base::win::ShortcutOperation::kUpdateExisting)) {
      ++changed;
    } else {
      LOG(ERROR) << "Can't retarget " << path;
    }
  }
  return changed;
}

int RetargetLegacyShortcuts(const base::FilePath& target_path) {
  const base::FilePath legacy_exe = target_path.Append(kLegacyChromeExe);
  const base::FilePath compass_exe =
      target_path.Append(::installer::kChromeExe);
  int changed = 0;
  for (int key : std::initializer_list<int>{
           base::DIR_USER_DESKTOP, base::DIR_START_MENU,
           base::DIR_USER_QUICK_LAUNCH, base::DIR_USER_STARTUP}) {
    base::FilePath dir;
    if (base::PathService::Get(key, &dir)) {
      changed += RetargetShortcutsIn(dir, /*recursive=*/false, legacy_exe,
                                     compass_exe);
    }
  }
  // User Pinned\TaskBar, and the jump lists' User Pinned\ImplicitAppShortcuts.
  base::FilePath pins;
  if (base::PathService::Get(base::DIR_TASKBAR_PINS, &pins)) {
    changed += RetargetShortcutsIn(pins.DirName(), /*recursive=*/true,
                                   legacy_exe, compass_exe);
  }
  return changed;
}

}  // namespace compass::installer
