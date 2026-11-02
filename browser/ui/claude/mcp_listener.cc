// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/mcp_listener.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <deque>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/socket/stream_socket.h"
#include "net/socket/unix_domain_server_socket_posix.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace compass::claude {

namespace {

constexpr int kReadSize = 64 * 1024;
// A message longer than this closes the connection: no tool input is near
// it, and a client that never sends a newline can't grow the buffer forever.
constexpr size_t kMaxLine = 8 * 1024 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_mcp", R"(
        semantics {
          sender: "Compass MCP bridge"
          description:
            "Answers MCP clients on this computer (Claude Code and others, "
            "through compass-mcp) over a Unix socket only the user can open."
          trigger: "An MCP client the user configured calls Compass's tools."
          data: "Tool results: page text, screenshots, console messages."
          destination: LOCAL
        }
        policy {
          cookies_allowed: NO
          setting: "Settings, Claude, Allow MCP clients (off by default)."
          policy_exception_justification: "Not implemented."
        })");

// Only processes of this user: the socket's mode says so too, but its
// directory might not.
bool IsSameUser(const net::UnixDomainServerSocket::Credentials& credentials) {
  return credentials.user_id == geteuid();
}

}  // namespace

McpListener::Callbacks::Callbacks() = default;
McpListener::Callbacks::Callbacks(Callbacks&&) = default;
McpListener::Callbacks& McpListener::Callbacks::operator=(Callbacks&&) =
    default;
McpListener::Callbacks::~Callbacks() = default;

struct McpListener::Connection {
  std::unique_ptr<net::StreamSocket> socket;
  scoped_refptr<net::IOBuffer> read_buffer =
      base::MakeRefCounted<net::IOBufferWithSize>(kReadSize);
  std::string pending;
  std::deque<std::string> writes;
  scoped_refptr<net::DrainableIOBuffer> writing;
};

McpListener::McpListener(base::FilePath path, Callbacks callbacks)
    : path_(std::move(path)), callbacks_(std::move(callbacks)) {
  // A file left by a browser that crashed.
  ::unlink(path_.value().c_str());
  std::string listen_path = path_.value();
  // A socket's address holds about 100 characters; a long user name or
  // user data directory is longer. As upstream's ProcessSingleton does: a
  // short private directory, and a link from the usual place (compass-mcp
  // follows it).
  if (listen_path.size() >= sizeof(sockaddr_un::sun_path)) {
    char dir[] = "/tmp/compass-mcp-XXXXXX";
    if (!::mkdtemp(dir)) {
      std::move(callbacks_.listening).Run(false);
      return;
    }
    short_dir_ = base::FilePath(dir);
    listen_path = short_dir_.Append("s").value();
    if (::symlink(listen_path.c_str(), path_.value().c_str()) != 0) {
      ::rmdir(dir);
      short_dir_.clear();
      std::move(callbacks_.listening).Run(false);
      return;
    }
  }
  server_ = std::make_unique<net::UnixDomainServerSocket>(
      base::BindRepeating(&IsSameUser), /*use_abstract_namespace=*/false);
  const int result = server_->BindAndListen(listen_path, /*backlog=*/8);
  if (result != net::OK) {
    LOG(ERROR) << "MCP: can't listen on " << listen_path << ": "
               << net::ErrorToString(result);
    server_.reset();
    ::unlink(path_.value().c_str());
    std::move(callbacks_.listening).Run(false);
    return;
  }
  ::chmod(listen_path.c_str(), S_IRUSR | S_IWUSR);
  std::move(callbacks_.listening).Run(true);
  Accept();
}

McpListener::~McpListener() {
  connections_.clear();
  if (server_) {
    server_.reset();
    ::unlink(path_.value().c_str());
  }
  if (!short_dir_.empty()) {
    ::unlink(short_dir_.Append("s").value().c_str());
    ::rmdir(short_dir_.value().c_str());
  }
}

void McpListener::Accept() {
  while (server_) {
    const int result = server_->Accept(
        &accepted_,
        base::BindOnce(&McpListener::OnAccept, weak_factory_.GetWeakPtr()));
    if (result == net::ERR_IO_PENDING) {
      return;
    }
    if (result != net::OK) {
      // A refused peer (another user), or a passing error: go on.
      accepted_.reset();
      continue;
    }
    OnAccept(net::OK);
    return;
  }
}

void McpListener::OnAccept(int result) {
  if (result == net::OK && accepted_) {
    const int id = next_id_++;
    auto connection = std::make_unique<Connection>();
    connection->socket = std::move(accepted_);
    connections_.emplace(id, std::move(connection));
    callbacks_.connected.Run(id);
    Read(id);
  }
  accepted_.reset();
  // Posted: an accept that finishes at once mustn't recurse.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&McpListener::Accept, weak_factory_.GetWeakPtr()));
}

void McpListener::Read(int id) {
  auto it = connections_.find(id);
  if (it == connections_.end()) {
    return;
  }
  Connection& connection = *it->second;
  const int result = connection.socket->Read(
      connection.read_buffer.get(), kReadSize,
      base::BindOnce(&McpListener::OnRead, weak_factory_.GetWeakPtr(), id));
  if (result != net::ERR_IO_PENDING) {
    OnRead(id, result);
  }
}

void McpListener::OnRead(int id, int result) {
  auto it = connections_.find(id);
  if (it == connections_.end()) {
    return;
  }
  if (result <= 0) {
    Drop(id);
    return;
  }
  Connection& connection = *it->second;
  connection.pending.append(connection.read_buffer->data(), result);
  size_t start = 0;
  for (size_t end; (end = connection.pending.find('\n', start)) !=
                   std::string::npos;
       start = end + 1) {
    std::string line = connection.pending.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      callbacks_.line.Run(id, std::move(line));
    }
  }
  connection.pending.erase(0, start);
  if (connection.pending.size() > kMaxLine) {
    Drop(id);
    return;
  }
  // Posted, so a fast client can't starve the IO thread.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&McpListener::Read, weak_factory_.GetWeakPtr(), id));
}

void McpListener::Send(int id, std::string line) {
  auto it = connections_.find(id);
  if (it == connections_.end()) {
    return;
  }
  line += '\n';
  it->second->writes.push_back(std::move(line));
  if (!it->second->writing) {
    Write(id);
  }
}

void McpListener::Write(int id) {
  auto it = connections_.find(id);
  if (it == connections_.end()) {
    return;
  }
  Connection& connection = *it->second;
  if (!connection.writing) {
    if (connection.writes.empty()) {
      return;
    }
    std::string next = std::move(connection.writes.front());
    connection.writes.pop_front();
    const size_t size = next.size();
    auto buffer = base::MakeRefCounted<net::StringIOBuffer>(std::move(next));
    connection.writing =
        base::MakeRefCounted<net::DrainableIOBuffer>(std::move(buffer), size);
  }
  const int result = connection.socket->Write(
      connection.writing.get(), connection.writing->BytesRemaining(),
      base::BindOnce(&McpListener::OnWrite, weak_factory_.GetWeakPtr(), id),
      kTrafficAnnotation);
  if (result != net::ERR_IO_PENDING) {
    OnWrite(id, result);
  }
}

void McpListener::OnWrite(int id, int result) {
  auto it = connections_.find(id);
  if (it == connections_.end()) {
    return;
  }
  if (result < 0) {
    Drop(id);
    return;
  }
  Connection& connection = *it->second;
  connection.writing->DidConsume(result);
  if (connection.writing->BytesRemaining() == 0) {
    connection.writing.reset();
  }
  Write(id);
}

void McpListener::Close(int id) {
  // Asked by the bridge: no "closed" back.
  connections_.erase(id);
}

void McpListener::Drop(int id) {
  if (connections_.erase(id)) {
    callbacks_.closed.Run(id);
  }
}

}  // namespace compass::claude
