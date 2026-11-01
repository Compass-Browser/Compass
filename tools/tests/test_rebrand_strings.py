# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'rebrand_strings', ROOT / 'strings' / 'rebrand_strings.py')
rebrand_strings = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rebrand_strings)
rename = rebrand_strings.rename


class RenameTest(unittest.TestCase):

  def test_renames_the_product(self):
    self.assertEqual(rename('Relaunch Chromium?'), 'Relaunch Compass?')
    self.assertEqual(rename("Chromium's settings"), "Compass's settings")

  def test_other_scripts_count_as_word_boundaries(self):
    self.assertEqual(rename('Chromium을 다시 시작'), 'Compass을 다시 시작')
    self.assertEqual(rename('Chromium を再起動'), 'Compass を再起動')

  def test_keeps_longer_words(self):
    self.assertEqual(rename('ChromiumOS'), 'ChromiumOS')
    self.assertEqual(rename('Chromiums'), 'Chromiums')

  def test_keeps_the_project(self):
    self.assertEqual(
        rename('Chrome is made possible by the Chromium open source project'),
        'Chrome is made possible by the Chromium open source project')
    self.assertEqual(rename('The Chromium Projects'), 'The Chromium Projects')
    self.assertTrue(
        rebrand_strings.names_project('the Chromium open-source project'))

  def test_keeps_the_project_link_in_grd_and_xtb(self):
    grd = ('Chromium is made possible by the <ph name="BEGIN_LINK_CHROMIUM">'
           '&lt;a href="$1"&gt;</ph>Chromium<ph name="END_LINK_CHROMIUM">'
           '&lt;/a&gt;</ph> open source project.')
    self.assertEqual(
        rename(grd),
        grd.replace('Chromium is made', 'Compass is made', 1))
    self.assertFalse(rebrand_strings.names_project(grd))
    xtb = ('<ph name="BEGIN_LINK_CHROMIUM"/>Chromium<ph name="END_LINK_CHROMIUM"/>'
           ' rende possibile Chromium')
    self.assertEqual(rename(xtb), xtb[:-len('Chromium')] + 'Compass')

  def test_keeps_examples(self):
    self.assertEqual(rename('$1<ex>About Chromium</ex> Chromium'),
                     '$1<ex>About Chromium</ex> Compass')

  def test_replaces_the_placeholder_profile_name(self):
    source = ('<message name="IDS_PROFILE_MENU_PLACEHOLDER_PROFILE_NAME" '
              'desc="Placeholder profile name.">\n        Your Chromium\n'
              '      </message>\n'
              '<message name="IDS_OTHER">\n        Your Chromium\n'
              '      </message>')
    self.assertEqual(
        rebrand_strings.rename_messages(source),
        source.replace('Your Chromium', 'Default', 1).replace(
            'Your Chromium', 'Your Compass'))

  def test_renames_the_authors(self):
    self.assertEqual(rename('Copyright 2026 The Chromium Authors.'),
                     'Copyright 2026 The Compass Authors.')

  def test_only_message_bodies(self):
    source = ('<message name="IDS_A" desc="Says Chromium">Open Chromium'
              '</message>')
    self.assertEqual(
        rebrand_strings.rename_messages(source),
        '<message name="IDS_A" desc="Says Chromium">Open Compass</message>')


class SourceFilesTest(unittest.TestCase):

  def test_lists_parts_recursively_and_translations(self):
    with tempfile.TemporaryDirectory() as d:
      Path(d, 'a.grd').write_text(
          '<grit><translations>'
          '<file path="resources/a_it.xtb" lang="it" />'
          '</translations><part file="b.grdp" /></grit>')
      Path(d, 'b.grdp').write_text('<grit-part><part file="sub/c.grdp" />'
                                   '</grit-part>')
      os.makedirs(Path(d, 'sub'))
      Path(d, 'sub', 'c.grdp').write_text('<grit-part />')
      parts, xtbs = rebrand_strings.source_files(str(Path(d, 'a.grd')))
      self.assertEqual(parts, ['b.grdp', os.path.join('sub', 'c.grdp')])
      self.assertEqual(xtbs, ['resources/a_it.xtb'])


class WriteResourceIdsTest(unittest.TestCase):

  def test_copies_start_ids_under_the_copies_paths(self):
    with tempfile.TemporaryDirectory() as src:
      out = Path(src, 'out', 'gen')
      os.makedirs(Path(src, 'tools'))
      ids = Path(src, 'tools', 'ids')
      ids.write_text("{'SRCDIR': '..', 'chrome/a.grd': "
                     "{'META': {'join': 2}, 'messages': [800]}}")
      result = out / 'compass' / 'resource_ids'
      rebrand_strings.write_resource_ids([os.path.join(src, 'chrome/a.grd')],
                                         str(ids), str(result), src,
                                         str(out / 'compass' / 'strings'))
      written = eval(result.read_text())
      self.assertEqual(written['SRCDIR'], '../../..')
      self.assertEqual(written['out/gen/compass/strings/chrome/a.grd'],
                       {'messages': [800]})


if __name__ == '__main__':
  unittest.main()
