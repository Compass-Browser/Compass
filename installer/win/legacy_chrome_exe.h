// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_INSTALLER_WIN_LEGACY_CHROME_EXE_H_
#define COMPASS_INSTALLER_WIN_LEGACY_CHROME_EXE_H_

#include <string>

#include "base/files/file_path.h"

class WorkItemList;

// Compass's browser was chrome.exe until it became compass.exe
// (docs/design/updates.md, "Windows: compass.exe"). Setup keeps a small
// chrome.exe that starts compass.exe on installs that had one, since taskbar
// pins, shortcuts and registry entries the installer can't find, and a
// running older browser's "Relaunch to update", start chrome.exe. It also
// points what it can find at compass.exe.
namespace compass::installer {

// Names from before the rename, in the install folder.
inline constexpr wchar_t kLegacyChromeExe[] = L"chrome.exe";
inline constexpr wchar_t kLegacyChromeNewExe[] = L"new_chrome.exe";
inline constexpr wchar_t kLegacyChromeOldExe[] = L"old_chrome.exe";
inline constexpr wchar_t kLegacyVisualElementsManifest[] =
    L"chrome.VisualElementsManifest.xml";

// The chrome.exe that starts compass.exe (legacy_chrome_main.cc), shipped in
// the version folder.
inline constexpr wchar_t kLegacyChromeStub[] = L"legacy_chrome.exe";

// Adds to |list|, once compass.exe and the version folder |version_dir| are
// in |target_path|: removing new_chrome.exe, old_chrome.exe and
// chrome.VisualElementsManifest.xml, and, when |target_path| has a
// chrome.exe, putting the stub in its place. A running chrome.exe is moved
// aside (Windows allows that for a running program) into |temp_path|.
void AddLegacyChromeExeWorkItems(const base::FilePath& target_path,
                                 const base::FilePath& version_dir,
                                 const base::FilePath& temp_path,
                                 WorkItemList* list);

// Adds to |list| a last, best-effort item that, on a per-user install with a
// chrome.exe, runs MigrateLegacyRegistrations() and
// RetargetLegacyShortcuts().
void AddLegacyChromeExeMigrationWorkItem(const base::FilePath& target_path,
                                         bool system_install,
                                         WorkItemList* list);

// Finds the suffix (see ShellUtil::GetCurrentInstallationSuffix()) under
// which Compass registered |legacy_exe| with Default Programs: the one whose
// Software\Clients\StartMenuInternet\<name><suffix> opens it.
bool FindLegacyRegistrationSuffix(const base::FilePath& legacy_exe,
                                  std::wstring* suffix);

// Points the registrations of |target_path|\chrome.exe at compass.exe under
// the same suffix, so their names stay and with them the user's default
// browser choice (which names the ProgId). Deletes App Paths\chrome.exe when
// it's Compass's. Returns whether it found a registration to move.
bool MigrateLegacyRegistrations(const base::FilePath& target_path);

// Retargets the shortcuts in |dir| (and its subfolders if |recursive|) that
// start |legacy_exe| to |new_exe|, and their icon if it's |legacy_exe|'s.
// Returns how many it changed.
int RetargetShortcutsIn(const base::FilePath& dir,
                        bool recursive,
                        const base::FilePath& legacy_exe,
                        const base::FilePath& new_exe);

// RetargetShortcutsIn() for the current user's desktop, Start menu, Quick
// Launch, startup folder and taskbar pins.
int RetargetLegacyShortcuts(const base::FilePath& target_path);

}  // namespace compass::installer

#endif  // COMPASS_INSTALLER_WIN_LEGACY_CHROME_EXE_H_
