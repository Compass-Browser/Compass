# Copyright 2026 The Strata Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Keeps ninja aware of chromium_src overrides being added or removed.

Once an override has been compiled, the compiler's depfile tracks it. Ninja
can't know that a *new* override exists, or that one was deleted, so we touch
the upstream file in both cases to make ninja rebuild it.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import List, Set

STAMP = '.strata_overrides.json'
IGNORED_NAMES = {'README.md', '.DS_Store'}


def list_overrides(overrides_dir: Path) -> Set[str]:
  if not overrides_dir.is_dir():
    return set()
  return {path.relative_to(overrides_dir).as_posix()
          for path in overrides_dir.rglob('*')
          if path.is_file() and path.name not in IGNORED_NAMES}


def orphans(overrides_dir: Path, src: Path) -> List[str]:
  """Overrides whose upstream file doesn't exist (renamed in a rebase?)."""
  return sorted(path for path in list_overrides(overrides_dir)
                if not (src / path).is_file())


def sync(overrides_dir: Path, src: Path, out_dir: Path) -> List[str]:
  """Touches upstream files whose override appeared or disappeared."""
  stamp = out_dir / STAMP
  previous = set(json.loads(stamp.read_text())) if stamp.is_file() else set()
  current = list_overrides(overrides_dir)
  touched = []
  for path in sorted(current ^ previous):
    upstream = src / path
    if upstream.is_file():
      os.utime(upstream)
      touched.append(path)
  out_dir.mkdir(parents=True, exist_ok=True)
  stamp.write_text(json.dumps(sorted(current), indent=1))
  return touched
