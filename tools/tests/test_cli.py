# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compass_tool.cli import (args_gn, browser_test_args,  # noqa: E402
                              parser, require_buildable_layout,
                              require_in_tree_init)
from compass_tool.layout import (Layout, detect, host_cpu,  # noqa: E402
                                 vs_toolchain_hash)
from compass_tool.process import CommandError  # noqa: E402

KEEP_GPU = '--disable-features=MacDisclaimGpuTccResponsibility'


class BuildableLayoutTest(unittest.TestCase):

  def layout(self, in_tree):
    return Layout(Path('/w/compass'), Path('/w/compass-chromium'), in_tree)

  def test_windows_rejects_linked_layout(self):
    with self.assertRaises(CommandError) as raised:
      require_buildable_layout(self.layout(False), 'win32')
    self.assertIn('in-tree', str(raised.exception))

  def test_windows_accepts_in_tree_layout(self):
    require_buildable_layout(self.layout(True), 'win32')

  def test_other_platforms_accept_linked_layout(self):
    require_buildable_layout(self.layout(False), 'darwin')
    require_buildable_layout(self.layout(False), 'linux')

  def test_windows_init_rejects_linked_layout(self):
    with self.assertRaises(CommandError) as raised:
      require_in_tree_init(self.layout(False), 'win32')
    self.assertIn('git clone', str(raised.exception))

  def test_windows_init_accepts_in_tree_layout(self):
    require_in_tree_init(self.layout(True), 'win32')

  def test_other_platforms_init_linked_layout(self):
    require_in_tree_init(self.layout(False), 'darwin')


class DetectTest(unittest.TestCase):

  def test_in_tree_clone_before_init(self):
    layout = detect(Path('/w/compass-chromium/src/compass'), {})
    self.assertTrue(layout.in_tree)
    self.assertEqual(layout.chromium_dir, Path('/w/compass-chromium'))

  def test_plain_src_directory_is_linked(self):
    # ~/src/compass next to ~/src/compass-chromium is the linked layout.
    layout = detect(Path('/home/me/src/compass'), {})
    self.assertFalse(layout.in_tree)
    self.assertEqual(layout.chromium_dir, Path('/home/me/src/compass-chromium'))


class BrowserTestArgsTest(unittest.TestCase):

  def test_headless_by_default(self):
    self.assertEqual(browser_test_args(['--gtest_filter=A.*'], False, 'linux'),
                     ['--headless', '--gtest_filter=A.*'])
    self.assertEqual(browser_test_args([], True, 'linux'), [])

  def test_mac_gpu_process_keeps_the_terminals_access(self):
    self.assertEqual(browser_test_args(['--gtest_filter=A.*'], True, 'darwin'),
                     [KEEP_GPU, '--gtest_filter=A.*'])

  def test_mac_merges_disabled_features(self):
    self.assertEqual(
        browser_test_args(['--disable-features=Foo,Bar', '-x'], False,
                          'darwin'),
        ['--headless', KEEP_GPU + ',Foo,Bar', '-x'])


class IosParserTest(unittest.TestCase):

  def test_sim_arguments(self):
    args = parser().parse_args(
        ['ios', 'sim', '--device', 'iPhone X', '--screenshot', '/tmp/a.png',
         '--app-arg=-ResetState'])
    self.assertEqual(args.action, 'sim')
    self.assertEqual(args.device, 'iPhone X')
    self.assertEqual(args.screenshot, '/tmp/a.png')
    self.assertEqual(args.app_args, ['-ResetState'])

  def test_archive_arguments(self):
    args = parser().parse_args(
        ['ios', 'archive', '--upload', '--api-key', 'k.p8', '--api-key-id',
         'ABC', '--api-issuer', 'uuid'])
    self.assertEqual(args.action, 'archive')
    self.assertTrue(args.upload)
    self.assertEqual((args.api_key, args.api_key_id, args.api_issuer),
                     ('k.p8', 'ABC', 'uuid'))

  def test_rejects_unknown_action(self):
    with self.assertRaises(SystemExit):
      parser().parse_args(['ios', 'deploy'])


class OutDirTest(unittest.TestCase):

  def layout(self):
    return Layout(Path('/w/compass'), Path('/w/compass-chromium'), False)

  def test_host_cpu_uses_gn_names(self):
    self.assertEqual(host_cpu('x86_64'), 'x64')
    self.assertEqual(host_cpu('arm64'), 'arm64')
    self.assertEqual(host_cpu('AMD64'), 'x64')

  def test_cpu_arguments(self):
    for argv in (['gen', 'portable'], ['build', '-c', 'portable'],
                 ['run', '-c', 'portable'], ['sign'], ['release', 'publish']):
      args = parser().parse_args(argv + ['--cpu', 'x64'])
      self.assertEqual(args.cpu, 'x64')

  def test_out_dir_without_cpu_is_unchanged(self):
    self.assertEqual(self.layout().out_dir('release'),
                     Path('/w/compass-chromium/src/out/Release'))

  def test_out_dir_per_cpu(self):
    self.assertEqual(self.layout().out_dir('release', 'x64'),
                     Path('/w/compass-chromium/src/out/Release-x64'))

  def test_out_dir_for_windows_from_a_mac(self):
    self.assertEqual(self.layout().out_dir('release', 'win-x64'),
                     Path('/w/compass-chromium/src/out/Release-win-x64'))
    self.assertEqual('win-x64', parser().parse_args(
        ['release', 'build-win', '--cpu', 'win-x64']).cpu)

  def test_out_dir_for_android(self):
    self.assertEqual(self.layout().out_dir('portable', 'android-arm64'),
                     Path('/w/compass-chromium/src/out/Portable-android-arm64'))
    self.assertEqual('android-arm64', parser().parse_args(
        ['gen', 'portable', '--cpu', 'android-arm64']).cpu)
    self.assertTrue(parser().parse_args(
        ['gen', 'portable', '--cpu', 'android-arm64',
         '--skip-patches']).skip_patches)

  def test_build_commands_take_cpu(self):
    for argv in (['gen', 'release', '--cpu', 'x64'],
                 ['build', '-c', 'release', '--cpu', 'x64'],
                 ['run', '-c', 'release', '--cpu', 'x64'],
                 ['test', '--cpu', 'x64']):
      self.assertEqual(parser().parse_args(argv).cpu, 'x64', argv)

  def test_rejects_unknown_cpu(self):
    with self.assertRaises(SystemExit):
      parser().parse_args(['gen', '--cpu', 'riscv'])

  def test_sign_and_release_take_universal(self):
    self.assertTrue(parser().parse_args(['sign', '--universal']).universal)
    self.assertTrue(parser().parse_args(['release', '--universal']).universal)
    self.assertFalse(parser().parse_args(['sign']).universal)

  def test_universal_defaults_to_release(self):
    args = parser().parse_args(['universal', '--build', '-j', '6'])
    self.assertEqual((args.config, args.build, args.jobs), ('release', True, 6))


class IosDeviceTest(unittest.TestCase):

  def test_device_arguments(self):
    args = parser().parse_args(
        ['ios', 'device', '--api-key', 'k.p8', '--api-key-id', 'ID',
         '--api-issuer', 'ISS'])
    self.assertEqual(args.action, 'device')
    self.assertEqual(args.api_key_id, 'ID')


class IosXcconfigTest(unittest.TestCase):

  def test_local_xcconfig_only_when_present(self):
    import tempfile
    from compass_tool.cli import ios_xcconfig_args
    with tempfile.TemporaryDirectory() as d:
      ios = Path(d)
      self.assertEqual([], ios_xcconfig_args(ios))
      (ios / 'Local.xcconfig').write_text('COMPASS_SYNC_SERVER =\n')
      self.assertEqual(['-xcconfig', str(ios / 'Local.xcconfig')],
                       ios_xcconfig_args(ios))


if __name__ == '__main__':
  unittest.main()


class WinCrossTest(unittest.TestCase):

  def make(self, tmp):
    chromium = Path(tmp) / 'compass-chromium'
    (chromium / 'src' / 'build').mkdir(parents=True)
    (chromium / 'src' / 'build' / 'vs_toolchain.py').write_text(
        "SDK = 1\nTOOLCHAIN_HASH = '3bfcb536c8'\nSDK_VERSION = '10.0.28000.0'\n")
    return Layout(Path(tmp) / 'compass', chromium, False)

  def test_toolchain_env(self):
    with tempfile.TemporaryDirectory() as tmp:
      layout = self.make(tmp)
      self.assertEqual('3bfcb536c8', vs_toolchain_hash(layout.src))
      self.assertEqual({}, layout.win_toolchain_env())
      layout.win_toolchain_dir.mkdir()
      (layout.win_toolchain_dir / '6f8bd28bfd.zip').write_bytes(b'')
      self.assertEqual({
          'DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL': str(layout.win_toolchain_dir),
          'GYP_MSVS_HASH_3bfcb536c8': '6f8bd28bfd',
      }, layout.win_toolchain_env())
      if sys.platform != 'win32':
        self.assertEqual('6f8bd28bfd',
                         layout.env()['GYP_MSVS_HASH_3bfcb536c8'])

  def test_args_gn(self):
    with tempfile.TemporaryDirectory() as tmp:
      text = args_gn(self.make(tmp), 'release', 'win-x64')
      self.assertIn('target_os = "win"\n', text)
      self.assertIn('target_cpu = "x64"\n', text)
      self.assertIn('import("//compass/config/args/release.gn")', text)
      self.assertNotIn('target_os', args_gn(self.make(tmp + '/b'), 'release',
                                             'x64'))

  def test_args_gn_for_android(self):
    with tempfile.TemporaryDirectory() as tmp:
      text = args_gn(self.make(tmp), 'portable', 'android-arm64')
      self.assertIn('target_os = "android"\n', text)
      self.assertIn('target_cpu = "arm64"\n', text)
      self.assertIn('import("//compass/config/args/portable.gn")', text)
      self.assertTrue(text.rstrip().endswith(
          'import("//compass/config/args/android.gn")'))
