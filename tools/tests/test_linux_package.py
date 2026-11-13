# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys
import tarfile
import tempfile
import unittest
from pathlib import Path, PureWindowsPath

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compass_tool import linux_package  # noqa: E402
from compass_tool.cli import parser  # noqa: E402
from compass_tool.layout import REPO_ROOT  # noqa: E402
from compass_tool.process import CommandError  # noqa: E402

REQUIRED = ('chrome', 'chrome_crashpad_handler', 'resources.pak',
            'icudtl.dat', 'locales/en-US.pak')


def make_out_dir(root: Path, names=REQUIRED) -> Path:
  out = root / 'out'
  for name in names:
    path = out / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(name.encode())
  return out


class BuildFilesTest(unittest.TestCase):

  def test_lists_present_files_and_renames_the_sandbox(self):
    with tempfile.TemporaryDirectory() as tmp:
      out = make_out_dir(Path(tmp), REQUIRED + ('chrome_sandbox', 'libEGL.so',
                                                'MEIPreload/manifest.json',
                                                'obj/x.o', 'args.gn'))
      names = [name for name, _ in linux_package.build_files(out)]
    self.assertIn('chrome-sandbox', names)
    self.assertIn('libEGL.so', names)
    self.assertIn('locales/en-US.pak', names)
    self.assertIn('MEIPreload/manifest.json', names)
    self.assertNotIn('chrome_sandbox', names)
    self.assertNotIn('args.gn', names)
    self.assertFalse(any(name.startswith('obj') for name in names))

  def test_missing_chrome_is_an_error(self):
    with tempfile.TemporaryDirectory() as tmp:
      out = make_out_dir(Path(tmp), REQUIRED[1:])
      with self.assertRaises(CommandError):
        list(linux_package.build_files(out))

  def test_optional_files_may_be_missing(self):
    with tempfile.TemporaryDirectory() as tmp:
      names = [n for n, _ in linux_package.build_files(
          make_out_dir(Path(tmp)))]
    self.assertNotIn('chrome-sandbox', names)
    self.assertNotIn('v8_context_snapshot.bin', names)


class IconsTest(unittest.TestCase):

  def test_finds_both_icon_folders(self):
    with tempfile.TemporaryDirectory() as tmp:
      theme = Path(tmp) / 'chrome' / 'app' / 'theme'
      for rel in ('chromium/linux/product_logo_256.png',
                  'default_100_percent/chromium/linux/product_logo_16.png'):
        (theme / rel).parent.mkdir(parents=True, exist_ok=True)
        (theme / rel).write_bytes(b'png')
      sizes = [size for size, _ in linux_package.icon_files(Path(tmp))]
    self.assertEqual(sizes, [16, 256])


class ModesTest(unittest.TestCase):

  def test_modes(self):
    self.assertEqual(linux_package.mode_for('chrome'), 0o755)
    self.assertEqual(linux_package.mode_for('compass'), 0o755)
    self.assertEqual(linux_package.mode_for('chrome-sandbox'), 0o4755)
    self.assertEqual(linux_package.mode_for('resources.pak'), 0o644)
    self.assertEqual(linux_package.mode_for('libEGL.so'), 0o644)

  def test_strippable(self):
    self.assertTrue(linux_package.strippable('chrome'))
    self.assertTrue(linux_package.strippable('libvulkan.so.1'))
    self.assertFalse(linux_package.strippable('resources.pak'))


class TarballTest(unittest.TestCase):

  def test_tarball_has_modes_owner_and_extras(self):
    with tempfile.TemporaryDirectory() as tmp:
      out = make_out_dir(Path(tmp), REQUIRED + ('chrome_sandbox',))
      tarball = linux_package.make_tarball(REPO_ROOT, Path(tmp) / 'src', out,
                                           '20261109.2000', None)
      self.assertEqual(tarball.name, 'Compass-20261109.2000-linux-x64.tar.xz')
      with tarfile.open(tarball) as tar:
        members = {m.name: m for m in tar.getmembers()}
        desktop = tar.extractfile(
            'Compass-20261109.2000-linux-x64/compass.desktop').read().decode()
    top = 'Compass-20261109.2000-linux-x64/'
    self.assertEqual(members[top + 'chrome'].mode, 0o755)
    self.assertEqual(members[top + 'compass'].mode, 0o755)
    self.assertEqual(members[top + 'chrome-sandbox'].mode, 0o4755)
    self.assertEqual(members[top + 'chrome-sandbox'].uid, 0)
    self.assertEqual(members[top + 'resources.pak'].mode, 0o644)
    self.assertGreater(members[top + 'chrome'].mtime, 0)
    self.assertTrue(members[top + 'locales'].isdir())
    self.assertTrue(members[top.rstrip('/')].isdir())
    self.assertIn(top + 'README', members)
    self.assertIn('Exec=compass %U\n', desktop)
    self.assertIn('Exec=compass --incognito\n', desktop)
    self.assertNotIn('@', desktop)


class FlatpakTest(unittest.TestCase):

  def test_manifest_and_stage(self):
    with tempfile.TemporaryDirectory() as tmp:
      tarball = Path(tmp) / 'c.tar.xz'
      tarball.write_bytes(b'x')
      stage = linux_package.stage_flatpak(REPO_ROOT, tarball, '1.2',
                                          Path(tmp) / 'flatpak')
      names = sorted(p.name for p in stage.iterdir())
      metainfo = (stage / (linux_package.APP_ID + '.metainfo.xml')).read_text()
      manifest = (stage / (linux_package.APP_ID + '.yml')).read_text()
      desktop = (stage / (linux_package.APP_ID + '.desktop')).read_text()
    self.assertEqual(names, [
        'compass.sh', 'compass.tar.xz', linux_package.APP_ID + '.desktop',
        linux_package.APP_ID + '.metainfo.xml', linux_package.APP_ID + '.yml'
    ])
    self.assertIn('app-id: ' + linux_package.APP_ID, manifest)
    self.assertIn('rm -f /app/compass/chrome-sandbox', manifest)
    self.assertIn('Icon=' + linux_package.APP_ID + '\n', desktop)
    self.assertRegex(metainfo,
                     r'<release version="1.2" date="\d{4}-\d\d-\d\d"/>')
    self.assertIn('name: zypak', manifest)

  def test_wsl_path(self):
    self.assertEqual(
        linux_package.wsl_path(PureWindowsPath(r'C:\src\out\flatpak')),
        '/mnt/c/src/out/flatpak')

  def test_script_runs_builder_and_bundle(self):
    script = linux_package.flatpak_script(
        '/mnt/c/s', '/mnt/c/o/C.flatpak', '/mnt/c/o/C.oci', 'canary',
        'oci+https://example.test/')
    self.assertIn('flatpak-builder', script)
    self.assertIn('--default-branch=canary', script)
    # The bundle updates from the remote once installed.
    self.assertIn('--repo-url=oci+https://example.test/ repo '
                  '"/mnt/c/o/C.flatpak" %s canary' % linux_package.APP_ID,
                  script)
    self.assertIn('build-bundle --oci repo oci %s canary' %
                  linux_package.APP_ID, script)
    self.assertIn('cp -r oci "/mnt/c/o/C.oci"', script)


class PackageArgsTest(unittest.TestCase):

  def test_parses(self):
    args = parser().parse_args(
        ['package', '--cpu', 'linux-x64', '--flatpak', '--build'])
    self.assertEqual(args.config, 'portable')
    self.assertTrue(args.flatpak)
    self.assertTrue(args.build)
    self.assertEqual(args.wsl_distro, 'Ubuntu-24.04')
    self.assertEqual(args.channel, 'canary')


if __name__ == '__main__':
  unittest.main()
