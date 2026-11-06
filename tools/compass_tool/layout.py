# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Where everything lives on disk."""

from __future__ import annotations

import os
import platform
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# portable: like fast, but not a component build, so the .app runs on another
# Mac (e.g. an x64 build copied off an arm64 build machine).
CONFIGS = {
    'debug': 'Debug',
    'fast': 'Fast',
    'portable': 'Portable',
    'release': 'Release',
}


def host_cpu(machine: str | None = None) -> str:
  """The host as a GN target_cpu: arm64 or x64."""
  machine = (machine or platform.machine()).lower()
  return {'x86_64': 'x64', 'amd64': 'x64', 'aarch64': 'arm64'}.get(
      machine, machine)


class Layout:
  """Paths for one Compass checkout.

  Two layouts are supported:
    linked:  <repo>/ next to <chromium_dir>/src, with src/compass -> <repo>
    in-tree: the repo is cloned directly to <chromium_dir>/src/compass
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

  def out_dir(self, config: str, cpu: str | None = None) -> Path:
    """out/<Config>, or out/<Config>-<cpu> when cross-building, so an x64
    build doesn't overwrite the host's."""
    name = CONFIGS[config]
    if cpu and cpu != host_cpu():
      name += '-' + cpu
    return self.src / 'out' / name

  def env(self) -> dict:
    """Environment for running depot_tools and the build."""
    env = dict(os.environ)
    env['PATH'] = str(self.depot_tools) + os.pathsep + env.get('PATH', '')
    if sys.platform == 'win32':
      # Use the locally installed Visual Studio, not Google's internal one.
      env['DEPOT_TOOLS_WIN_TOOLCHAIN'] = '0'
    return env


CHROMIUM_DIR_NAMES = ('compass-chromium', 'strata-chromium')


def detect(root: Path = REPO_ROOT, environ=None) -> Layout:
  environ = os.environ if environ is None else environ
  if root.name == 'compass' and root.parent.name == 'src' and (
      (root.parent / '.gn').is_file() or
      root.parent.parent.name in CHROMIUM_DIR_NAMES):
    # In-tree, either checked out already or cloned there before `init`.
    return Layout(root, root.parent.parent, in_tree=True)
  chromium_dir = environ.get('COMPASS_CHROMIUM_DIR')
  if chromium_dir:
    return Layout(root, Path(chromium_dir).expanduser().resolve(), False)
  chromium_dir = root.parent / 'compass-chromium'
  # Checkouts made before the project became Compass (Strata).
  legacy_dir = root.parent / 'strata-chromium'
  if not chromium_dir.exists() and legacy_dir.exists():
    chromium_dir = legacy_dir
  return Layout(root, chromium_dir, in_tree=False)
