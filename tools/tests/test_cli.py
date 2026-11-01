# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compass_tool.cli import (browser_test_args,  # noqa: E402
                              parser, require_buildable_layout)
from compass_tool.layout import Layout  # noqa: E402
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

  def test_rejects_unknown_action(self):
    with self.assertRaises(SystemExit):
      parser().parse_args(['ios', 'deploy'])


if __name__ == '__main__':
  unittest.main()
