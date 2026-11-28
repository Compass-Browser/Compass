# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'translation'))

import update_models as update  # noqa: E402


def record(source, target, file_type, version='3.0', architecture='base',
           filter_expression='', name=None):
  return {
      'sourceLanguage': source,
      'targetLanguage': target,
      'fileType': file_type,
      'version': version,
      'architecture': architecture,
      'filter_expression': filter_expression,
      'decompressedSize': 10,
      'decompressedHash': 'd' * 64,
      'attachment': {
          'location': 'main/%s' % (name or '%s%s-%s-%s-%s' % (
              source, target, file_type, version, architecture)),
          'size': 5,
          'hash': 'c' * 64,
      },
  }


def files(source, target, *types, **kwargs):
  return [record(source, target, t, **kwargs) for t in types]


class SelectTest(unittest.TestCase):

  def test_versions(self):
    self.assertGreater(update.version_key('3.1'), update.version_key('3.0'))
    self.assertGreater(update.version_key('3.0'), update.version_key('3.0a1'))
    self.assertGreater(update.version_key('3.0a2'),
                       update.version_key('3.0a1'))
    with self.assertRaises(ValueError):
      update.version_key('three')

  def test_newest_version_then_largest_architecture(self):
    records = (files('de', 'en', 'model', 'vocab', version='3.0',
                     architecture='base') +
               files('de', 'en', 'model', 'vocab', version='3.1',
                     architecture='tiny') +
               files('de', 'en', 'model', 'vocab', version='3.1',
                     architecture='base-memory'))
    version, architecture, chosen = update.select(records)['de-en']
    self.assertEqual((version, architecture), ('3.1', 'base-memory'))
    self.assertEqual(set(chosen), {'model', 'vocab'})

  def test_sets_are_never_mixed(self):
    # The newest model lacks a vocabulary: the older complete set wins.
    records = (files('en', 'it', 'model', version='3.1') +
               files('en', 'it', 'model', 'vocab', version='3.0'))
    self.assertEqual(update.select(records)['en-it'][0], '3.0')

  def test_one_vocabulary_per_side(self):
    records = files('en', 'zh-Hans', 'model', 'srcvocab', 'trgvocab')
    self.assertIn('en-zh-Hans', update.select(records))
    self.assertNotIn('en-ja', update.select(files('en', 'ja', 'model',
                                                  'srcvocab')))

  def test_filters(self):
    records = (
        files('en', 'fr', 'model', 'vocab',
              filter_expression="env.appinfo.OS == 'Android'") +
        files('en', 'es', 'model', 'vocab',
              filter_expression="env.channel == 'nightly'") +
        files('en', 'de', 'model', 'vocab',
              filter_expression="env.appinfo.OS != 'Android'"))
    self.assertEqual(set(update.select(records)), {'en-de'})

  def test_shortlists_and_unknown_types_are_left_out(self):
    records = files('de', 'en', 'model', 'vocab', 'lex', 'qualityModel')
    data = update.manifest(update.select(records))
    types = [f['type'] for f in data['directions']['de-en']['files']]
    self.assertEqual(types, ['model', 'vocab'])

  def test_manifest(self):
    data = update.manifest(update.select(files('de', 'en', 'model', 'vocab')))
    self.assertEqual(data['attachments'], update.MIRROR)
    entry = data['directions']['de-en']
    self.assertEqual(entry['version'], '3.0')
    self.assertEqual(entry['files'][0], {
        'type': 'model',
        'location': 'main/deen-model-3.0-base',
        'size': 5,
        'hash': 'c' * 64,
        'decompressed_size': 10,
        'decompressed_hash': 'd' * 64,
    })

  def test_checked_in_manifest(self):
    if not os.path.exists(update.OUTPUT):
      self.skipTest('no manifest')
    with open(update.OUTPUT, encoding='utf-8') as f:
      data = json.load(f)
    self.assertTrue(data['attachments'].startswith('https://'))
    for direction, entry in data['directions'].items():
      types = {f['type'] for f in entry['files']}
      self.assertTrue(update.complete(types), direction)
      for file in entry['files']:
        self.assertRegex(file['hash'], r'^[0-9a-f]{64}$')
        self.assertRegex(file['decompressed_hash'], r'^[0-9a-f]{64}$')
        self.assertFalse(file['location'].startswith('/'))


if __name__ == '__main__':
  unittest.main()
