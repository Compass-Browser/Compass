# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Sparkle, the macOS updater (docs/design/updates.md).

`compass build` fetches the pinned release into third_party/sparkle/
(gitignored):

- sdk/Sparkle.framework: the whole framework, for its headers;
- bundle/Sparkle.framework: what Compass.app ships, without headers, module
  maps or the XPC services (only sandboxed apps need those);
- bin/: generate_keys, sign_update and BinaryDelta, for `compass release`.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import tempfile
import urllib.request
from pathlib import Path

VERSION = '2.10.0'
URL = ('https://github.com/sparkle-project/Sparkle/releases/download/'
       '{0}/Sparkle-{0}.tar.xz'.format(VERSION))
SHA256 = 'c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c'

# Not shipped inside Compass.app.
_UNSHIPPED = ('Headers', 'PrivateHeaders', 'Modules', 'XPCServices')


def sparkle_dir(root: Path) -> Path:
  return root / 'third_party' / 'sparkle'


def tool(root: Path, name: str) -> Path:
  return sparkle_dir(root) / 'bin' / name


def _stamp(root: Path) -> Path:
  return sparkle_dir(root) / 'VERSION'


def is_current(root: Path) -> bool:
  stamp = _stamp(root)
  return stamp.is_file() and stamp.read_text().strip() == VERSION


def prune(framework: Path) -> None:
  """Removes what Compass doesn't ship, in every version and its links."""
  versions = framework / 'Versions'
  for version in versions.iterdir() if versions.is_dir() else []:
    if version.is_symlink():
      continue
    for name in _UNSHIPPED:
      path = version / name
      if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
  for name in _UNSHIPPED:
    link = framework / name
    if link.is_symlink():
      link.unlink()


def ensure(root: Path) -> Path:
  """Fetches Sparkle if third_party/sparkle doesn't have VERSION."""
  target = sparkle_dir(root)
  if is_current(root):
    return target
  print('fetching Sparkle %s' % VERSION)
  with tempfile.TemporaryDirectory() as tmp:
    archive = Path(tmp) / 'sparkle.tar.xz'
    with urllib.request.urlopen(URL) as response:
      archive.write_bytes(response.read())
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SHA256:
      raise RuntimeError('Sparkle %s: sha256 %s, expected %s' %
                         (VERSION, digest, SHA256))
    extracted = Path(tmp) / 'x'
    extracted.mkdir()
    subprocess.run(['tar', '-xf', str(archive), '-C', str(extracted)],
                   check=True)
    if target.exists():
      shutil.rmtree(target)
    (target / 'sdk').mkdir(parents=True)
    (target / 'bundle').mkdir()
    # cp -R keeps the framework's symlinks.
    framework = extracted / 'Sparkle.framework'
    subprocess.run(['cp', '-R', str(framework), str(target / 'sdk')],
                   check=True)
    subprocess.run(['cp', '-R', str(framework), str(target / 'bundle')],
                   check=True)
    prune(target / 'bundle' / 'Sparkle.framework')
    subprocess.run(['cp', '-R', str(extracted / 'bin'), str(target)],
                   check=True)
    shutil.copy(extracted / 'LICENSE', target / 'LICENSE')
    _stamp(root).write_text(VERSION + '\n')
  return target
