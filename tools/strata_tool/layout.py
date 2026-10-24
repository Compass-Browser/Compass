# Copyright 2026 The Strata Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Where everything lives on disk."""

from __future__ import annotations

import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

CONFIGS = {'debug': 'Debug', 'fast': 'Fast', 'release': 'Release'}


class Layout:
  """Paths for one Strata checkout.

  Two layouts are supported:
    linked:  <repo>/ next to <chromium_dir>/src, with src/strata -> <repo>
    in-tree: the repo is cloned directly to <chromium_dir>/src/strata
  """

  def __init__(self, root: Path, chromium_dir: Path, in_tree: bool):
    self.root = root
    self.chromium_dir = chromium_dir
    self.in_tree = in_tree

  @property
  def src(self) -> Path:
    return self.chromium_dir / 'src'

  @property
  def depot_tools(self) -> Path:
    return self.chromium_dir / 'depot_tools'

  @property
  def patches_dir(self) -> Path:
    return self.root / 'patches'

  @property
  def overrides_dir(self) -> Path:
    return self.root / 'chromium_src'

  @property
  def version_file(self) -> Path:
    return self.root / 'config' / 'chromium_version'

  def chromium_version(self) -> str:
    return self.version_file.read_text().strip()

  def out_dir(self, config: str) -> Path:
    return self.src / 'out' / CONFIGS[config]

  def env(self) -> dict:
    """Environment for running depot_tools and the build."""
    env = dict(os.environ)
    env['PATH'] = str(self.depot_tools) + os.pathsep + env.get('PATH', '')
    if sys.platform == 'win32':
      # Use the locally installed Visual Studio, not Google's internal one.
      env['DEPOT_TOOLS_WIN_TOOLCHAIN'] = '0'
    return env


def detect(root: Path = REPO_ROOT, environ=None) -> Layout:
  environ = os.environ if environ is None else environ
  if root.name == 'strata' and (root.parent / '.gn').is_file():
    return Layout(root, root.parent.parent, in_tree=True)
  chromium_dir = environ.get('STRATA_CHROMIUM_DIR')
  if chromium_dir:
    return Layout(root, Path(chromium_dir).expanduser().resolve(), False)
  return Layout(root, root.parent / 'strata-chromium', in_tree=False)
