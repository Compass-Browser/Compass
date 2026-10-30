# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('redirect_cc',
                                              ROOT / 'build' / 'redirect_cc.py')
redirect_cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(redirect_cc)


class RewriteCommandTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.TemporaryDirectory()
    self.src = Path(self.tmp.name) / 'src'
    self.out = self.src / 'out' / 'Debug'
    self.out.mkdir(parents=True)
    (self.src / 'chrome').mkdir()
    (self.src / 'chrome' / 'a.cc').write_text('')
    (self.src / 'chrome' / 'b.cc').write_text('')
    overrides = self.src / 'compass' / 'chromium_src' / 'chrome'
    overrides.mkdir(parents=True)
    (overrides / 'a.cc').write_text('')

  def tearDown(self):
    self.tmp.cleanup()

  def rewrite(self, command):
    return redirect_cc.rewrite_command(command, str(self.out))

  def test_compiles_override_instead_of_upstream(self):
    result = self.rewrite(['clang++', '-DX', '-c', '../../chrome/a.cc',
                           '-o', 'obj/a.o'])
    self.assertEqual(result[0], 'clang++')
    self.assertIn('../../compass/chromium_src/chrome/a.cc', result)
    self.assertNotIn('../../chrome/a.cc', result)

  def test_keeps_upstream_without_override(self):
    result = self.rewrite(['clang++', '-c', '../../chrome/b.cc'])
    self.assertIn('../../chrome/b.cc', result)

  def test_overrides_come_first_on_include_path(self):
    result = self.rewrite(['clang++', '-I../..', '-c', '../../chrome/b.cc'])
    self.assertEqual(result[1], '-I../../compass/chromium_src')
    self.assertLess(result.index('-I../../compass/chromium_src'),
                    result.index('-I../..'))

  def test_checkout_root_comes_last_on_include_path(self):
    # V8 includes its own headers as "src/base/..." via -I../../v8. The
    # checkout root must not shadow that with Chromium's src/base.
    result = self.rewrite(['clang++', '-I../../v8', '-c', '../../chrome/b.cc'])
    self.assertEqual(result[-1], '-I../../..')
    self.assertLess(result.index('-I../../v8'), result.index('-I../../..'))

  def test_uses_msvc_style_include_for_clang_cl(self):
    result = self.rewrite(['C:/llvm/bin/clang-cl.exe', '/c',
                           '../../chrome/a.cc'])
    self.assertEqual(result[1], '/I../../compass/chromium_src')
    self.assertEqual(result[-1], '/I../../..')
    self.assertIn('../../compass/chromium_src/chrome/a.cc', result)

  def test_leaves_non_compile_commands_alone(self):
    command = ['clang++', '-shared', '-o', 'libx.so', 'obj/a.o']
    self.assertEqual(self.rewrite(command), command)

  def test_ignores_sources_outside_src(self):
    outside = os.path.relpath(Path(self.tmp.name) / 'gen.cc', self.out)
    result = self.rewrite(['clang++', '-c', outside])
    self.assertIn(outside, result)


if __name__ == '__main__':
  unittest.main()
