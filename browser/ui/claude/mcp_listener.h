// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_MCP_LISTENER_H_
#define COMPASS_BROWSER_UI_CLAUDE_MCP_LISTENER_H_

#include <map>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"

namespace net {
class StreamSocket;
class UnixDomainServerSocket;
}  // namespace net

namespace compass::claude {

// The MCP bridge's socket, on the IO thread (net's sockets need its message
// pump): a Unix socket that only processes of this user may connect to,
// mode 0600. Each connection's bytes are split into lines, one JSON-RPC
// message each (MCP's stdio framing, which compass-mcp relays as is), and
// passed to the UI thread's McpBridge. See docs/design/claude.md, D6.
class McpListener {
 public:
  // Called on the IO thread; the bridge posts them on to the UI thread.
  struct Callbacks {
    Callbacks();
    Callbacks(Callbacks&&);
    Callbacks& operator=(Callbacks&&);
    ~Callbacks();

    base::RepeatingCallback<void(int)> connected;
    base::RepeatingCallback<void(int, std::string)> line;
    base::RepeatingCallback<void(int)> closed;
    // Whether the socket listens: false if it couldn't be made.
    base::OnceCallback<void(bool)> listening;
  };

  McpListener(base::FilePath path, Callbacks callbacks);
  McpListener(const McpListener&) = delete;
  McpListener& operator=(const McpListener&) = delete;
  // Closes every connection and removes the socket's file.
  ~McpListener();

  // Writes |line| and a newline to connection |id|.
  void Send(int id, std::string line);
  void Close(int id);

 private:
  struct Connection;

  void Accept();
  void OnAccept(int result);
  void Read(int id);
  void OnRead(int id, int result);
  void Write(int id);
  void OnWrite(int id, int result);
  void Drop(int id);

  const base::FilePath path_;
  // Where it listens when |path_| is too long for a socket address: a
  // short private directory's file, which |path_| links to.
  base::FilePath short_dir_;
  Callbacks callbacks_;
  std::unique_ptr<net::UnixDomainServerSocket> server_;
  std::unique_ptr<net::StreamSocket> accepted_;
  int next_id_ = 1;
  std::map<int, std::unique_ptr<Connection>> connections_;
  base::WeakPtrFactory<McpListener> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_CLAUDE_MCP_LISTENER_H_
