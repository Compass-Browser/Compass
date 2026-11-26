# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'update_translations', ROOT / 'strings' / 'update_translations.py')
update_translations = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update_translations)
check_text = update_translations.check_text


class CheckTextTest(unittest.TestCase):

  def test_accepts_matching_placeholders(self):
    self.assertIsNone(
        check_text('IDS_X', 'Elimina <ph name="SPACE_NAME"/>?', ['SPACE_NAME']))
    self.assertIsNone(check_text('IDS_X', 'Salva', []))

  def test_accepts_placeholders_in_another_order(self):
    self.assertIsNone(
        check_text('IDS_X', '<ph name="B"/> e <ph name="A"/>', ['A', 'B']))

  def test_rejects_missing_or_extra_placeholders(self):
    self.assertIn('placeholders',
                  check_text('IDS_X', 'Elimina lo spazio?', ['SPACE_NAME']))
    self.assertIn('placeholders',
                  check_text('IDS_X', 'Apri <ph name="URL"/>', []))

  def test_rejects_invalid_xml(self):
    self.assertIn('XML', check_text('IDS_X', 'Tom & Jerry', []))
    self.assertIsNone(check_text('IDS_X', 'Tom &amp; Jerry', []))


class XtbTest(unittest.TestCase):

  def test_round_trip_sorted_by_id(self):
    with tempfile.TemporaryDirectory() as d:
      path = str(Path(d, 'x_it.xtb'))
      update_translations.write_xtb(path, 'it', {
          '2': 'Due <ph name="N"/>',
          '10': 'Dieci',
      })
      text = Path(path).read_text(encoding='utf-8')
      self.assertIn('<translationbundle lang="it">', text)
      self.assertLess(text.index('id="10"'), text.index('id="2"'))
      self.assertEqual(update_translations.read_xtb(path), {
          '2': 'Due <ph name="N"/>',
          '10': 'Dieci',
      })

  def test_missing_xtb_reads_empty(self):
    self.assertEqual(update_translations.read_xtb('/nonexistent/x.xtb'), {})


if __name__ == '__main__':
  unittest.main()
