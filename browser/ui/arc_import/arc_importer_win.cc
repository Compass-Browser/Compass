// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The parts of the Arc importer that talk to Arc itself, on Windows (see
// arc_importer_mac.mm). Arc's key is read by GetArcDecryptor(), from its
// Local State; there's no Keychain, so ReadArcKeychainPassword() only keeps
// the testing hook.

#include <windows.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "base/no_destructor.h"
#include "base/process/process_iterator.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/scoped_handle.h"
#include "compass/browser/arc_import/arc_paths.h"
#include "compass/browser/ui/arc_import/arc_importer.h"

namespace compass::arc {

namespace {

// Arc's executable (its MSIX package's Application element).
constexpr wchar_t kArcExecutable[] = L"Arc.exe";

std::optional<std::optional<std::string>>& PasswordForTesting() {
  static base::NoDestructor<std::optional<std::optional<std::string>>> value;
  return *value;
}

std::optional<bool>& RunningForTesting() {
  static std::optional<bool> running;
  return running;
}

// Whether |pid|'s executable is in Arc's package folder (WindowsApps\
// TheBrowserCompany.Arc_<version>_<arch>__<publisher ID>), not another
// app's "Arc.exe".
bool IsArcPackageProcess(base::ProcessId pid) {
  base::win::ScopedHandle process(
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process.is_valid()) {
    return false;
  }
  wchar_t path[MAX_PATH * 2];
  DWORD size = std::size(path);
  if (!::QueryFullProcessImageNameW(process.get(), 0, path, &size)) {
    return false;
  }
  const std::wstring folder =
      L"\\" + base::UTF8ToWide(kArcWindowsPackageName) + L"_";
  return std::wstring_view(path, size).find(folder) != std::wstring::npos;
}

// Arc's processes.
std::set<DWORD> ArcProcessIds() {
  std::set<DWORD> ids;
  base::ProcessIterator processes(nullptr);
  while (const base::ProcessEntry* entry = processes.NextProcessEntry()) {
    if (base::EqualsCaseInsensitiveASCII(entry->exe_file(), kArcExecutable) &&
        IsArcPackageProcess(entry->pid())) {
      ids.insert(entry->pid());
    }
  }
  return ids;
}

struct CloseWindows {
  std::set<DWORD> pids;
  bool asked = false;
};

BOOL CALLBACK CloseArcWindow(HWND window, LPARAM param) {
  auto* close = reinterpret_cast<CloseWindows*>(param);
  DWORD pid = 0;
  ::GetWindowThreadProcessId(window, &pid);
  if (close->pids.contains(pid) && ::IsWindowVisible(window) &&
      !::GetWindow(window, GW_OWNER)) {
    close->asked |= ::PostMessageW(window, WM_CLOSE, 0, 0) != 0;
  }
  return TRUE;
}

}  // namespace

void SetArcKeychainPasswordForTesting(std::optional<std::string> password) {
  PasswordForTesting() = std::move(password);
}

std::optional<std::string> ReadArcKeychainPassword() {
  if (PasswordForTesting()) {
    return *PasswordForTesting();
  }
  return std::nullopt;
}

void SetArcRunningForTesting(std::optional<bool> running) {
  RunningForTesting() = running;
}

bool IsArcRunning() {
  if (RunningForTesting()) {
    return *RunningForTesting();
  }
  return !ArcProcessIds().empty();
}

bool QuitArc() {
  CloseWindows close;
  close.pids = ArcProcessIds();
  if (close.pids.empty()) {
    return false;
  }
  ::EnumWindows(&CloseArcWindow, reinterpret_cast<LPARAM>(&close));
  return close.asked;
}

}  // namespace compass::arc
