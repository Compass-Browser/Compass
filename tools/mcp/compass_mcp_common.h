// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_TOOLS_MCP_COMPASS_MCP_COMMON_H_
#define COMPASS_TOOLS_MCP_COMPASS_MCP_COMMON_H_

#include <string>

// What compass_mcp.cc (POSIX) and compass_mcp_win.cc share. Plain C++, no
// Chromium library.

namespace compass::mcp {

inline constexpr char kNotListening[] =
    "Compass isn't accepting MCP clients. Open Compass, then turn on "
    "Settings > Claude > Allow MCP clients.";

// The "id" of a JSON-RPC request, as its JSON text; empty for
// notifications. Enough for answering errors, not a JSON parser.
inline std::string RequestId(const std::string& line) {
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

}  // namespace compass::mcp

#endif  // COMPASS_TOOLS_MCP_COMPASS_MCP_COMMON_H_
