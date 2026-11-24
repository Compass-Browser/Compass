// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_ON_DEVICE_MODEL_H_
#define COMPASS_BROWSER_CLAUDE_ON_DEVICE_MODEL_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/types/expected.h"

namespace base {
class SequencedTaskRunner;
}  // namespace base

// Apple Intelligence for the sidebar's small requests on the Mac
// (docs/design/ai-sidebar.md, "On this device"), through the
// compass-on-device helper (tools/on_device/) next to the browser.
namespace compass::claude {

// Whether the on-device model can answer. The helper's "availability"
// values (CompassKit's OnDeviceAvailability).
enum class OnDeviceAvailability {
  // Not asked yet.
  kUnknown,
  kAvailable,
  // An older macOS, or a Mac that can't run Apple Intelligence (or the
  // helper didn't start).
  kUnsupported,
  // Apple Intelligence is turned off in System Settings.
  kDisabled,
  // The model is still downloading.
  kNotReady,
};

// What the settings say for |availability|; empty for kAvailable and
// kUnknown.
std::string DescribeOnDeviceAvailability(OnDeviceAvailability availability);

enum class OnDeviceError {
  // The model can't answer now (see OnDeviceAvailability).
  kUnavailable,
  // Its guardrails, or the model, declined.
  kRefused,
  // The helper failed, or answered with anything else.
  kFailed,
  // No answer in time; the helper was stopped.
  kTimeout,
};

// The reply to a request line (the helper's protocol, CompassKit's
// OnDeviceLines): the JSON text the feature asked for, or why not. A line
// that isn't a reply is kFailed.
base::expected<std::string, OnDeviceError> ParseOnDeviceReply(
    std::string_view line);
// A status reply's availability; kUnsupported for anything else.
OnDeviceAvailability ParseOnDeviceStatus(std::string_view line);

// Runs |helper| with |request| (one line) on its stdin, closed after it,
// and returns the first line it writes to stdout, without the line break.
// Null if it can't be started, writes no line, or takes longer than
// |timeout|: then it's stopped. Blocking.
std::optional<std::string> RunOnDeviceHelper(const base::FilePath& helper,
                                             const std::string& request,
                                             base::TimeDelta timeout);

// The browser's on-device model: one helper process for each request, one
// request at a time, on a background sequence. Lives as long as the
// browser; use on the UI thread.
class OnDeviceModel {
 public:
  using AvailabilityCallback = base::OnceCallback<void(OnDeviceAvailability)>;
  using ReplyCallback =
      base::OnceCallback<void(base::expected<std::string, OnDeviceError>)>;

  explicit OnDeviceModel(base::FilePath helper);
  OnDeviceModel(const OnDeviceModel&) = delete;
  OnDeviceModel& operator=(const OnDeviceModel&) = delete;
  ~OnDeviceModel();

  // The browser's, with the helper in Contents/MacOS; null in builds
  // without it (compass_on_device_model = false) and off the Mac.
  static OnDeviceModel* Get();
  // Get() returns |model| instead, until called with nullptr.
  static void SetForTesting(OnDeviceModel* model);

  // What the last status request or answer said; kUnknown before either.
  OnDeviceAvailability availability() const { return availability_; }

  // Asks the helper whether the model can answer now; also updates
  // availability().
  void CheckAvailability(AvailabilityCallback callback);

  // |feature|'s reply ("folder_name", "tidy_today", "short_titles": a
  // prompt's id) to |user|, told |system|: the JSON object's text, or why
  // not. Within |timeout| of the call, also while waiting for an earlier
  // request. Never calls back before Ask() returns.
  void Ask(std::string_view feature,
           std::string_view system,
           std::string_view user,
           base::TimeDelta timeout,
           ReplyCallback callback);

 private:
  void OnStatus(AvailabilityCallback callback, std::optional<std::string> line);
  void OnReply(ReplyCallback callback,
               base::TimeTicks deadline,
               std::optional<std::string> line);

  const base::FilePath helper_;
  // One request at a time: the model answers one well, not several.
  const scoped_refptr<base::SequencedTaskRunner> runner_;
  OnDeviceAvailability availability_ = OnDeviceAvailability::kUnknown;
  int next_id_ = 1;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<OnDeviceModel> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_ON_DEVICE_MODEL_H_
