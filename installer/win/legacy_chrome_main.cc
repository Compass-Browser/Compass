// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// Plain Win32 with no //base for spans, as compass-mcp.exe.
#pragma allow_unsafe_buffers
#endif

// chrome.exe on installs from before compass.exe (legacy_chrome_exe.h): starts
// compass.exe from its own folder with the same arguments, and exits. Plain
// Win32, so it stays small and starts fast.

#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

#include <string>
#include <vector>

namespace {

// What follows the program name in |command_line|, by CommandLineToArgvW's
// rules for the first argument: up to the closing quote when it starts with
// one, otherwise up to the first space or tab.
const wchar_t* Arguments(const wchar_t* command_line) {
  const wchar_t* p = command_line;
  if (*p == L'"') {
    ++p;
    while (*p && *p != L'"') {
      ++p;
    }
    if (*p == L'"') {
      ++p;
    }
  } else {
    while (*p && *p != L' ' && *p != L'\t') {
      ++p;
    }
  }
  while (*p == L' ' || *p == L'\t') {
    ++p;
  }
  return p;
}

// The handle an older browser's relaunch passes in --wait-for-parent-handle
// (inheritable, its own process) for the new browser to wait on; compass.exe
// inherits it with the same value. Null when there's none.
HANDLE ParentHandle(const wchar_t* arguments) {
  static constexpr wchar_t kSwitch[] = L"--wait-for-parent-handle=";
  const wchar_t* found = wcsstr(arguments, kSwitch);
  if (!found) {
    return nullptr;
  }
  const unsigned long value = wcstoul(found + wcslen(kSwitch), nullptr, 10);
  HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
  DWORD flags = 0;
  if (!handle || !::GetHandleInformation(handle, &flags) ||
      !(flags & HANDLE_FLAG_INHERIT)) {
    return nullptr;
  }
  return handle;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int) {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = ::GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
      return 1;
    }
    if (length < path.size()) {
      path.resize(length);
      break;
    }
    path.resize(path.size() * 2);
  }
  const std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
  const std::wstring compass = dir + L"\\compass.exe";

  const wchar_t* arguments = Arguments(::GetCommandLineW());
  std::wstring command = L"\"" + compass + L"\"";
  if (*arguments) {
    command += L' ';
    command += arguments;
  }

  // As started: the window state, and the shortcut that started it, which
  // ties compass.exe's windows to a taskbar pin.
  STARTUPINFOEXW startup = {};
  ::GetStartupInfoW(&startup.StartupInfo);
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.lpReserved = nullptr;
  startup.StartupInfo.cbReserved2 = 0;
  startup.StartupInfo.lpReserved2 = nullptr;
  startup.StartupInfo.dwFlags &= ~STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = nullptr;
  startup.StartupInfo.hStdOutput = nullptr;
  startup.StartupInfo.hStdError = nullptr;

  // Only the parent's handle is inherited, nothing else of the caller's.
  DWORD flags = 0;
  BOOL inherit = FALSE;
  std::vector<char> attributes;
  HANDLE parent = ParentHandle(arguments);
  if (parent) {
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    attributes.resize(size);
    auto* list =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (::InitializeProcThreadAttributeList(list, 1, 0, &size)) {
      if (::UpdateProcThreadAttribute(list, 0,
                                      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      &parent, sizeof(parent), nullptr,
                                      nullptr)) {
        startup.lpAttributeList = list;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
        inherit = TRUE;
      } else {
        ::DeleteProcThreadAttributeList(list);
      }
    }
  }

  PROCESS_INFORMATION process = {};
  const BOOL started = ::CreateProcessW(
      compass.c_str(), command.data(), nullptr, nullptr, inherit, flags,
      nullptr, dir.c_str(), &startup.StartupInfo, &process);
  if (startup.lpAttributeList) {
    ::DeleteProcThreadAttributeList(startup.lpAttributeList);
  }
  if (!started) {
    return 1;
  }
  // Let it bring its window to the front, as this process could.
  ::AllowSetForegroundWindow(process.dwProcessId);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  return 0;
}
