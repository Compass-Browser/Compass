// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// A standalone relay on plain Win32, with no //base for spans, as the POSIX
// one (compass_mcp.cc).
#pragma allow_unsafe_buffers
#endif

// compass-mcp.exe: the Windows relay for MCP clients that speak stdio
// (Claude Code, Claude Desktop, ...), as compass_mcp.cc is on macOS. The
// running Compass speaks MCP on a named pipe named after its user data
// directory (mcp_pipe_name_win.h) once "Allow MCP clients" is on. It starts
// Compass, minimized, if Compass isn't running. See docs/design/claude.md,
// D6.
//
//   claude mcp add compass -- "%LOCALAPPDATA%\Compass\Application\compass-mcp.exe"
//
// Options as on macOS: --user-data-dir=DIR, --no-launch, --chat=TOKEN.

#include <windows.h>

#include <shlobj.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "compass/tools/mcp/compass_mcp_common.h"
#include "compass/tools/mcp/mcp_pipe_name_win.h"

namespace {

using compass::mcp::kNotListening;
using compass::mcp::RequestId;

// As on macOS.
constexpr DWORD kWaitMs = 30000;
constexpr DWORD kPollMs = 250;
constexpr DWORD kChatWaitMs = 10000;
// After the client closes stdin, replies still on their way are passed on
// until none comes for this long. (A pipe can't be half closed, as the
// POSIX relay's socket is.)
constexpr DWORD kDrainMs = 5000;
// upstream's ProcessSingleton window: one per running browser, titled with
// its user data directory.
constexpr wchar_t kMessageWindowClass[] = L"Chrome_MessageWindow";

std::wstring ExeDir() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD size = ::GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (size == 0) {
      return std::wstring();
    }
    if (size < path.size()) {
      path.resize(size);
      break;
    }
    path.resize(path.size() * 2);
  }
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// As upstream's install_static for Compass's install mode:
// %LOCALAPPDATA%\Compass\User Data.
std::wstring DefaultUserDataDir() {
  PWSTR local_app_data = nullptr;
  std::wstring dir;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                       &local_app_data))) {
    dir = std::wstring(local_app_data) + L"\\Compass\\User Data";
  }
  ::CoTaskMemFree(local_app_data);
  return dir;
}

// This process's user, as a TOKEN_USER buffer.
std::vector<unsigned char> ProcessUser(HANDLE process) {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(process, TOKEN_QUERY, &token)) {
    return {};
  }
  DWORD size = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  if (size == 0 || !::GetTokenInformation(token, TokenUser, user.data(),
                                          size, &size)) {
    user.clear();
  }
  ::CloseHandle(token);
  return user;
}

// Whether the pipe's server runs as this user: another user can't have
// made Compass's pipe (the browser makes it first, and only this user may
// open it), but the relay checks rather than trusts that.
bool ServedBySameUser(HANDLE pipe) {
  ULONG pid = 0;
  if (!::GetNamedPipeServerProcessId(pipe, &pid)) {
    return false;
  }
  HANDLE server =
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!server) {
    return false;
  }
  const std::vector<unsigned char> theirs = ProcessUser(server);
  ::CloseHandle(server);
  const std::vector<unsigned char> ours = ProcessUser(::GetCurrentProcess());
  return !theirs.empty() && !ours.empty() &&
         ::EqualSid(reinterpret_cast<const TOKEN_USER*>(theirs.data())->User.Sid,
                    reinterpret_cast<const TOKEN_USER*>(ours.data())->User.Sid);
}

HANDLE Connect(const std::wstring& name) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    HANDLE pipe = ::CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      if (ServedBySameUser(pipe)) {
        return pipe;
      }
      ::CloseHandle(pipe);
      return INVALID_HANDLE_VALUE;
    }
    // Every instance busy: the browser makes another as each is taken.
    if (::GetLastError() != ERROR_PIPE_BUSY ||
        !::WaitNamedPipeW(name.c_str(), 2000)) {
      return INVALID_HANDLE_VALUE;
    }
  }
  return INVALID_HANDLE_VALUE;
}

bool IsCompassRunning(const std::wstring& user_data_dir) {
  return ::FindWindowExW(HWND_MESSAGE, nullptr, kMessageWindowClass,
                         user_data_dir.c_str()) != nullptr;
}

// Starts Compass minimized, without the focus, unless it's running (it
// would open another window, and its MCP setting is the user's to turn on).
void Launch(const std::wstring& user_data_dir, bool explicit_dir) {
  if (IsCompassRunning(user_data_dir)) {
    return;
  }
  const std::wstring dir = ExeDir();
  if (dir.empty()) {
    return;
  }
  const std::wstring chrome = dir + L"\\chrome.exe";
  std::wstring command = L"\"" + chrome + L"\"";
  if (explicit_dir) {
    command += L" \"--user-data-dir=" + user_data_dir + L"\"";
  }
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_SHOWMINNOACTIVE;
  PROCESS_INFORMATION process = {};
  // No inherited handles: the client waits for its pipes to close.
  if (::CreateProcessW(chrome.c_str(), command.data(), nullptr, nullptr,
                       FALSE, 0, nullptr, nullptr, &startup, &process)) {
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
  }
}

bool WriteAllSync(HANDLE handle, const char* data, DWORD size) {
  while (size > 0) {
    DWORD written = 0;
    if (!::WriteFile(handle, data, size, &written, nullptr)) {
      return false;
    }
    data += written;
    size -= written;
  }
  return true;
}

// The pipe is overlapped (one thread reads it while another writes).
bool WriteAllOverlapped(HANDLE pipe, HANDLE event, const char* data,
                        DWORD size) {
  while (size > 0) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = event;
    DWORD written = 0;
    if (!::WriteFile(pipe, data, size, nullptr, &overlapped) &&
        ::GetLastError() != ERROR_IO_PENDING) {
      return false;
    }
    if (!::GetOverlappedResult(pipe, &overlapped, &written, TRUE)) {
      return false;
    }
    data += written;
    size -= written;
  }
  return true;
}

// Without Compass, every request gets an error that says what to do.
int ServeErrors() {
  fprintf(stderr, "compass-mcp: %s\n", kNotListening);
  HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
  HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  std::string pending;
  char buffer[65536];
  for (;;) {
    DWORD got = 0;
    if (!::ReadFile(in, buffer, sizeof(buffer), &got, nullptr) || got == 0) {
      return 1;
    }
    pending.append(buffer, got);
    for (size_t end; (end = pending.find('\n')) != std::string::npos;) {
      const std::string id = RequestId(pending.substr(0, end));
      pending.erase(0, end + 1);
      if (id.empty() || id == "null") {
        continue;
      }
      const std::string reply =
          "{\"jsonrpc\":\"2.0\",\"id\":" + id +
          ",\"error\":{\"code\":-32000,\"message\":\"" + kNotListening +
          "\"}}\n";
      if (!WriteAllSync(out, reply.data(), static_cast<DWORD>(reply.size()))) {
        return 1;
      }
    }
  }
}

struct StdinRelay {
  HANDLE pipe;
  // Set when stdin ends.
  HANDLE done;
};

// stdin to the pipe, on its own thread.
DWORD WINAPI RelayStdin(void* param) {
  StdinRelay* relay = static_cast<StdinRelay*>(param);
  HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
  HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  char buffer[65536];
  for (;;) {
    DWORD got = 0;
    if (!::ReadFile(in, buffer, sizeof(buffer), &got, nullptr) || got == 0) {
      break;
    }
    if (!WriteAllOverlapped(relay->pipe, event, buffer, got)) {
      break;
    }
  }
  ::CloseHandle(event);
  ::SetEvent(relay->done);
  return 0;
}

// The pipe to stdout, until the browser closes it, or stdin has ended and
// nothing more comes.
int Relay(HANDLE pipe) {
  StdinRelay relay = {pipe, ::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  HANDLE thread =
      ::CreateThread(nullptr, 0, &RelayStdin, &relay, 0, nullptr);
  if (!thread) {
    return 1;
  }
  HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE read_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  char buffer[65536];
  int result = 0;
  for (;;) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = read_event;
    ::ResetEvent(read_event);
    if (!::ReadFile(pipe, buffer, sizeof(buffer), nullptr, &overlapped) &&
        ::GetLastError() != ERROR_IO_PENDING) {
      break;
    }
    HANDLE waits[] = {read_event, relay.done};
    DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0 + 1) {
      // stdin ended: what's still coming may take a little.
      wait = ::WaitForSingleObject(read_event, kDrainMs);
    }
    if (wait == WAIT_TIMEOUT) {
      ::CancelIoEx(pipe, &overlapped);
      DWORD ignored = 0;
      ::GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
      break;
    }
    DWORD got = 0;
    if (!::GetOverlappedResult(pipe, &overlapped, &got, TRUE) || got == 0) {
      break;
    }
    if (!WriteAllSync(out, buffer, got)) {
      result = 1;
      break;
    }
  }
  ::CloseHandle(read_event);
  // Ends the stdin thread's write, if any; its read of stdin ends with the
  // process.
  ::CancelIoEx(pipe, nullptr);
  ::CloseHandle(thread);
  return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring user_data_dir;
  bool launch = true;
  std::string chat;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg.starts_with(L"--chat=")) {
      const std::wstring token = arg.substr(wcslen(L"--chat="));
      // The browser's tokens are hex; nothing else goes in the hello line.
      if (token.empty() ||
          token.find_first_not_of(L"0123456789abcdefABCDEF") !=
              std::wstring::npos) {
        fprintf(stderr, "compass-mcp: bad --chat token\n");
        return 2;
      }
      chat.assign(token.begin(), token.end());
      launch = false;
    } else if (arg.starts_with(L"--user-data-dir=")) {
      user_data_dir = arg.substr(wcslen(L"--user-data-dir="));
    } else if (arg == L"--no-launch") {
      launch = false;
    } else if (arg == L"--help" || arg == L"-h") {
      printf(
          "compass-mcp: Compass's MCP server, for MCP clients over stdio.\n"
          "  --user-data-dir=DIR  Compass's user data directory\n"
          "  --no-launch          don't start Compass if it isn't running\n"
          "  --chat=TOKEN         serve the Claude panel's chat TOKEN\n");
      return 0;
    } else {
      fprintf(stderr, "compass-mcp: unknown option %ls\n", arg.c_str());
      return 2;
    }
  }
  const bool explicit_dir = !user_data_dir.empty();
  if (!explicit_dir) {
    user_data_dir = DefaultUserDataDir();
  }
  const std::wstring name = compass::mcp::McpPipeName(user_data_dir);

  HANDLE pipe = Connect(name);
  if (pipe == INVALID_HANDLE_VALUE && launch) {
    Launch(user_data_dir, explicit_dir);
    for (DWORD waited = 0; pipe == INVALID_HANDLE_VALUE && waited < kWaitMs;
         waited += kPollMs) {
      ::Sleep(kPollMs);
      pipe = Connect(name);
    }
  }
  for (DWORD waited = 0;
       pipe == INVALID_HANDLE_VALUE && !chat.empty() && waited < kChatWaitMs;
       waited += kPollMs) {
    ::Sleep(kPollMs);
    pipe = Connect(name);
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    return ServeErrors();
  }
  if (!chat.empty()) {
    // McpBridge reads the first line: this connection is the chat's.
    const std::string hello = "{\"compass_chat\":\"" + chat + "\"}\n";
    HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool sent = WriteAllOverlapped(pipe, event, hello.data(),
                                         static_cast<DWORD>(hello.size()));
    ::CloseHandle(event);
    if (!sent) {
      return 1;
    }
  }
  const int result = Relay(pipe);
  ::CloseHandle(pipe);
  // Not returning through the CRT's exit with the stdin thread blocked in
  // ReadFile is fine: ExitProcess ends it.
  return result;
}
