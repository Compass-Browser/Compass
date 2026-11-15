# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Each release's debug symbols, kept on the machine that built it, so a
crash report from that release can be symbolicated
(`compass release symbolicate`, docs/design/updates.md, "Symbols").

The symbols folder (<release folder>/symbols, never published) has one
folder per version:

- <cpu>.tar.gz: the release build's dSYMs (macOS) or PDBs (Windows) of what
  ships, not of tests or build tools;
- uuids.txt (macOS): `dwarfdump --uuid` of those dSYMs, to match a crash
  report's binary images against.

Official builds already make the dSYMs (enable_dsyms) and PDBs; nothing else
in the out directory keeps them past the next build.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path
from typing import Dict, Iterable, List, Optional

from . import release
from .process import CommandError, run

SYMBOLS_DIR = release.RELEASE_DIR / 'symbols'
VERSION_RE = re.compile(r'^\d{8}\.\d{4}$')
# Versions kept; older ones are removed when a release adds its own, unless
# a channel still offers them.
KEEP = 40

# Windows: the binaries the installer ships (chrome/installer/mini_installer/
# chrome.release), whose <name>.pdb are kept.
WIN_SHIPPED = (
    'chrome.dll', 'chrome_elf.dll', 'chrome_proxy.exe', 'chrome_pwa_launcher.exe',
    'chrome_wer.dll', 'compass.exe', 'compass_installer.exe', 'dxcompiler.dll',
    'elevated_tracing_service.exe', 'elevation_service.exe',
    'eventlog_provider.dll', 'libEGL.dll', 'libGLESv2.dll', 'mini_installer.exe',
    'notification_helper.exe', 'setup.exe', 'vk_swiftshader.dll')

# The macOS binary a crash usually points into.
DEFAULT_BINARY = 'Compass Framework'
# atos only needs some load address: frame offsets are added to it.
LOAD_ADDRESS = 0x100000000


def select(symbol_names: Iterable[str], suffix: str,
           shipped: Iterable[str]) -> List[str]:
  """The symbol files (<binary><suffix>) of the binaries in |shipped|."""
  wanted = set(shipped)
  return sorted(n for n in symbol_names
                if n.endswith(suffix) and n[:-len(suffix)] in wanted)


def bundle_files(app: Path) -> List[str]:
  """The names of the files in an app bundle (its binaries among them)."""
  return [p.name for p in app.rglob('*') if p.is_file() or p.is_symlink()]


def version_dir(version: str, root: Path = SYMBOLS_DIR) -> Path:
  if not VERSION_RE.match(version):
    raise CommandError('not a release version: %s' % version)
  return root / version


def _tar(out_dir: Path, names: List[str], archive: Path) -> None:
  partial = archive.with_name(archive.name + '.partial')
  with tarfile.open(partial, 'w:gz', compresslevel=6) as tar:
    for name in names:
      tar.add(out_dir / name, arcname=name)
  partial.replace(archive)


def archive(version: str, out_dirs: Dict[str, Path], suffix: str,
            shipped_by_cpu: Dict[str, Iterable[str]],
            root: Path = SYMBOLS_DIR) -> List[Path]:
  """Packs each CPU's symbols into <root>/<version>/<cpu>.tar.gz."""
  dest = version_dir(version, root)
  dest.mkdir(parents=True, exist_ok=True)
  made = []
  for cpu, out_dir in sorted(out_dirs.items()):
    names = select((p.name for p in out_dir.iterdir()), suffix,
                   shipped_by_cpu[cpu])
    if not names:
      raise CommandError('no %s files in %s' % (suffix, out_dir))
    path = dest / ('%s.tar.gz' % cpu)
    _tar(out_dir, names, path)
    made.append(path)
  return made


def archive_mac(version: str, out_dirs: Dict[str, Path], product: str,
                root: Path = SYMBOLS_DIR) -> List[Path]:
  shipped = {cpu: bundle_files(d / (product + '.app'))
             for cpu, d in out_dirs.items()}
  made = archive(version, out_dirs, '.dSYM', shipped, root)
  uuids = []
  for cpu, out_dir in sorted(out_dirs.items()):
    for name in select((p.name for p in out_dir.iterdir()), '.dSYM',
                       shipped[cpu]):
      done = run(['dwarfdump', '--uuid', out_dir / name], out_dir,
                 check=False, capture=True, quiet=True)
      uuids += [l.replace(str(out_dir) + '/', '')
                for l in done.stdout.splitlines() if l.startswith('UUID:')]
  (version_dir(version, root) / 'uuids.txt').write_text(
      '\n'.join(uuids) + '\n')
  return made


def archive_win(version: str, out_dir: Path, cpu: str = 'x64',
                root: Path = SYMBOLS_DIR) -> List[Path]:
  return archive(version, {cpu: out_dir}, '.pdb', {cpu: WIN_SHIPPED}, root)


def to_prune(versions: Iterable[str], keep: int,
             protected: Iterable[str]) -> List[str]:
  """The versions beyond the newest |keep|, except |protected| ones."""
  ordered = sorted(versions, key=release.version_key, reverse=True)
  spared = set(protected)
  return [v for v in ordered[keep:] if v not in spared]


def prune(manifest: Dict, keep: int = KEEP,
          root: Path = SYMBOLS_DIR) -> List[str]:
  """Removes old versions' symbols; keeps those a channel still offers."""
  if not root.is_dir():
    return []
  versions = [p.name for p in root.iterdir()
              if p.is_dir() and VERSION_RE.match(p.name)]
  protected = []
  for channel in release.CHANNELS:
    for platform in ('mac', 'win'):
      found = release.latest(manifest, channel, platform)
      if found:
        protected.append(found['version'])
  removed = to_prune(versions, keep, protected)
  for version in removed:
    shutil.rmtree(root / version)
  return removed


def parse_offset(text: str) -> int:
  """A frame's offset into its binary: hex (0x...) or decimal."""
  try:
    return int(text, 16) if text.lower().startswith('0x') else int(text)
  except ValueError:
    raise CommandError('not an offset: %s' % text)


def atos_command(dwarf: Path, cpu: str, offsets: Iterable[int],
                 load: int = LOAD_ADDRESS) -> List[str]:
  return (['atos', '-arch', cpu, '-o', str(dwarf), '-l', hex(load), '-i'] +
          [hex(load + o) for o in offsets])


def dsym_dwarf(dsym: Path) -> Path:
  return dsym / 'Contents' / 'Resources' / 'DWARF' / dsym.name[:-len('.dSYM')]


def symbolicate(version: str, offsets: List[str], cpu: str = 'arm64',
                binary: str = DEFAULT_BINARY,
                root: Path = SYMBOLS_DIR) -> str:
  """Unpacks |binary|'s dSYM of |version| and has atos name the frames at
  |offsets| (from the crash report: address - the binary's load address)."""
  path = version_dir(version, root) / ('%s.tar.gz' % cpu)
  if not path.is_file():
    raise CommandError('no symbols for %s (%s): %s not found' %
                       (version, cpu, path))
  parsed = [parse_offset(o) for o in offsets]
  member = binary + '.dSYM'
  with tempfile.TemporaryDirectory() as tmp:
    with tarfile.open(path) as tar:
      names = [m for m in tar.getmembers()
               if m.name == member or m.name.startswith(member + '/')]
      if not names:
        raise CommandError('%s has no %s' % (path, member))
      safe = {'filter': 'data'} if hasattr(tarfile, 'data_filter') else {}
      tar.extractall(tmp, members=names, **safe)
    done = subprocess.run(atos_command(dsym_dwarf(Path(tmp) / member), cpu,
                                       parsed),
                          text=True, capture_output=True)
    if done.returncode != 0:
      raise CommandError('atos failed: %s' % done.stderr.strip())
    return done.stdout
