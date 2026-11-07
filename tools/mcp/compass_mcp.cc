// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// A standalone relay in plain POSIX, with no //base for spans: it must run
// without Chromium's libraries (component builds put them in the
// framework).
#pragma allow_unsafe_buffers
#endif

// compass-mcp: an MCP server for MCP clients that speak stdio (Claude Code,
// Claude Desktop, ...). It's only a relay: the running Compass speaks MCP
// on <user data dir>/compass-mcp.sock (McpBridge) once "Allow MCP clients"
// is on in Settings, Claude. It starts Compass, in the background, if
// nothing answers. See docs/design/claude.md, D6.
//
//   claude mcp add compass -- /Applications/Compass.app/Contents/MacOS/compass-mcp
//
// Options: --user-data-dir=DIR (Compass's, if not the default one),
// --no-launch (don't start Compass), --chat=TOKEN (Compass started this
// relay's client for a chat in its Claude panel, in Claude Code mode: the
// relay says so first, and the browser runs the calls as that chat's).

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace {

constexpr char kSocketName[] = "compass-mcp.sock";
// How long Compass may take to start and listen.
constexpr int kWaitMs = 30000;
constexpr int kPollMs = 250;
// A chat's relay doesn't start Compass, which is running; it waits a little
// for the listener, which the chat starts with its Claude Code process.
constexpr int kChatWaitMs = 10000;

constexpr char kNotListening[] =
    "Compass isn't accepting MCP clients. Open Compass, then turn on "
    "Settings > Claude > Allow MCP clients.";

std::string Home() {
  const char* home = getenv("HOME");
  return home ? home : "";
}

#if defined(__APPLE__)
// The app bundle this binary is in: .../Compass.app.
std::string AppPath() {
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) != 0) {
    return std::string();
  }
  path.resize(strlen(path.c_str()));
  char resolved[PATH_MAX];
  if (realpath(path.c_str(), resolved)) {
    path = resolved;
  }
  // .../Compass.app/Contents/MacOS/compass-mcp
  for (int i = 0; i < 3; ++i) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
      return std::string();
    }
    path.resize(slash);
  }
  return path.size() > 4 && path.ends_with(".app") ? path : std::string();
}

// As chrome_paths_mac.mm: ~/Library/Application Support/ and the app's
// CrProductDirName, else "Compass" as compass::GetMacProductDirName()
// (common/user_data_dir_mac.mm), not upstream's "Chromium".
std::string DefaultUserDataDir() {
  std::string product = "Compass";
  if (CFBundleRef bundle = CFBundleGetMainBundle()) {
    CFTypeRef value =
        CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("CrProductDirName"));
    if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
      char buffer[256];
      if (CFStringGetCString(static_cast<CFStringRef>(value), buffer,
                             sizeof(buffer), kCFStringEncodingUTF8)) {
        product = buffer;
      }
    }
  }
  return Home() + "/Library/Application Support/" + product;
}
#else
std::string AppPath() {
  return std::string();
}

std::string DefaultUserDataDir() {
  const char* config = getenv("XDG_CONFIG_HOME");
  return (config && *config ? std::string(config) : Home() + "/.config") +
         "/compass";
}
#endif

int Connect(std::string path) {
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    // Too long for a socket's address: the browser listens in a short
    // directory and links to it from here.
    char target[sizeof(address.sun_path)];
    const ssize_t size = readlink(path.c_str(), target, sizeof(target) - 1);
    if (size <= 0) {
      return -1;
    }
    path.assign(target, size);
  }
  memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    close(fd);
    return -1;
  }
  return fd;
}

// Starts Compass in the background, without taking the user's focus.
void Launch(const std::string& user_data_dir, bool explicit_dir) {
  const std::string app = AppPath();
  if (app.empty()) {
    return;
  }
  std::vector<std::string> args = {"open", "-g", "-a", app};
  if (explicit_dir) {
    args.push_back("--args");
    args.push_back("--user-data-dir=" + user_data_dir);
  }
  std::vector<char*> argv;
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  pid_t pid;
  if (posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr, argv.data(),
                  environ) == 0) {
    int status;
    waitpid(pid, &status, 0);
  }
}

bool WriteAll(int fd, const char* data, size_t size) {
  while (size > 0) {
    const ssize_t written = write(fd, data, size);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    data += written;
    size -= written;
  }
  return true;
}

// The "id" of a JSON-RPC request, as its JSON text; empty for
// notifications. Enough for answering errors, not a JSON parser.
std::string RequestId(const std::string& line) {
  size_t at = line.find("\"id\"");
  if (at == std::string::npos) {
    return std::string();
  }
  at = line.find(':', at + 4);
  if (at == std::string::npos) {
    return std::string();
  }
  ++at;
  while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
    ++at;
  }
  if (at < line.size() && line[at] == '"') {
    size_t end = at + 1;
    while (end < line.size() && line[end] != '"') {
      end += line[end] == '\\' ? 2 : 1;
    }
    return line.substr(at, end + 1 - at);
  }
  size_t end = at;
  while (end < line.size() && line[end] != ',' && line[end] != '}' &&
         line[end] != ' ') {
    ++end;
  }
  return line.substr(at, end - at);
}

// Without Compass, every request gets an error that says what to do.
int ServeErrors() {
  fprintf(stderr, "compass-mcp: %s\n", kNotListening);
  std::string pending;
  char buffer[65536];
  for (;;) {
    const ssize_t got = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (got <= 0) {
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
      if (!WriteAll(STDOUT_FILENO, reply.data(), reply.size())) {
        return 1;
      }
    }
  }
}

// stdin to the socket, the socket to stdout, until the browser closes it.
int Relay(int socket_fd) {
  bool stdin_open = true;
  char buffer[65536];
  for (;;) {
    pollfd fds[2] = {{socket_fd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    const int count = stdin_open ? 2 : 1;
    if (poll(fds, count, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 1;
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      const ssize_t got = read(socket_fd, buffer, sizeof(buffer));
      if (got <= 0) {
        return 0;
      }
      if (!WriteAll(STDOUT_FILENO, buffer, got)) {
        return 1;
      }
    }
    if (stdin_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      const ssize_t got = read(STDIN_FILENO, buffer, sizeof(buffer));
      if (got <= 0) {
        // The client is done: the browser finishes what it's answering.
        stdin_open = false;
        shutdown(socket_fd, SHUT_WR);
        continue;
      }
      if (!WriteAll(socket_fd, buffer, got)) {
        return 1;
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  std::string user_data_dir;
  bool launch = true;
  std::string chat;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.starts_with("--chat=")) {
      chat = arg.substr(strlen("--chat="));
      // The browser's tokens are hex; nothing else goes in the hello line.
      if (chat.empty() ||
          chat.find_first_not_of("0123456789abcdefABCDEF") !=
              std::string::npos) {
        fprintf(stderr, "compass-mcp: bad --chat token\n");
        return 2;
      }
      launch = false;
    } else if (arg.starts_with("--user-data-dir=")) {
      user_data_dir = arg.substr(strlen("--user-data-dir="));
    } else if (arg == "--no-launch") {
      launch = false;
    } else if (arg == "--help" || arg == "-h") {
      printf(
          "compass-mcp: Compass's MCP server, for MCP clients over stdio.\n"
          "  --user-data-dir=DIR  Compass's user data directory\n"
          "  --no-launch          don't start Compass if it isn't running\n"
          "  --chat=TOKEN         serve the Claude panel's chat TOKEN\n");
      return 0;
    } else {
      fprintf(stderr, "compass-mcp: unknown option %s\n", arg.c_str());
      return 2;
    }
  }
  const bool explicit_dir = !user_data_dir.empty();
  if (!explicit_dir) {
    user_data_dir = DefaultUserDataDir();
  }
  const std::string path = user_data_dir + "/" + kSocketName;

  int fd = Connect(path);
  if (fd < 0 && launch) {
    Launch(user_data_dir, explicit_dir);
    for (int waited = 0; fd < 0 && waited < kWaitMs; waited += kPollMs) {
      usleep(kPollMs * 1000);
      fd = Connect(path);
    }
  }
  for (int waited = 0; fd < 0 && !chat.empty() && waited < kChatWaitMs;
       waited += kPollMs) {
    usleep(kPollMs * 1000);
    fd = Connect(path);
  }
  if (fd < 0) {
    return ServeErrors();
  }
  if (!chat.empty()) {
    // McpBridge reads the first line: this connection is the chat's.
    const std::string hello = "{\"compass_chat\":\"" + chat + "\"}\n";
    if (!WriteAll(fd, hello.data(), hello.size())) {
      return 1;
    }
  }
  return Relay(fd);
}
