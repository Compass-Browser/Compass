# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import update_strip_on_share as update  # noqa: E402


class CheckListTest(unittest.TestCase):

  def test_shipped_lists_pass(self):
    for ours in update.FILES.values():
      with open(os.path.join(update.ROOT, ours), encoding='utf-8') as f:
        update.check_list(ours, f.read())

  def test_refuses_bad_lists(self):
    bad = [
        'not json',
        '[]',
        '{}',
        json.dumps({'global': {'isGlobal': True}}),
        json.dumps({'global': {'isGlobal': True, 'queryParams': [1]}}),
        json.dumps({'x': {'queryParams': ['a'], 'hosts': ['a.com']}}),
        json.dumps({'global': {'isGlobal': True, 'queryParams': []},
                    'x': {'queryParams': ['a']}}),
        json.dumps({'global': {'isGlobal': True, 'queryParams': [],
                               'hosts': 'a.com'}}),
    ]
    for text in bad:
      with self.subTest(text=text):
        with self.assertRaises(ValueError):
          update.check_list('test', text)

  def test_accepts_site_rules(self):
    update.check_list('test', json.dumps({
        'global': {'isGlobal': True, 'queryParams': ['utm_source']},
        'a': {'queryParams': ['si'], 'schemelessSites': ['spotify.com']},
        'b': {'queryParams': ['x'], 'hosts': ['www.a.com']},
    }))


class ReadmeTest(unittest.TestCase):

  def test_shipped_readme_has_the_line(self):
    with open(os.path.join(update.ROOT, 'README.md'), encoding='utf-8') as f:
      readme = f.read()
    updated = update.with_commit(readme, 'a' * 40, '2027-01-02')
    self.assertIn('commit\n`%s` (2027-01-02)' % ('a' * 40), updated)
    self.assertEqual(len(updated), len(readme))

  def test_refuses_a_readme_without_the_line(self):
    with self.assertRaises(ValueError):
      update.with_commit('nothing here', 'a' * 40, '2027-01-02')


if __name__ == '__main__':
  unittest.main()
