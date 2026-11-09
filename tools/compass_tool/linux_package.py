# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Linux packages of a `--cpu linux-x64` build: a .tar.xz, and a Flatpak
bundle made from it (docs/building.md, "Linux packages").

The tarball is written on the Windows host, so Unix modes come from here, not
from the out dir's files (NTFS has no exec bits). The Flatpak is built in WSL
with flatpak-builder from installer/linux/flatpak/.
"""

from __future__ import annotations

import io
import shutil
import tarfile
import tempfile
from pathlib import Path, PurePath, PurePosixPath
from typing import Iterator, List, Tuple

from .process import CommandError, run

APP_ID = 'io.github.Compass_Browser.Compass'
ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)
# Executables in the out dir, by what they're called in the package.
EXECUTABLES = {
    'chrome': 'chrome',
    'chrome_crashpad_handler': 'chrome_crashpad_handler',
    'chrome-sandbox': 'chrome_sandbox',
}
LIBRARIES = ('libEGL.so', 'libGLESv2.so', 'libvk_swiftshader.so',
             'libvulkan.so.1')
DATA_FILES = ('chrome_100_percent.pak', 'chrome_200_percent.pak',
              'headless_command_resources.pak', 'resources.pak',
              'icudtl.dat', 'snapshot_blob.bin', 'v8_context_snapshot.bin',
              'vk_swiftshader_icd.json')
DATA_DIRS = ('locales', 'MEIPreload')
# setuid root, so the sandbox works where user namespaces are restricted
# (Ubuntu 24.04's AppArmor). Unpacking as root keeps it; docs say how
# otherwise.
SANDBOX_MODE = 0o4755


def package_name(version: str, ext: str) -> str:
  return 'Compass-%s-linux-x64.%s' % (version, ext)


def installer_dir(repo_root: Path) -> Path:
  return repo_root / 'installer' / 'linux'


def icon_files(src: Path) -> List[Tuple[int, Path]]:
  """Compass's logos, which the branding patches put over Chromium's."""
  theme = src / 'chrome' / 'app' / 'theme' / 'chromium'
  icons = []
  for size in ICON_SIZES:
    path = theme / 'linux' / ('product_logo_%d.png' % size)
    if not path.is_file():
      path = theme / 'default_100_percent' / 'chromium' / 'linux' / (
          'product_logo_%d.png' % size)
    if path.is_file():
      icons.append((size, path))
  return icons


def build_files(out_dir: Path) -> Iterator[Tuple[str, Path]]:
  """(name in the package, file in the out dir) for everything the browser
  needs at run time. Missing required files raise; optional ones (the
  context snapshot, the sandbox before it's built) are left out."""
  required = {'chrome', 'chrome_crashpad_handler', 'resources.pak',
              'icudtl.dat', 'locales'}
  for name, built in EXECUTABLES.items():
    yield from _present(out_dir, name, built, name in required)
  for name in LIBRARIES + DATA_FILES:
    yield from _present(out_dir, name, name, name in required)
  for name in DATA_DIRS:
    root = out_dir / name
    if not root.is_dir():
      if name in required:
        raise CommandError('%s is missing; build `chrome` first' % root)
      continue
    for path in sorted(root.rglob('*')):
      if path.is_file():
        yield (PurePosixPath(name, *path.relative_to(root).parts).as_posix(),
               path)


def _present(out_dir: Path, name: str, built: str,
             required: bool) -> Iterator[Tuple[str, Path]]:
  path = out_dir / built
  if path.is_file():
    yield name, path
  elif required:
    raise CommandError('%s is missing; build `chrome` first' % path)


def mode_for(name: str) -> int:
  if name == 'chrome-sandbox':
    return SANDBOX_MODE
  if name in EXECUTABLES or name in ('compass',):
    return 0o755
  return 0o644


def strippable(name: str) -> bool:
  return name in EXECUTABLES or name.endswith('.so') or '.so.' in name


def strip(tool: Path, files: List[Tuple[str, Path]],
          work: Path) -> List[Tuple[str, Path]]:
  """Copies of the ELF files without symbols (chrome halves), in |work|."""
  result = []
  for name, path in files:
    if not strippable(name):
      result.append((name, path))
      continue
    stripped = work / name
    stripped.parent.mkdir(parents=True, exist_ok=True)
    flags = [] if name in EXECUTABLES else ['--strip-unneeded']
    run([tool] + flags + ['-o', stripped, path], work, quiet=True)
    result.append((name, stripped))
  return result


def desktop_entry(repo_root: Path, command: str, icon: str) -> str:
  template = (installer_dir(repo_root) / 'compass.desktop.in').read_text()
  return (template.replace('@EXEC@', command + ' %U')
          .replace('@EXEC_BARE@', command).replace('@ICON@', icon))


def extra_files(repo_root: Path, src: Path) -> List[Tuple[str, bytes]]:
  """The launcher, desktop file and icons that go next to the build."""
  linux = installer_dir(repo_root)
  files = [('compass', (linux / 'compass').read_bytes()),
           ('compass.desktop',
            desktop_entry(repo_root, 'compass', 'compass').encode()),
           ('README', (linux / 'README').read_bytes())]
  for size, path in icon_files(src):
    files.append(('product_logo_%d.png' % size, path.read_bytes()))
  return files


def _info(arcname: str, size: int, mode: int) -> tarfile.TarInfo:
  info = tarfile.TarInfo(arcname)
  info.size = size
  info.mode = mode
  info.uid = info.gid = 0
  info.uname = info.gname = 'root'
  info.mtime = 0
  return info


def write_tarball(tarball: Path, top: str, files: List[Tuple[str, Path]],
                  extras: List[Tuple[str, bytes]]) -> Path:
  """|files| and |extras| under |top|/, with Unix modes set here."""
  partial = tarball.with_name(tarball.name + '.partial')
  with tarfile.open(partial, 'w:xz', preset=6) as tar:
    top_dir = _info(top, 0, 0o755)
    top_dir.type = tarfile.DIRTYPE
    tar.addfile(top_dir)
    dirs = set()
    entries = sorted(
        [(name, path, None) for name, path in files] +
        [(name, None, data) for name, data in extras],
        key=lambda e: e[0])
    for name, path, data in entries:
      for parent in reversed(list(PurePosixPath(name).parents)[:-1]):
        if parent not in dirs:
          dirs.add(parent)
          d = _info('%s/%s' % (top, parent), 0, 0o755)
          d.type = tarfile.DIRTYPE
          tar.addfile(d)
      arcname = '%s/%s' % (top, name)
      if data is None:
        with path.open('rb') as f:
          tar.addfile(_info(arcname, path.stat().st_size, mode_for(name)), f)
      else:
        tar.addfile(_info(arcname, len(data), mode_for(name)),
                    io.BytesIO(data))
  partial.replace(tarball)
  return tarball


def make_tarball(repo_root: Path, src: Path, out_dir: Path, version: str,
                 strip_tool: Path | None) -> Path:
  """The release .tar.xz, which the Flatpak is also made from."""
  files = list(build_files(out_dir))
  extras = extra_files(repo_root, src)
  top = 'Compass-%s-linux-x64' % version
  with tempfile.TemporaryDirectory(dir=out_dir) as work:
    if strip_tool:
      files = strip(strip_tool, files, Path(work))
    return write_tarball(out_dir / package_name(version, 'tar.xz'), top, files,
                         extras)


def stage_flatpak(repo_root: Path, tarball: Path, version: str,
                  stage: Path) -> Path:
  """A folder flatpak-builder can run in: manifest, tarball, metadata."""
  if stage.exists():
    shutil.rmtree(stage)
  stage.mkdir(parents=True)
  flatpak = installer_dir(repo_root) / 'flatpak'
  shutil.copyfile(tarball, stage / 'compass.tar.xz')
  for name in ('compass.sh', APP_ID + '.yml'):
    shutil.copyfile(flatpak / name, stage / name)
  metainfo = (flatpak / (APP_ID + '.metainfo.xml')).read_text()
  (stage / (APP_ID + '.metainfo.xml')).write_bytes(
      metainfo.replace('@VERSION@', version).encode())
  (stage / (APP_ID + '.desktop')).write_bytes(
      desktop_entry(repo_root, 'compass', APP_ID).encode())
  return stage


def wsl_path(path: PurePath) -> str:
  """C:\\src\\x (absolute) → /mnt/c/src/x."""
  drive = path.drive.rstrip(':').lower()
  rest = PurePosixPath(*path.parts[1:]).as_posix()
  return '/mnt/%s/%s' % (drive, rest)


def flatpak_script(stage: str, bundle: str) -> str:
  """Runs in WSL, as root (docs/building.md, "Linux packages")."""
  return '\n'.join([
      'set -e',
      # flatpak-builder wants a Linux filesystem for its state and build dirs.
      'work=/var/tmp/compass-flatpak',
      'rm -rf "$work"/build',
      'mkdir -p "$work"',
      'cp -r "%s"/. "$work"/' % stage,
      'cd "$work"',
      'flatpak-builder --force-clean --install-deps-from=flathub '
      '--repo=repo build %s.yml' % APP_ID,
      'flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/'
      'flathub.flatpakrepo repo "%s" %s' % (bundle, APP_ID),
  ]) + '\n'


def build_flatpak(stage: Path, bundle: Path, distro: str) -> Path:
  wsl = shutil.which('wsl') or r'C:\Program Files\WSL\wsl.exe'
  script = stage / 'build.sh'
  script.write_bytes(
      flatpak_script(wsl_path(stage), wsl_path(bundle)).encode())
  run([wsl, '-d', distro, '-u', 'root', '--', 'bash', wsl_path(script)],
      stage)
  if not bundle.is_file():
    raise CommandError('flatpak-builder did not write %s' % bundle)
  return bundle
