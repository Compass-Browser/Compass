// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/mcp_listener.h"

#include <windows.h>

#include <sddl.h>

#include <deque>
#include <optional>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/message_loop/message_pump_win.h"
#include "base/task/current_thread.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/win/access_token.h"
#include "base/win/scoped_handle.h"
#include "base/win/sid.h"
#include "compass/tools/mcp/mcp_pipe_name_win.h"

namespace compass::claude {

namespace {

constexpr DWORD kReadSize = 64 * 1024;
// As on POSIX: a message longer than this closes the connection.
constexpr size_t kMaxLine = 8 * 1024 * 1024;
// A pipe instance that couldn't be made (after the first) is tried again.
constexpr base::TimeDelta kRetryDelay = base::Seconds(1);

// A new instance of |name| that only this user may open, for local clients
// only. The first must be the pipe's first instance: if another process
// already made one (to pose as Compass), listening fails.
base::win::ScopedHandle CreatePipeInstance(const std::wstring& name,
                                           bool first) {
  std::optional<base::win::AccessToken> token =
      base::win::AccessToken::FromCurrentProcess();
  std::optional<std::wstring> user =
      token ? token->User().ToSddlString() : std::nullopt;
  if (!user) {
    return base::win::ScopedHandle();
  }
  // Protected DACL: full access for this user, nobody else.
  const std::wstring sddl = L"D:P(A;;GA;;;" + *user + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    return base::win::ScopedHandle();
  }
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
  base::win::ScopedHandle pipe(::CreateNamedPipeW(
      name.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
          (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      PIPE_UNLIMITED_INSTANCES, kReadSize, kReadSize, /*nDefaultTimeOut=*/0,
      &attributes));
  ::LocalFree(descriptor);
  return pipe;
}

}  // namespace

McpListener::Callbacks::Callbacks() = default;
McpListener::Callbacks::Callbacks(Callbacks&&) = default;
McpListener::Callbacks& McpListener::Callbacks::operator=(Callbacks&&) =
    default;
McpListener::Callbacks::~Callbacks() = default;

// One pipe instance: it waits for a client, then is that connection. Each
// overlapped operation holds a reference until its completion comes, so
// the instance outlives what the IO thread's completion port still has.
class McpListener::Pipe : public base::MessagePumpForIO::IOHandler,
                          public base::RefCounted<Pipe> {
 public:
  Pipe(base::win::ScopedHandle handle, base::WeakPtr<McpListener> listener)
      : base::MessagePumpForIO::IOHandler(FROM_HERE),
        handle_(std::move(handle)),
        listener_(std::move(listener)),
        read_buffer_(kReadSize) {}
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  // Waits for a client. False if it can't.
  bool Listen() {
    if (!base::CurrentIOThread::Get()->RegisterIOHandler(handle_.get(),
                                                         this)) {
      return false;
    }
    AddRef();
    if (::ConnectNamedPipe(handle_.get(),
                           connect_context_.GetOverlapped())) {
      // Overlapped: completes through the port anyway.
      return true;
    }
    switch (::GetLastError()) {
      case ERROR_IO_PENDING:
        return true;
      case ERROR_PIPE_CONNECTED:
        // A client came between creating and connecting: no completion
        // comes for this one.
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE,
            base::BindOnce(&Pipe::OnIOCompleted, base::WrapRefCounted(this),
                           &connect_context_, DWORD{0},
                           DWORD{ERROR_SUCCESS}));
        return true;
      default:
        Release();
        return false;
    }
  }

  void StartReading(int id) {
    id_ = id;
    Read();
  }

  void Send(std::string line) {
    if (closed_) {
      return;
    }
    line += '\n';
    writes_.push_back(std::move(line));
    if (!writing_) {
      Write();
    }
  }

  // No more callbacks; pending operations end (aborted) and let go.
  void Close() {
    closed_ = true;
    ::CancelIoEx(handle_.get(), nullptr);
  }

  // base::MessagePumpForIO::IOHandler:
  void OnIOCompleted(base::MessagePumpForIO::IOContext* context,
                     DWORD bytes,
                     DWORD error) override {
    // The operation's reference goes; |self| keeps this alive meanwhile.
    scoped_refptr<Pipe> self(this);
    Release();
    if (context == &connect_context_) {
      if (closed_ || !listener_) {
        return;
      }
      if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED) {
        // A client that went before it was connected: wait for another.
        closed_ = true;
        listener_->OnPipeClosed(id_);
        listener_->Accept();
        return;
      }
      listener_->OnConnected(this);
    } else if (context == &read_context_) {
      reading_ = false;
      OnRead(bytes, error);
    } else if (context == &write_context_) {
      writing_ = false;
      OnWrite(bytes, error);
    }
  }

 private:
  friend class base::RefCounted<Pipe>;
  ~Pipe() override = default;

  void Read() {
    if (closed_ || reading_) {
      return;
    }
    reading_ = true;
    AddRef();
    if (::ReadFile(handle_.get(), read_buffer_.data(), kReadSize, nullptr,
                   read_context_.GetOverlapped()) ||
        ::GetLastError() == ERROR_IO_PENDING) {
      return;
    }
    reading_ = false;
    Release();
    Drop();
  }

  void OnRead(DWORD bytes, DWORD error) {
    if (closed_) {
      return;
    }
    if (error != ERROR_SUCCESS || bytes == 0) {
      Drop();
      return;
    }
    pending_.append(read_buffer_.data(), bytes);
    size_t start = 0;
    for (size_t end;
         (end = pending_.find('\n', start)) != std::string::npos;
         start = end + 1) {
      std::string line = pending_.substr(start, end - start);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!line.empty() && listener_) {
        listener_->OnLine(id_, std::move(line));
        if (closed_) {
          return;
        }
      }
    }
    pending_.erase(0, start);
    if (pending_.size() > kMaxLine) {
      Drop();
      return;
    }
    Read();
  }

  void Write() {
    if (closed_ || writing_) {
      return;
    }
    if (writing_data_.empty()) {
      if (writes_.empty()) {
        return;
      }
      writing_data_ = std::move(writes_.front());
      writes_.pop_front();
    }
    writing_ = true;
    AddRef();
    if (::WriteFile(handle_.get(), writing_data_.data(),
                    static_cast<DWORD>(writing_data_.size()), nullptr,
                    write_context_.GetOverlapped()) ||
        ::GetLastError() == ERROR_IO_PENDING) {
      return;
    }
    writing_ = false;
    Release();
    Drop();
  }

  void OnWrite(DWORD bytes, DWORD error) {
    if (closed_) {
      return;
    }
    if (error != ERROR_SUCCESS) {
      Drop();
      return;
    }
    writing_data_.erase(0, bytes);
    Write();
  }

  // The client went, or the connection failed.
  void Drop() {
    if (closed_) {
      return;
    }
    Close();
    if (listener_) {
      listener_->OnPipeClosed(id_);
    }
  }

  base::win::ScopedHandle handle_;
  base::WeakPtr<McpListener> listener_;
  // 0 until connected.
  int id_ = 0;
  bool closed_ = false;
  base::MessagePumpForIO::IOContext connect_context_;
  base::MessagePumpForIO::IOContext read_context_;
  base::MessagePumpForIO::IOContext write_context_;
  bool reading_ = false;
  bool writing_ = false;
  std::vector<char> read_buffer_;
  std::string pending_;
  std::deque<std::string> writes_;
  std::string writing_data_;
};

McpListener::McpListener(base::FilePath path, Callbacks callbacks)
    : pipe_name_(mcp::McpPipeName(path.DirName().value())),
      callbacks_(std::move(callbacks)) {
  Accept();
}

McpListener::~McpListener() {
  weak_factory_.InvalidateWeakPtrs();
  if (waiting_) {
    waiting_->Close();
  }
  for (auto& [id, pipe] : connections_) {
    pipe->Close();
  }
}

void McpListener::Accept() {
  if (waiting_) {
    return;
  }
  base::win::ScopedHandle handle =
      CreatePipeInstance(pipe_name_, first_instance_);
  auto pipe = handle.is_valid()
                  ? base::MakeRefCounted<Pipe>(std::move(handle),
                                               weak_factory_.GetWeakPtr())
                  : nullptr;
  if (!pipe || !pipe->Listen()) {
    if (first_instance_) {
      LOG(ERROR) << "MCP: can't listen on " << pipe_name_ << ": "
                 << ::GetLastError();
      std::move(callbacks_.listening).Run(false);
      return;
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&McpListener::Accept, weak_factory_.GetWeakPtr()),
        kRetryDelay);
    return;
  }
  waiting_ = std::move(pipe);
  if (first_instance_) {
    first_instance_ = false;
    std::move(callbacks_.listening).Run(true);
  }
}

void McpListener::OnConnected(Pipe* pipe) {
  if (waiting_.get() != pipe) {
    return;
  }
  const int id = next_id_++;
  scoped_refptr<Pipe> connected = std::move(waiting_);
  connections_.emplace(id, connected);
  callbacks_.connected.Run(id);
  connected->StartReading(id);
  // Posted: the next instance's connect may complete at once.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&McpListener::Accept, weak_factory_.GetWeakPtr()));
}

void McpListener::OnLine(int id, std::string line) {
  if (connections_.contains(id)) {
    callbacks_.line.Run(id, std::move(line));
  }
}

void McpListener::OnPipeClosed(int id) {
  if (id == 0) {
    // The waiting instance's client left before connecting.
    waiting_ = nullptr;
    return;
  }
  if (connections_.erase(id)) {
    callbacks_.closed.Run(id);
  }
}

void McpListener::Send(int id, std::string line) {
  auto it = connections_.find(id);
  if (it != connections_.end()) {
    it->second->Send(std::move(line));
  }
}

void McpListener::Close(int id) {
  // Asked by the bridge: no "closed" back.
  auto it = connections_.find(id);
  if (it == connections_.end()) {
    return;
  }
  it->second->Close();
  connections_.erase(it);
}

}  // namespace compass::claude
