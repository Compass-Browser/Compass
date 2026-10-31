# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compass_tool.cli import browser_test_args  # noqa: E402

KEEP_GPU = '--disable-features=MacDisclaimGpuTccResponsibility'


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


if __name__ == '__main__':
  unittest.main()
