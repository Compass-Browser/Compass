// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/on_device_model.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/values.h"
#include "compass/browser/claude/on_device_buildflags.h"

namespace compass::claude {

namespace {

// A reply this long isn't one these requests ask for.
constexpr size_t kMaxLine = 64 * 1024;
// A status request's whole time, the helper's start included.
constexpr base::TimeDelta kStatusTimeout = base::Seconds(10);

constexpr char kHelperName[] = "compass-on-device";

std::optional<OnDeviceModel*>& ModelForTesting() {
  static std::optional<OnDeviceModel*> model;
  return model;
}

}  // namespace

std::string DescribeOnDeviceAvailability(OnDeviceAvailability availability) {
  switch (availability) {
    case OnDeviceAvailability::kUnknown:
    case OnDeviceAvailability::kAvailable:
      return std::string();
    case OnDeviceAvailability::kUnsupported:
      return "This Mac can't run Apple Intelligence: it needs macOS 26 or "
             "later on a Mac with Apple silicon.";
    case OnDeviceAvailability::kDisabled:
      return "Apple Intelligence is turned off in System Settings.";
    case OnDeviceAvailability::kNotReady:
      return "Apple Intelligence is still downloading. Try again later.";
  }
  return std::string();
}

base::expected<std::string, OnDeviceError> ParseOnDeviceReply(
    std::string_view line) {
  std::optional<base::DictValue> reply =
      base::JSONReader::ReadDict(line, base::JSON_PARSE_RFC);
  if (!reply) {
    return base::unexpected(OnDeviceError::kFailed);
  }
  if (const std::string* text = reply->FindString("text")) {
    return *text;
  }
  const std::string* error = reply->FindString("error");
  if (error && *error == "unavailable") {
    return base::unexpected(OnDeviceError::kUnavailable);
  }
  if (error && *error == "refused") {
    return base::unexpected(OnDeviceError::kRefused);
  }
  return base::unexpected(OnDeviceError::kFailed);
}

OnDeviceAvailability ParseOnDeviceStatus(std::string_view line) {
  std::optional<base::DictValue> reply =
      base::JSONReader::ReadDict(line, base::JSON_PARSE_RFC);
  const std::string* availability =
      reply ? reply->FindString("availability") : nullptr;
  if (!availability) {
    return OnDeviceAvailability::kUnsupported;
  }
  if (*availability == "available") {
    return OnDeviceAvailability::kAvailable;
  }
  if (*availability == "disabled") {
    return OnDeviceAvailability::kDisabled;
  }
  if (*availability == "not_ready") {
    return OnDeviceAvailability::kNotReady;
  }
  return OnDeviceAvailability::kUnsupported;
}

std::optional<std::string> RunOnDeviceHelper(const base::FilePath& helper,
                                             const std::string& request,
                                             base::TimeDelta timeout) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  const base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
  int in_fds[2];
  int out_fds[2];
  if (pipe(in_fds) != 0) {
    return std::nullopt;
  }
  base::ScopedFD in_read(in_fds[0]);
  base::ScopedFD in_write(in_fds[1]);
  if (pipe(out_fds) != 0) {
    return std::nullopt;
  }
  base::ScopedFD out_read(out_fds[0]);
  base::ScopedFD out_write(out_fds[1]);

  base::LaunchOptions options;
  options.fds_to_remap.emplace_back(in_read.get(), STDIN_FILENO);
  options.fds_to_remap.emplace_back(out_write.get(), STDOUT_FILENO);
  base::Process process =
      base::LaunchProcess(base::CommandLine(helper), options);
  in_read.reset();
  out_write.reset();
  if (!process.IsValid()) {
    return std::nullopt;
  }

  // The request is small: the pipe holds it whether or not the helper reads
  // yet. Closing it tells the helper there's no other.
  const bool written = base::WriteFileDescriptor(in_write.get(), request) &&
                       base::WriteFileDescriptor(in_write.get(), "\n");
  in_write.reset();

  std::string output;
  bool done = false;
  while (written && !done) {
    const base::TimeDelta left = deadline - base::TimeTicks::Now();
    if (!left.is_positive()) {
      break;
    }
    struct pollfd poll_fd = {out_read.get(), POLLIN, 0};
    const int ready =
        HANDLE_EINTR(poll(&poll_fd, 1, left.InMillisecondsRoundedUp()));
    if (ready <= 0) {
      break;
    }
    char buffer[4096];
    const ssize_t read =
        HANDLE_EINTR(::read(out_read.get(), buffer, sizeof(buffer)));
    if (read <= 0) {
      break;
    }
    output.append(buffer, static_cast<size_t>(read));
    const size_t end = output.find('\n');
    if (end != std::string::npos) {
      output.resize(end);
      done = true;
    } else if (output.size() > kMaxLine) {
      break;
    }
  }
  out_read.reset();
  if (!done) {
    process.Terminate(/*exit_code=*/1, /*wait=*/false);
  }
  // It ends once its stdin is closed; reaped, or killed after a while, off
  // this sequence.
  base::EnsureProcessTerminated(std::move(process));
  if (!done) {
    return std::nullopt;
  }
  return output;
}

OnDeviceModel::OnDeviceModel(base::FilePath helper)
    : helper_(std::move(helper)),
      runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}

OnDeviceModel::~OnDeviceModel() = default;

// static
OnDeviceModel* OnDeviceModel::Get() {
  if (ModelForTesting().has_value()) {
    return *ModelForTesting();
  }
#if BUILDFLAG(COMPASS_ON_DEVICE_MODEL)
  static base::NoDestructor<OnDeviceModel> model([] {
    base::FilePath exe_dir;
    base::PathService::Get(base::DIR_EXE, &exe_dir);
    return exe_dir.AppendASCII(kHelperName);
  }());
  return model.get();
#else
  return nullptr;
#endif
}

// static
void OnDeviceModel::SetForTesting(OnDeviceModel* model) {
  if (model) {
    ModelForTesting() = model;
  } else {
    ModelForTesting().reset();
  }
}

void OnDeviceModel::CheckAvailability(AvailabilityCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::string request;
  base::JSONWriter::Write(
      base::DictValue().Set("id", next_id_++).Set("status", true), &request);
  runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&RunOnDeviceHelper, helper_, std::move(request),
                     kStatusTimeout),
      base::BindOnce(&OnDeviceModel::OnStatus, weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

void OnDeviceModel::Ask(std::string_view feature,
                        std::string_view system,
                        std::string_view user,
                        base::TimeDelta timeout,
                        ReplyCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::string request;
  base::JSONWriter::Write(base::DictValue()
                              .Set("id", next_id_++)
                              .Set("feature", feature)
                              .Set("system", system)
                              .Set("user", user),
                          &request);
  const base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
  runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath helper, std::string request,
             base::TimeTicks deadline) -> std::optional<std::string> {
            // Waited out behind earlier requests: not worth starting.
            const base::TimeDelta left = deadline - base::TimeTicks::Now();
            if (!left.is_positive()) {
              return std::nullopt;
            }
            return RunOnDeviceHelper(helper, request, left);
          },
          helper_, std::move(request), deadline),
      base::BindOnce(&OnDeviceModel::OnReply, weak_factory_.GetWeakPtr(),
                     std::move(callback), deadline));
}

void OnDeviceModel::OnStatus(AvailabilityCallback callback,
                             std::optional<std::string> line) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  availability_ = line ? ParseOnDeviceStatus(*line)
                       : OnDeviceAvailability::kUnsupported;
  std::move(callback).Run(availability_);
}

void OnDeviceModel::OnReply(ReplyCallback callback,
                            base::TimeTicks deadline,
                            std::optional<std::string> line) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!line) {
    std::move(callback).Run(base::unexpected(
        base::TimeTicks::Now() >= deadline ? OnDeviceError::kTimeout
                                           : OnDeviceError::kFailed));
    return;
  }
  base::expected<std::string, OnDeviceError> reply = ParseOnDeviceReply(*line);
  if (reply.has_value()) {
    availability_ = OnDeviceAvailability::kAvailable;
  } else if (reply.error() == OnDeviceError::kUnavailable) {
    // Turned off, or gone, since the last check: the settings ask again.
    availability_ = OnDeviceAvailability::kUnknown;
  }
  std::move(callback).Run(std::move(reply));
}

}  // namespace compass::claude
