# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""website/boost.js against common/boosts/share_vectors.json, in Node."""

import json
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Decodes every vector and prints what it got, as JSON.
SCRIPT = r"""
import {readFileSync} from 'node:fs';
const {decodeBoost} = await import(process.argv[1]);
const vectors = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const results = {};
for (const vector of vectors) {
  try {
    results[vector.name] = {boost: await decodeBoost(vector.fragment)};
  } catch (error) {
    results[vector.name] = {error: error.kind ?? String(error)};
  }
}
console.log(JSON.stringify(results));
"""


@unittest.skipUnless(shutil.which('node'), 'needs Node.js')
class WebsiteBoostTest(unittest.TestCase):

  def test_vectors(self):
    vectors_path = ROOT / 'common' / 'boosts' / 'share_vectors.json'
    vectors = json.loads(vectors_path.read_text(encoding='utf-8'))
    result = subprocess.run(
        ['node', '--input-type=module', '-e', SCRIPT, '--',
         (ROOT / 'website' / 'boost.js').as_uri(), str(vectors_path)],
        capture_output=True, text=True, encoding='utf-8', check=True)
    results = json.loads(result.stdout)

    for vector in (v for v in vectors if 'error' not in v):
      with self.subTest(vector['name']):
        shared = json.loads(vector['json'])
        boost = results[vector['name']]['boost']
        # The page shows what the link says; Compass normalizes it.
        self.assertEqual(boost['name'], shared.get('name', ''))
        self.assertEqual(boost['match'], shared['match'])
        self.assertEqual(boost['css'], shared.get('css', ''))
        self.assertEqual(boost['js'], shared.get('js', ''))
        self.assertEqual(boost['jsWorld'],
                         shared.get('js_world') or 'isolated')

    for vector in (v for v in vectors if 'error' in v):
      if vector['error'] == 'refused':
        # Compass's saving rules; the page only shows the Boost.
        continue
      with self.subTest(vector['name']):
        self.assertEqual(results[vector['name']].get('error'),
                         vector['error'])

  def test_page_never_renders_code_as_markup(self):
    page = (ROOT / 'website' / 'boost-page.js').read_text(encoding='utf-8')
    for sink in ('innerHTML', 'outerHTML', 'insertAdjacentHTML', 'eval(',
                 'new Function', 'document.write', 'fetch(',
                 'XMLHttpRequest', 'sendBeacon'):
      self.assertNotIn(sink, page)


if __name__ == '__main__':
  unittest.main()
