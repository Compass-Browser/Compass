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


# Builds for another OS than the host's, as --cpu values: out/<Config>-<target>.
# win-x64: Windows from a Mac (docs/building.md, "Windows from a Mac").
CROSS_TARGETS = ('win-x64',)


def vs_toolchain_hash(src: Path) -> str:
  """TOOLCHAIN_HASH in Chromium's build/vs_toolchain.py: the Windows toolchain
  package it wants (GYP_MSVS_HASH_<it> maps it to ours)."""
  import re
  match = re.search(r"^TOOLCHAIN_HASH = '([0-9a-f]+)'",
                    (src / 'build' / 'vs_toolchain.py').read_text(), re.M)
  if not match:
    raise ValueError('no TOOLCHAIN_HASH in build/vs_toolchain.py')
  return match.group(1)


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
    """out/<Config>, or out/<Config>-<cpu> for a build for one CPU (two of
    them make a macOS universal build, `compass universal`)."""
    name = CONFIGS[config]
    return self.src / 'out' / (name + '-' + cpu if cpu else name)

  @property
  def win_toolchain_dir(self) -> Path:
    """Where a Mac keeps the packaged Windows toolchain for cross builds
    (docs/building.md, "Windows from a Mac"): <hash>.zip, made on Windows
    by depot_tools' win_toolchain/package_from_installed.py."""
    return self.chromium_dir / 'win_toolchain'

  def win_toolchain_env(self) -> dict:
    """The variables that point Chromium's vs_toolchain.py at the packaged
    toolchain, if there is one: it otherwise wants Google's."""
    zips = sorted(self.win_toolchain_dir.glob('*.zip'))
    if not zips:
      return {}
    if len(zips) > 1:
      raise ValueError('more than one toolchain in %s' % self.win_toolchain_dir)
    return {
        'DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL': str(self.win_toolchain_dir),
        'GYP_MSVS_HASH_%s' % vs_toolchain_hash(self.src): zips[0].stem,
    }

  def env(self) -> dict:
    """Environment for running depot_tools and the build."""
    env = dict(os.environ)
    env['PATH'] = str(self.depot_tools) + os.pathsep + env.get('PATH', '')
    if sys.platform == 'win32':
      # Use the locally installed Visual Studio, not Google's internal one.
      env['DEPOT_TOOLS_WIN_TOOLCHAIN'] = '0'
    elif 'DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL' not in env:
      env.update(self.win_toolchain_env())
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
