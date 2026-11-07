// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_CODE_PROCESS_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_CODE_PROCESS_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/threading/sequence_bound.h"

namespace compass::claude {

// A running `claude` (Claude Code mode): its stdin takes stream-json lines,
// its stdout (stderr too, in the same pipe) comes back a line at a time on
// the sequence that started it. Destroying it ends the process.
class ClaudeCodeProcess {
 public:
  // A line of output, without its newline.
  using LineCallback = base::RepeatingCallback<void(std::string line)>;
  // The output ended and the process exited (-1 if its code is unknown).
  using ExitCallback = base::OnceCallback<void(int exit_code)>;

  // Starts |program| with |args| in |directory|. Null if it couldn't start.
  // The callbacks don't run after the object is destroyed.
  static std::unique_ptr<ClaudeCodeProcess> Start(
      const base::FilePath& program,
      const std::vector<std::string>& args,
      const base::FilePath& directory,
      LineCallback on_line,
      ExitCallback on_exit);

  ClaudeCodeProcess(const ClaudeCodeProcess&) = delete;
  ClaudeCodeProcess& operator=(const ClaudeCodeProcess&) = delete;
  ~ClaudeCodeProcess();

  // Writes |data| to its stdin, in order, off this sequence.
  void Write(std::string data);

 private:
  class Stdin;

  ClaudeCodeProcess(base::Process process, LineCallback on_line,
                    ExitCallback on_exit);
  void OnLine(std::string line);
  void OnExit(int exit_code);

  base::Process process_;
  base::SequenceBound<Stdin> stdin_;
  LineCallback on_line_;
  ExitCallback on_exit_;
  base::WeakPtrFactory<ClaudeCodeProcess> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_CODE_PROCESS_H_
