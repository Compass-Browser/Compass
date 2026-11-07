// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compass's installer for a first install (BUILD.gn): a small window with
// the icon, "Installing Compass…" and a progress bar, while the
// mini_installer.exe it carries installs Compass for the current user. Then
// it opens Compass and goes away, or says what went wrong.
//
// mini_installer reports no progress, so the bar is a marquee, and it can't
// be stopped safely halfway, so there's no Cancel: the window can't be
// closed while it runs.

#include <windows.h>

#include <commctrl.h>

#include <string>

#include "compass/installer/win/compass_installer_resource.h"

namespace {

constexpr wchar_t kWindowClass[] = L"CompassInstaller";
constexpr wchar_t kTitle[] = L"Compass Installer";
// Posted to the window when the install is over; WPARAM is the exit code.
constexpr UINT kInstallDone = WM_APP + 1;
// The window's client area, at 96 DPI.
constexpr int kWidth = 420;
constexpr int kHeight = 220;
constexpr int kIconSize = 64;
constexpr int kBarWidth = 300;
constexpr int kBarHeight = 6;

// The installer's own exit codes are Chromium's (installer::InstallStatus
// and mini_installer's ExitCode); these are the few that come before it.
constexpr DWORD kExtractFailed = 0xC0A50001;
constexpr DWORD kLaunchFailed = 0xC0A50002;
constexpr DWORD kOpenFailed = 0xC0A50003;

struct Window {
  HWND hwnd = nullptr;
  HWND status = nullptr;
  HWND bar = nullptr;
  HICON icon = nullptr;
  HFONT font = nullptr;
  UINT dpi = USER_DEFAULT_SCREEN_DPI;
  bool installing = false;
};

Window* g_window = nullptr;

int Scale(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

// %TEMP%\CompassInstaller-<pid>.
std::wstring WorkDir() {
  wchar_t temp[MAX_PATH + 1] = {};
  const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
  if (length == 0 || length > MAX_PATH) {
    return std::wstring();
  }
  return std::wstring(temp) + L"CompassInstaller-" +
         std::to_wstring(GetCurrentProcessId());
}

// Writes the mini_installer.exe this program carries to |path|.
bool ExtractInstaller(const std::wstring& path) {
  HRSRC resource =
      FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_MINI_INSTALLER), RT_RCDATA);
  HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
  const char* bytes =
      loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
  const DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
  if (!bytes || size == 0) {
    return false;
  }
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD done = 0;
  bool ok = true;
  while (ok && done < size) {
    DWORD written = 0;
    ok = WriteFile(file, bytes + done, size - done, &written, nullptr) &&
         written > 0;
    done += written;
  }
  ok = CloseHandle(file) && ok;
  if (!ok) {
    DeleteFileW(path.c_str());
  }
  return ok;
}

// Runs |installer| and waits for it. Compass is opened afterwards by this
// program, so that a repair (the same version installed already, which
// opens nothing) also ends with Compass open.
DWORD RunInstaller(const std::wstring& installer) {
  std::wstring command_line =
      L"\"" + installer + L"\" --do-not-launch-chrome";
  STARTUPINFOW startup = {};
  startup.cb = static_cast<DWORD>(sizeof(startup));
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(installer.c_str(), command_line.data(), nullptr,
                      nullptr, FALSE, 0, nullptr, nullptr, &startup,
                      &process)) {
    return kLaunchFailed;
  }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = kLaunchFailed;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  return exit_code;
}

DWORD WINAPI InstallThread(void* param) {
  HWND hwnd = static_cast<HWND>(param);
  const std::wstring dir = WorkDir();
  DWORD result = kExtractFailed;
  if (!dir.empty()) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring installer = dir + L"\\mini_installer.exe";
    if (ExtractInstaller(installer)) {
      result = RunInstaller(installer);
      DeleteFileW(installer.c_str());
    }
    RemoveDirectoryW(dir.c_str());
  }
  PostMessageW(hwnd, kInstallDone, result, 0);
  return 0;
}

// Compass as installed for the current user (chromium_install_modes.h's
// kProductPathName, patched to "Compass").
std::wstring InstalledCompass() {
  wchar_t local_app_data[MAX_PATH + 1] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data,
                                               MAX_PATH + 1);
  if (length == 0 || length > MAX_PATH) {
    return std::wstring();
  }
  return std::wstring(local_app_data) + L"\\Compass\\Application\\chrome.exe";
}

bool OpenCompass() {
  const std::wstring compass = InstalledCompass();
  if (compass.empty() ||
      GetFileAttributesW(compass.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  std::wstring command_line = L"\"" + compass + L"\"";
  STARTUPINFOW startup = {};
  startup.cb = static_cast<DWORD>(sizeof(startup));
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(compass.c_str(), command_line.data(), nullptr, nullptr,
                      FALSE, 0, nullptr, nullptr, &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

void ShowFailure(HWND hwnd, DWORD code) {
  std::wstring text;
  if (code == kOpenFailed) {
    text = L"Compass is installed, but it couldn't be opened. Open it from "
           L"the Start menu.";
  } else if (code == kExtractFailed) {
    text = L"Compass couldn't be installed: this installer couldn't unpack "
           L"itself. Check that there's free space on the disk, or download "
           L"the installer again.";
  } else {
    wchar_t hex[16] = {};
    wsprintfW(hex, L"0x%lX", code);
    text = std::wstring(L"Compass couldn't be installed (error ") +
           std::to_wstring(code) + L", " + hex +
           L").\n\nDetails are in the installer log, "
           L"chromium_installer.log in your Temp folder.";
  }
  MessageBoxW(hwnd, text.c_str(), kTitle, MB_OK | MB_ICONERROR);
}

void UpdateFont(Window& window) {
  if (window.font) {
    DeleteObject(window.font);
  }
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = static_cast<UINT>(sizeof(metrics));
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, metrics.cbSize,
                             &metrics, 0, window.dpi);
  metrics.lfMessageFont.lfHeight = -MulDiv(
      12, static_cast<int>(window.dpi), 72);
  window.font = CreateFontIndirectW(&metrics.lfMessageFont);
  SendMessageW(window.status, WM_SETFONT,
               reinterpret_cast<WPARAM>(window.font), TRUE);
}

void Layout(Window& window) {
  RECT client = {};
  GetClientRect(window.hwnd, &client);
  const int width = client.right - client.left;
  const int bar_width = Scale(kBarWidth, window.dpi);
  const int text_top = Scale(28 + kIconSize + 18, window.dpi);
  const int text_height = Scale(24, window.dpi);
  SetWindowPos(window.status, nullptr, 0, text_top, width, text_height,
               SWP_NOZORDER | SWP_NOACTIVATE);
  SetWindowPos(window.bar, nullptr, (width - bar_width) / 2,
               text_top + text_height + Scale(14, window.dpi), bar_width,
               Scale(kBarHeight, window.dpi), SWP_NOZORDER | SWP_NOACTIVATE);
}

void Paint(Window& window) {
  PAINTSTRUCT paint = {};
  HDC dc = BeginPaint(window.hwnd, &paint);
  RECT client = {};
  GetClientRect(window.hwnd, &client);
  const int size = Scale(kIconSize, window.dpi);
  if (window.icon) {
    DrawIconEx(dc, (client.right - size) / 2, Scale(28, window.dpi),
               window.icon, size, size, 0, nullptr, DI_NORMAL);
  }
  EndPaint(window.hwnd, &paint);
}

void LoadIconForDpi(Window& window) {
  if (window.icon) {
    DestroyIcon(window.icon);
  }
  const int size = Scale(kIconSize, window.dpi);
  window.icon = static_cast<HICON>(
      LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_COMPASS),
                 IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  Window* window = g_window;
  switch (message) {
    case WM_CREATE: {
      window->hwnd = hwnd;
      window->dpi = GetDpiForWindow(hwnd);
      window->status = CreateWindowExW(
          0, L"STATIC", L"Installing Compass…",
          WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 0, 0, hwnd, nullptr,
          nullptr, nullptr);
      window->bar = CreateWindowExW(
          0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
          0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
      SendMessageW(window->bar, PBM_SETMARQUEE, TRUE, 30);
      UpdateFont(*window);
      LoadIconForDpi(*window);
      Layout(*window);
      window->installing = true;
      HANDLE thread =
          CreateThread(nullptr, 0, &InstallThread, hwnd, 0, nullptr);
      if (!thread) {
        PostMessageW(hwnd, kInstallDone, kLaunchFailed, 0);
      } else {
        CloseHandle(thread);
      }
      return 0;
    }
    case WM_CTLCOLORSTATIC:
      // The status text on the window's own background.
      SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_PAINT:
      Paint(*window);
      return 0;
    case WM_DPICHANGED: {
      window->dpi = HIWORD(wparam);
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      UpdateFont(*window);
      LoadIconForDpi(*window);
      Layout(*window);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }
    case kInstallDone: {
      window->installing = false;
      const DWORD code = static_cast<DWORD>(wparam);
      if (code == 0 && OpenCompass()) {
        PostQuitMessage(0);
      } else {
        ShowWindow(window->bar, SW_HIDE);
        ShowFailure(hwnd, code == 0 ? kOpenFailed : code);
        PostQuitMessage(code == 0 ? 0 : static_cast<int>(code));
      }
      return 0;
    }
    case WM_CLOSE:
      if (window->installing) {
        return 0;  // Not halfway through.
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX controls = {};
  controls.dwSize = static_cast<DWORD>(sizeof(controls));
  controls.dwICC = ICC_PROGRESS_CLASS;
  InitCommonControlsEx(&controls);

  // Another copy already running (a double click): bring it forward.
  if (HWND other = FindWindowW(kWindowClass, nullptr)) {
    SetForegroundWindow(other);
    return 0;
  }

  Window window;
  g_window = &window;

  WNDCLASSEXW window_class = {};
  window_class.cbSize = static_cast<UINT>(sizeof(window_class));
  window_class.lpfnWndProc = &WindowProc;
  window_class.hInstance = instance;
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_COMPASS));
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  window_class.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&window_class)) {
    return 1;
  }

  // A fixed-size window, centered on the primary monitor at its DPI.
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  POINT origin = {0, 0};
  HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info = {};
  info.cbSize = static_cast<DWORD>(sizeof(info));
  GetMonitorInfoW(monitor, &info);
  const UINT dpi = GetDpiForSystem();
  RECT frame = {0, 0, Scale(kWidth, dpi), Scale(kHeight, dpi)};
  AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;
  const RECT& work = info.rcWork;
  HWND hwnd = CreateWindowExW(
      0, kWindowClass, kTitle, style,
      work.left + (work.right - work.left - width) / 2,
      work.top + (work.bottom - work.top - height) / 2, width, height,
      nullptr, nullptr, instance, nullptr);
  if (!hwnd) {
    return 1;
  }
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  MSG message = {};
  int exit_code = 0;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (message.message == WM_QUIT) {
    exit_code = static_cast<int>(message.wParam);
  }
  if (window.icon) {
    DestroyIcon(window.icon);
  }
  if (window.font) {
    DeleteObject(window.font);
  }
  return exit_code;
}
