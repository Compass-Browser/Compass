// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_code_process.h"

#include <optional>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/process/launch.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)
#include <fcntl.h>
#include <unistd.h>
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace compass::claude {

namespace {

// A line of output larger than this (it would be a screenshot's result
// many times over) ends the reading.
constexpr size_t kMaxLine = 64 * 1024 * 1024;

// A pipe: |read| and |write| ends, neither inherited by children until
// LaunchProcess() passes the child's end explicitly.
bool MakePipe(base::File* read, base::File* write) {
#if BUILDFLAG(IS_POSIX)
  int fds[2];
  if (pipe(fds) != 0) {
    return false;
  }
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *read = base::File(fds[0]);
  *write = base::File(fds[1]);
  return true;
#elif BUILDFLAG(IS_WIN)
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), nullptr, TRUE};
  HANDLE read_handle = nullptr;
  HANDLE write_handle = nullptr;
  if (!CreatePipe(&read_handle, &write_handle, &attributes, 0)) {
    return false;
  }
  *read = base::File(read_handle);
  *write = base::File(write_handle);
  return true;
#endif
}

// Reads |out| to its end, a line at a time, then waits for |process|.
void ReadOutput(base::File out,
                base::Process process,
                ClaudeCodeProcess::LineCallback on_line,
                ClaudeCodeProcess::ExitCallback on_exit) {
  std::string pending;
  std::vector<uint8_t> buffer(64 * 1024);
  for (;;) {
    std::optional<size_t> got;
    {
      base::ScopedBlockingCall blocking(FROM_HERE,
                                        base::BlockingType::WILL_BLOCK);
      got = out.ReadAtCurrentPosNoBestEffort(base::span(buffer));
    }
    if (!got || *got == 0) {
      break;
    }
    pending.append(base::as_string_view(base::span(buffer).first(*got)));
    size_t start = 0;
    for (size_t end; (end = pending.find('\n', start)) != std::string::npos;
         start = end + 1) {
      std::string line = pending.substr(start, end - start);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!line.empty()) {
        on_line.Run(std::move(line));
      }
    }
    pending.erase(0, start);
    if (pending.size() > kMaxLine) {
      LOG(ERROR) << "Claude Code: a line of output too long; stopped reading";
      break;
    }
  }
  if (!pending.empty()) {
    on_line.Run(std::move(pending));
  }
  int exit_code = -1;
  {
    base::ScopedBlockingCall blocking(FROM_HERE,
                                      base::BlockingType::WILL_BLOCK);
    if (!process.WaitForExitWithTimeout(base::Seconds(5), &exit_code)) {
      exit_code = -1;
    }
  }
  std::move(on_exit).Run(exit_code);
}

}  // namespace

// Owns the stdin pipe on a blocking sequence: a write may wait for the
// process to read.
class ClaudeCodeProcess::Stdin {
 public:
  explicit Stdin(base::File file) : file_(std::move(file)) {}

  void Write(std::string data) {
    if (!file_.IsValid()) {
      return;
    }
    base::ScopedBlockingCall blocking(FROM_HERE,
                                      base::BlockingType::WILL_BLOCK);
    if (!file_.WriteAtCurrentPosAndCheck(base::as_byte_span(data))) {
      // The process went; its exit says why.
      file_.Close();
    }
  }

 private:
  base::File file_;
};

// static
std::unique_ptr<ClaudeCodeProcess> ClaudeCodeProcess::Start(
    const base::FilePath& program,
    const std::vector<std::string>& args,
    const base::FilePath& directory,
    LineCallback on_line,
    ExitCallback on_exit) {
  base::File stdin_read, stdin_write, out_read, out_write;
  if (!MakePipe(&stdin_read, &stdin_write) ||
      !MakePipe(&out_read, &out_write)) {
    return nullptr;
  }
  base::CommandLine command(program);
  for (const std::string& arg : args) {
#if BUILDFLAG(IS_WIN)
    command.AppendArgNative(base::UTF8ToWide(arg));
#else
    command.AppendArg(arg);
#endif
  }
  base::LaunchOptions options;
  options.current_directory = directory;
#if BUILDFLAG(IS_POSIX)
  options.fds_to_remap.emplace_back(stdin_read.GetPlatformFile(),
                                    STDIN_FILENO);
  options.fds_to_remap.emplace_back(out_write.GetPlatformFile(),
                                    STDOUT_FILENO);
  options.fds_to_remap.emplace_back(out_write.GetPlatformFile(),
                                    STDERR_FILENO);
  // Its own process group, so it isn't stopped with the browser's terminal
  // (development builds started from a shell).
  options.new_process_group = true;
#elif BUILDFLAG(IS_WIN)
  // Only the child's ends are inherited.
  SetHandleInformation(stdin_write.GetPlatformFile(), HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_read.GetPlatformFile(), HANDLE_FLAG_INHERIT, 0);
  options.stdin_handle = stdin_read.GetPlatformFile();
  options.stdout_handle = out_write.GetPlatformFile();
  options.stderr_handle = out_write.GetPlatformFile();
  options.handles_to_inherit.push_back(stdin_read.GetPlatformFile());
  options.handles_to_inherit.push_back(out_write.GetPlatformFile());
  options.start_hidden = true;
#endif
  base::Process process = base::LaunchProcess(command, options);
  if (!process.IsValid()) {
    return nullptr;
  }
  // The child's ends close here; the output ends when it exits.
  stdin_read.Close();
  out_write.Close();

  auto self = base::WrapUnique(new ClaudeCodeProcess(
      process.Duplicate(), std::move(on_line), std::move(on_exit)));
  self->stdin_ = base::SequenceBound<Stdin>(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN}),
      std::move(stdin_write));
  scoped_refptr<base::SequencedTaskRunner> here =
      base::SequencedTaskRunner::GetCurrentDefault();
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::WithBaseSyncPrimitives(),
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          &ReadOutput, std::move(out_read), std::move(process),
          base::BindPostTask(
              here, base::BindRepeating(&ClaudeCodeProcess::OnLine,
                                        self->weak_factory_.GetWeakPtr())),
          base::BindPostTask(
              here, base::BindOnce(&ClaudeCodeProcess::OnExit,
                                   self->weak_factory_.GetWeakPtr()))));
  return self;
}

ClaudeCodeProcess::ClaudeCodeProcess(base::Process process,
                                     LineCallback on_line,
                                     ExitCallback on_exit)
    : process_(std::move(process)),
      on_line_(std::move(on_line)),
      on_exit_(std::move(on_exit)) {}

ClaudeCodeProcess::~ClaudeCodeProcess() {
  if (process_.IsValid()) {
    // Its MCP relay goes when the bridge closes the chat's connection.
    process_.Terminate(0, /*wait=*/false);
  }
}

void ClaudeCodeProcess::Write(std::string data) {
  stdin_.AsyncCall(&Stdin::Write).WithArgs(std::move(data));
}

void ClaudeCodeProcess::OnLine(std::string line) {
  on_line_.Run(std::move(line));
}

void ClaudeCodeProcess::OnExit(int exit_code) {
  if (on_exit_) {
    std::move(on_exit_).Run(exit_code);
  }
}

}  // namespace compass::claude
