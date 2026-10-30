# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Running external commands."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


class CommandError(Exception):
  pass


def depot_tool(name: str) -> str:
  """depot_tools entry points are .bat files on Windows."""
  return name + '.bat' if sys.platform == 'win32' else name


def run(cmd, cwd: Path, env=None, check=True, capture=False,
        quiet=False) -> subprocess.CompletedProcess:
  cmd = [str(c) for c in cmd]
  if not quiet:
    print('$ ' + ' '.join(cmd), flush=True)
  result = subprocess.run(cmd, cwd=str(cwd), env=env, text=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None)
  if check and result.returncode != 0:
    detail = (result.stderr or '').strip() if capture else ''
    raise CommandError('command failed (%d): %s%s' %
                       (result.returncode, ' '.join(cmd),
                        '\n' + detail if detail else ''))
  return result


def git(args, cwd: Path, check=True) -> subprocess.CompletedProcess:
  return run(['git'] + list(args), cwd, check=check, capture=True, quiet=True)
