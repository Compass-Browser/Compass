# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compass_tool import symbols  # noqa: E402
from compass_tool.cli import parser  # noqa: E402
from compass_tool.process import CommandError  # noqa: E402


def _release(version, channel):
  return {'version': version, 'channel': channel,
          'mac': {'file': 'Compass-%s.dmg' % version}}


class SelectTest(unittest.TestCase):

  def test_only_what_ships(self):
    names = ['Compass Framework.dSYM', 'Compass Helper (GPU).dSYM',
             'compass_unittests.dSYM', 'mksnapshot.dSYM',
             'libEGL.dylib.dSYM', 'Compass.app', 'args.gn']
    shipped = ['Compass Framework', 'Compass Helper (GPU)', 'libEGL.dylib',
               'Info.plist']
    self.assertEqual(symbols.select(names, '.dSYM', shipped),
                     ['Compass Framework.dSYM', 'Compass Helper (GPU).dSYM',
                      'libEGL.dylib.dSYM'])

  def test_windows_pdbs(self):
    names = ['chrome.dll.pdb', 'compass.exe.pdb', 'protoc.exe.pdb',
             'compass_unittests.exe.pdb', 'chrome.dll']
    self.assertEqual(symbols.select(names, '.pdb', symbols.WIN_SHIPPED),
                     ['chrome.dll.pdb', 'compass.exe.pdb'])


class ArchiveTest(unittest.TestCase):

  def test_packs_shipped_dsyms_per_cpu(self):
    with tempfile.TemporaryDirectory() as tmp:
      tmp = Path(tmp)
      out = tmp / 'out'
      app = out / 'Compass.app' / 'Contents' / 'Frameworks'
      app.mkdir(parents=True)
      (app / 'Compass Framework').write_text('binary')
      dwarf = symbols.dsym_dwarf(out / 'Compass Framework.dSYM')
      dwarf.parent.mkdir(parents=True)
      dwarf.write_text('dwarf')
      (out / 'mksnapshot.dSYM').mkdir()
      shipped = {'arm64': symbols.bundle_files(out / 'Compass.app')}
      made = symbols.archive('20261114.1035', {'arm64': out}, '.dSYM',
                             shipped, tmp / 'symbols')
      self.assertEqual(made,
                       [tmp / 'symbols' / '20261114.1035' / 'arm64.tar.gz'])
      with tarfile.open(made[0]) as tar:
        self.assertIn('Compass Framework.dSYM/Contents/Resources/DWARF/'
                      'Compass Framework', tar.getnames())
        self.assertFalse(any(n.startswith('mksnapshot') for n in
                             tar.getnames()))

  def test_nothing_to_keep_is_an_error(self):
    with tempfile.TemporaryDirectory() as tmp:
      with self.assertRaises(CommandError):
        symbols.archive('20261114.1035', {'x64': Path(tmp)}, '.pdb',
                        {'x64': symbols.WIN_SHIPPED}, Path(tmp) / 'symbols')

  def test_rejects_a_path_for_a_version(self):
    with self.assertRaises(CommandError):
      symbols.version_dir('../site')


class PruneTest(unittest.TestCase):

  def test_keeps_the_newest_and_what_a_channel_offers(self):
    versions = ['20261101.0100', '20261114.1035', '20261102.0100',
                '20261115.1657', '20261103.0100']
    self.assertEqual(
        symbols.to_prune(versions, 2, protected=['20261102.0100']),
        ['20261103.0100', '20261101.0100'])

  def test_prune_removes_folders(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = Path(tmp)
      for v in ('20261101.0100', '20261102.0100', '20261103.0100'):
        (root / v).mkdir()
      (root / 'notes').mkdir()
      manifest = {'releases': [_release('20261101.0100', 'stable'),
                               _release('20261103.0100', 'canary')]}
      self.assertEqual(symbols.prune(manifest, keep=1, root=root),
                       ['20261102.0100'])
      self.assertEqual(sorted(p.name for p in root.iterdir()),
                       ['20261101.0100', '20261103.0100', 'notes'])


class SymbolicateTest(unittest.TestCase):

  def test_offsets(self):
    self.assertEqual(symbols.parse_offset('0x65a6ca8'), 0x65a6ca8)
    self.assertEqual(symbols.parse_offset('100'), 100)
    with self.assertRaises(CommandError):
      symbols.parse_offset('frame')

  def test_atos_command(self):
    dwarf = symbols.dsym_dwarf(Path('/s/Compass Framework.dSYM'))
    self.assertEqual(dwarf, Path('/s/Compass Framework.dSYM/Contents/'
                                 'Resources/DWARF/Compass Framework'))
    self.assertEqual(
        symbols.atos_command(dwarf, 'arm64', [0x10, 0x65a6ca8], load=0x1000),
        ['atos', '-arch', 'arm64', '-o', str(dwarf), '-l', '0x1000', '-i',
         '0x1010', '0x65a7ca8'])

  def test_missing_version(self):
    with tempfile.TemporaryDirectory() as tmp:
      with self.assertRaises(CommandError):
        symbols.symbolicate('20261114.1035', ['0x10'], root=Path(tmp))

  def test_command_line(self):
    args = parser().parse_args(['release', 'symbolicate', '20261114.1035',
                                '0x65a6ca8', '0x65a7878', '--cpu', 'x64'])
    self.assertEqual(args.action, 'symbolicate')
    self.assertEqual(args.version, '20261114.1035')
    self.assertEqual(args.offsets, ['0x65a6ca8', '0x65a7878'])
    self.assertEqual(args.cpu, 'x64')
    self.assertEqual(args.binary, 'Compass Framework')


if __name__ == '__main__':
  unittest.main()
