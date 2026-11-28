# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'translation'))

import update_bergamot as update  # noqa: E402


def write(path, text=''):
  os.makedirs(os.path.dirname(path), exist_ok=True)
  with open(path, 'w', encoding='utf-8') as f:
    f.write(text)


class UpdateBergamotTest(unittest.TestCase):

  def setUp(self):
    self.temp = tempfile.TemporaryDirectory()
    self.sources = os.path.join(self.temp.name, 'sources')
    self.root = os.path.join(self.temp.name, 'repo', 'third_party',
                             'bergamot')
    self.saved = (update.ROOT, update.COPIES)
    update.ROOT = self.root

  def tearDown(self):
    update.ROOT, update.COPIES = self.saved
    self.temp.cleanup()

  def exists(self, relative):
    return os.path.exists(os.path.join(self.root, relative))

  def test_copies_matching_files_only(self):
    for name in ['a.h', 'a.cpp', 'notes.txt', 'CMakeLists.txt', 'k.cu',
                 'sub/b.h', 'test/t.cpp', 'FooTest.cpp']:
      write(os.path.join(self.sources, 'p', 'dir', name))
    write(os.path.join(self.sources, 'p', 'LICENSE'), 'MIT')
    update.COPIES = [
        ('p/LICENSE', 'x/LICENSE', None, False),
        ('p/dir', 'x/flat', update.SOURCE_PATTERNS, False),
        ('p/dir', 'x/deep', update.SOURCE_PATTERNS, True),
    ]
    update.copy(self.sources)
    self.assertTrue(self.exists('x/LICENSE'))
    self.assertTrue(self.exists('x/flat/a.h'))
    self.assertTrue(self.exists('x/flat/a.cpp'))
    self.assertFalse(self.exists('x/flat/notes.txt'))
    self.assertFalse(self.exists('x/flat/CMakeLists.txt'))
    self.assertFalse(self.exists('x/flat/k.cu'))
    self.assertFalse(self.exists('x/flat/FooTest.cpp'))
    self.assertFalse(self.exists('x/flat/sub/b.h'))
    self.assertTrue(self.exists('x/deep/sub/b.h'))
    self.assertFalse(self.exists('x/deep/test/t.cpp'))

  def test_missing_source_stops(self):
    update.COPIES = [('nowhere', 'x', None, False)]
    with self.assertRaises(SystemExit):
      update.copy(self.sources)

  def test_clear_keeps_our_files(self):
    for name in ['README.md', 'BUILD.gn', 'patches/1.patch', 'LICENSE',
                 'marian/src/a.h']:
      write(os.path.join(self.root, name))
    update.clear()
    self.assertTrue(self.exists('README.md'))
    self.assertTrue(self.exists('BUILD.gn'))
    self.assertTrue(self.exists('patches/1.patch'))
    self.assertFalse(self.exists('LICENSE'))
    self.assertFalse(self.exists('marian'))

  def test_applies_patches_in_order(self):
    write(os.path.join(self.root, 'f.txt'), 'one\n')
    write(os.path.join(self.root, 'patches', '0001-two.patch'),
          '--- a/third_party/bergamot/f.txt\n'
          '+++ b/third_party/bergamot/f.txt\n'
          '@@ -1 +1 @@\n-one\n+two\n')
    write(os.path.join(self.root, 'patches', '0002-three.patch'),
          '--- a/third_party/bergamot/f.txt\n'
          '+++ b/third_party/bergamot/f.txt\n'
          '@@ -1 +1 @@\n-two\n+three\n')
    self.assertEqual(update.apply_patches(), 2)
    with open(os.path.join(self.root, 'f.txt'), encoding='utf-8') as f:
      self.assertEqual(f.read(), 'three\n')

  def test_vendored_tree_has_every_license(self):
    root = self.saved[0]
    if not os.path.isdir(root):
      self.skipTest('no vendored tree')
    for path in ['LICENSE', 'marian/LICENSE.md', 'ssplit/LICENSE.md',
                 'pcre2/LICENCE.md']:
      self.assertTrue(os.path.isfile(os.path.join(root, path)), path)
    third_party = os.path.join(root, 'marian', 'src', '3rd_party')
    for name in os.listdir(third_party):
      directory = os.path.join(third_party, name)
      if not os.path.isdir(directory) or name == 'half_float':
        continue  # half_float's license is in umHalf.h.
      self.assertTrue(os.path.isfile(os.path.join(directory, 'LICENSE')),
                      name)


if __name__ == '__main__':
  unittest.main()
