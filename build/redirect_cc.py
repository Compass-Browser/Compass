#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Compiler wrapper that applies //compass/chromium_src overrides.

GN runs every C-family compile as `<cc_wrapper> <compiler> <args...>` from
the output directory (src/out/<Config>). For each compile this wrapper:

  1. Compiles compass/chromium_src/<path> instead of <path> if an override
     exists. The override includes the original as "src/<path>".
  2. Puts compass/chromium_src first on the include path, so an overriding
     header wins everywhere, and the checkout root last, so "src/..." in an
     override resolves without shadowing projects that use "src/" themselves.

This file is on the hot path of every compile: keep it stdlib-only and fast.
"""

import os
import subprocess
import sys

SOURCE_EXTENSIONS = ('.c', '.cc', '.cpp', '.cxx', '.m', '.mm')
COMPILE_FLAGS = ('-c', '/c')


def find_source_index(args):
  """Returns the index in |args| of the file being compiled, or None."""
  for i, arg in enumerate(args[:-1]):
    if arg in COMPILE_FLAGS and args[i + 1].endswith(SOURCE_EXTENSIONS):
      return i + 1
  return None


def rewrite_command(command, out_dir, src_dir=None):
  """Returns |command| with chromium_src overrides applied.

  |command| is [compiler, args...]. |out_dir| is the directory the compile
  runs in. Paths in the result are relative to |out_dir| like the rest of
  Chromium's command lines, so builds stay reproducible.
  """
  src_dir = src_dir or os.path.normpath(os.path.join(out_dir, '..', '..'))
  overrides_dir = os.path.join(src_dir, 'compass', 'chromium_src')

  args = list(command[1:])
  source_index = find_source_index(args)
  if source_index is None:
    return list(command)

  source = os.path.normpath(os.path.join(out_dir, args[source_index]))
  relative_source = os.path.relpath(source, src_dir)
  override = os.path.join(overrides_dir, relative_source)
  if not relative_source.startswith('..') and os.path.isfile(override):
    args[source_index] = _relative_to(override, out_dir)

  include = '/I' if 'clang-cl' in os.path.basename(command[0]) else '-I'
  # Overrides go right before the source root's include dir (-I../..), so
  # they shadow upstream headers, but not before other include dirs.
  # compass/chromium_src is as deep as those dirs, so an include like
  # "../../third_party/ffmpeg/x.c" (meant for -I../../third_party/ffmpeg)
  # would otherwise be found through the overrides dir where ".." is
  # resolved lexically (Windows, or the in-tree layout), and path-based
  # checks such as the unsafe-buffers plugin would no longer see third_party/.
  # Without a source root include dir, overrides go first.
  root_includes = {flag + _relative_to(src_dir, out_dir) + suffix
                   for flag in ('-I', '/I') for suffix in ('', '/')}
  position = next((i for i, arg in enumerate(args) if arg in root_includes),
                  0)
  args.insert(position, include + _relative_to(overrides_dir, out_dir))
  # The checkout root (for "src/..." includes in overrides) goes last:
  # projects such as V8 use "src/..." for their own headers and their include
  # dirs must win.
  return ([command[0]] + args +
          [include + _relative_to(os.path.dirname(src_dir), out_dir)])


def _relative_to(path, start):
  return os.path.relpath(path, start).replace(os.sep, '/')


def main(argv):
  if len(argv) < 2:
    sys.stderr.write('usage: redirect_cc.py <compiler> [args...]\n')
    return 2
  out_dir = os.getcwd()
  command = rewrite_command(argv[1:], out_dir,
                            os.environ.get('COMPASS_CHROMIUM_SRC'))
  if sys.platform == 'win32':
    return subprocess.call(command)
  os.execvp(command[0], command)
  return 1  # Not reached.


if __name__ == '__main__':
  sys.exit(main(sys.argv))
