#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""The static-library step of Chromium's Linux toolchain on a Windows host.

Usage: ar_wrapper.py <output> <ar> <args...>

Upstream runs `cmd /s /c "<python> tool_wrapper.py delete-file <output> &&
<ar> ..."` there, and cmd.exe misreads the quoted forward-slash paths siso
passes it. This does the same without cmd.exe: removes <output>, so ar
doesn't add to an old archive, then runs ar. Used by
build-toolchain-gcc_toolchain.gni.patch (docs/building.md, "Linux from
Windows").
"""

import os
import subprocess
import sys


def main(argv):
  output, command = argv[1], argv[2:]
  try:
    os.remove(output)
  except FileNotFoundError:
    pass
  return subprocess.call(command)


if __name__ == '__main__':
  sys.exit(main(sys.argv))
