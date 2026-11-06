# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compass_tool.cli import (browser_test_args,  # noqa: E402
                              parser, require_buildable_layout)
from compass_tool.layout import Layout, host_cpu  # noqa: E402
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

  def test_host_build_has_no_suffix(self):
    out = Path('/w/compass-chromium/src/out')
    self.assertEqual(self.layout().out_dir('fast'), out / 'Fast')
    self.assertEqual(self.layout().out_dir('fast', host_cpu()), out / 'Fast')

  def test_cross_build_gets_its_own_dir(self):
    other = 'x64' if host_cpu() == 'arm64' else 'arm64'
    self.assertEqual(self.layout().out_dir('portable', other),
                     Path('/w/compass-chromium/src/out/Portable-' + other))

  def test_cpu_arguments(self):
    for argv in (['gen', 'portable'], ['build', '-c', 'portable'],
                 ['run', '-c', 'portable'], ['sign']):
      args = parser().parse_args(argv + ['--cpu', 'x64'])
      self.assertEqual(args.cpu, 'x64')


class IosDeviceTest(unittest.TestCase):

  def test_device_arguments(self):
    args = parser().parse_args(
        ['ios', 'device', '--api-key', 'k.p8', '--api-key-id', 'ID',
         '--api-issuer', 'ISS'])
    self.assertEqual(args.action, 'device')
    self.assertEqual(args.api_key_id, 'ID')


if __name__ == '__main__':
  unittest.main()
