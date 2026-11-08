// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_TOOLS_MCP_MCP_PIPE_NAME_WIN_H_
#define COMPASS_TOOLS_MCP_MCP_PIPE_NAME_WIN_H_

#include <stdint.h>

#include <string>

namespace compass::mcp {

// The named pipe the browser's MCP bridge listens on for the user data
// directory |user_data_dir|, which compass-mcp finds the same way: Windows
// has no Unix socket file in the directory, so the pipe's name comes from
// the directory's path (FNV-1a, ASCII letters lowercased, '/' as '\', no
// trailing separator). Header-only: compass-mcp uses no Chromium library.
// See docs/design/claude.md, D6.
inline std::wstring McpPipeName(std::wstring user_data_dir) {
  while (!user_data_dir.empty() &&
         (user_data_dir.back() == L'\\' || user_data_dir.back() == L'/')) {
    user_data_dir.pop_back();
  }
  uint64_t hash = 14695981039346656037ull;
  for (wchar_t c : user_data_dir) {
    if (c == L'/') {
      c = L'\\';
    } else if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    hash ^= static_cast<uint16_t>(c);
    hash *= 1099511628211ull;
  }
  std::wstring name = L"\\\\.\\pipe\\compass-mcp-";
  constexpr wchar_t kHex[] = L"0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4) {
    name += kHex[(hash >> shift) & 0xf];
  }
  return name;
}

}  // namespace compass::mcp

#endif  // COMPASS_TOOLS_MCP_MCP_PIPE_NAME_WIN_H_
