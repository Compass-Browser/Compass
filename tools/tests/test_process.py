# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from compass_tool import process  # noqa: E402


class RunTest(unittest.TestCase):

  def test_finds_program_only_on_child_path(self):
    with tempfile.TemporaryDirectory() as tmp:
      name = 'compass_fake_tool'
      if sys.platform == 'win32':
        (Path(tmp) / (name + '.bat')).write_text('@echo off\r\necho ok\r\n')
        cmd = name + '.bat'
      else:
        script = Path(tmp) / name
        script.write_text('#!/bin/sh\necho ok\n')
        script.chmod(0o755)
        cmd = name
      env = dict(os.environ)
      env['PATH'] = tmp + os.pathsep + env.get('PATH', '')
      result = process.run([cmd], Path(tmp), env=env, capture=True,
                           quiet=True)
      self.assertEqual(result.stdout.strip(), 'ok')


if __name__ == '__main__':
  unittest.main()
