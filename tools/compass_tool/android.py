# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Android builds from a Mac (docs/building.md, "Android from a Mac").

Chromium only supports Linux hosts for Android, but its CIPD packages come in
Mac flavors for nearly everything (the JDK, the SDK build tools, the NDK).
Two of them don't fit the build as is; `prepare_mac_host` fixes them up after
every gclient sync, which can put the originals back.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import List

# The --cpu value (layout.CROSS_TARGETS) and what it builds for.
# android-x64 is for the emulator on an x86_64 host, which can't run the
# arm64 build through its ARM translation.
TARGETS = {'android-arm64': 'arm64', 'android-x64': 'x64'}

ELF_MAGIC = b'\x7fELF'


def is_android(cpu: str | None) -> bool:
  return cpu in TARGETS


def _is_elf(path: Path) -> bool:
  with open(path, 'rb') as f:
    return f.read(4) == ELF_MAGIC


def link_jdk(src: Path) -> List[str]:
  """The Mac JDK is a bundle (current/Contents/Home/...); the build runs
  current/bin/java and lists current/lib/... as inputs. Links each entry of
  Contents/Home into current/. Returns what it linked."""
  current = src / 'third_party' / 'jdk' / 'current'
  home = current / 'Contents' / 'Home'
  linked = []
  if not home.is_dir():
    return linked
  for entry in sorted(home.iterdir()):
    link = current / entry.name
    if link.exists() or link.is_symlink():
      continue
    link.symlink_to(Path('Contents') / 'Home' / entry.name)
    linked.append(entry.name)
  return linked


def use_sdk_aapt2(src: Path) -> bool:
  """Chromium pins its own aapt2, Linux only; the SDK's build tools have a
  Mac one. Replaces the pinned one with a link to it. Returns whether it did."""
  pinned = src / 'third_party' / 'android_build_tools' / 'aapt2' / 'cipd' / 'aapt2'
  if not pinned.is_file() or pinned.is_symlink() or not _is_elf(pinned):
    return False
  build_tools = src / 'third_party' / 'android_sdk' / 'public' / 'build-tools'
  candidates = sorted(build_tools.glob('*/aapt2'))
  if not candidates:
    raise FileNotFoundError('no aapt2 in %s' % build_tools)
  pinned.rename(pinned.with_name('aapt2.linux'))
  pinned.symlink_to(os.path.relpath(candidates[-1], pinned.parent))
  return True


def prepare_mac_host(src: Path) -> None:
  """Fixes up the checkout's Android tools for a Mac host. Idempotent, and a
  no-op elsewhere."""
  if sys.platform != 'darwin':
    return
  for name in link_jdk(src):
    print('android: linked jdk/current/%s' % name)
  if use_sdk_aapt2(src):
    print('android: using the SDK\'s Mac aapt2')
