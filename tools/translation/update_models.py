#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Writes browser/translate/models.json from Firefox's translation models.

  update_models.py [--verify] [--upload] [--records FILE]

Reads Remote Settings' translations-models-v2 collection, picks one set of
files per direction (below), and writes the manifest Compass downloads
from (docs/design/translation.md, "Language models"): per direction, the
model and vocabularies with their path, sizes and SHA-256 hashes before
and after zstd. Compass checks every download against them, so a build
gets exactly the files it was made with.

Compass downloads them from its own mirror, MIRROR: the translation/
folder of BUCKET, the R2 bucket behind assets.compassbrowser.org (for
other features' files too). Mozilla's CDN turns away
browsers that say "Chrome" in their User-Agent. A file keeps its path on
Mozilla's CDN, and is never replaced or removed, so every Compass ever
built finds its files.

  --verify        download every file from Mozilla, check both hashes
                  (needs zstd on PATH; about 2.5 GB)
  --upload        copy to the mirror, after checking it, every file it
                  doesn't have yet (needs zstd, and npx for wrangler signed
                  in to the account with the bucket)
  --records FILE  read the records from FILE (the collection's JSON)
                  instead of the network

Per direction, the set is the newest version, then the largest
architecture (base, base-memory, tiny), whose records have no
filter_expression or one that only leaves Android out, and that has a
model and either one vocabulary or one per side. Lexical shortlists aren't
used (Firefox doesn't either).

Run it before each Chromium rebase, and commit the manifest.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

RECORDS_URL = ('https://firefox.settings.services.mozilla.com/v1/buckets/main/'
               'collections/translations-models-v2/records')
# Where Mozilla keeps the files, and where Compass downloads them.
ATTACHMENTS = 'https://firefox-settings-attachments.cdn.mozilla.net/'
BUCKET = 'compass-assets'
PREFIX = 'translation/'
MIRROR = 'https://assets.compassbrowser.org/' + PREFIX
OUTPUT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(
        __file__)))), 'browser', 'translate', 'models.json')

FILE_TYPES = ('model', 'vocab', 'srcvocab', 'trgvocab')
ARCHITECTURES = {'base': 2, 'base-memory': 1, 'tiny': 0}
ACCEPTED_FILTERS = ('', "env.appinfo.OS != 'Android'")


def fetch(url):
  request = urllib.request.Request(url, headers={'User-Agent': 'compass-tools'})
  with urllib.request.urlopen(request, timeout=120) as response:
    return response.read()


def version_key(version):
  """'3.1' > '3.0' > '3.0a1': numbers, then releases above pre-releases."""
  match = re.fullmatch(r'(\d+(?:\.\d+)*)(?:([a-z]+)(\d+))?', version)
  if not match:
    raise ValueError('version %r' % version)
  numbers = tuple(int(n) for n in match.group(1).split('.'))
  pre = (0, match.group(2), int(match.group(3))) if match.group(2) else (1,)
  return numbers, pre


def complete(files):
  return 'model' in files and ('vocab' in files or
                               ('srcvocab' in files and 'trgvocab' in files))


def select(records):
  """{'<from>-<to>': (version, architecture, {file type: record})}."""
  groups = {}
  for record in records:
    if record.get('fileType') not in FILE_TYPES:
      continue
    if record.get('filter_expression', '') not in ACCEPTED_FILTERS:
      continue
    key = (record['sourceLanguage'], record['targetLanguage'],
           record['version'], record.get('architecture', ''))
    groups.setdefault(key, {})[record['fileType']] = record
  best = {}
  for (source, target, version, architecture), files in groups.items():
    if not complete(files):
      continue
    rank = (version_key(version), ARCHITECTURES.get(architecture, -1))
    direction = '%s-%s' % (source, target)
    if direction not in best or rank > best[direction][0]:
      best[direction] = (rank, version, architecture, files)
  return {d: (v, a, f) for d, (_, v, a, f) in best.items()}


def manifest(selected):
  directions = {}
  for direction in sorted(selected):
    version, architecture, files = selected[direction]
    entries = []
    for file_type in FILE_TYPES:
      record = files.get(file_type)
      if not record:
        continue
      attachment = record['attachment']
      entries.append({
          'type': file_type,
          'location': attachment['location'],
          'size': attachment['size'],
          'hash': attachment['hash'],
          'decompressed_size': record['decompressedSize'],
          'decompressed_hash': record['decompressedHash'],
      })
    directions[direction] = {
        'version': version,
        'architecture': architecture,
        'files': entries,
    }
  return {
      'collection': 'translations-models-v2',
      'attachments': MIRROR,
      'directions': directions,
  }


def checked(file):
  """The file from Mozilla, if both its hashes match."""
  compressed = fetch(ATTACHMENTS + file['location'])
  if hashlib.sha256(compressed).hexdigest() != file['hash']:
    return None
  with tempfile.TemporaryDirectory() as temp:
    path = os.path.join(temp, 'file.zst')
    with open(path, 'wb') as f:
      f.write(compressed)
    plain = subprocess.run(['zstd', '-dc', path], check=True,
                           capture_output=True).stdout
  if (hashlib.sha256(plain).hexdigest() != file['decompressed_hash'] or
      len(plain) != file['decompressed_size']):
    return None
  return compressed


def mirrored(file):
  """Whether the mirror already serves the file (by its size)."""
  request = urllib.request.Request(MIRROR + file['location'], method='HEAD',
                                   headers={'User-Agent': 'compass-tools'})
  try:
    with urllib.request.urlopen(request, timeout=60) as response:
      return int(response.headers.get('Content-Length', -1)) == file['size']
  except urllib.error.HTTPError:
    return False


def upload(file, compressed):
  with tempfile.TemporaryDirectory() as temp:
    path = os.path.join(temp, 'file.zst')
    with open(path, 'wb') as f:
      f.write(compressed)
    subprocess.run(
        ['npx', '--yes', 'wrangler@4', 'r2', 'object', 'put',
         '%s/%s%s' % (BUCKET, PREFIX, file['location']), '--file', path,
         '--remote',
         '--content-type', 'application/zstd',
         '--cache-control', 'public, max-age=31536000, immutable'],
        check=True, stdout=subprocess.DEVNULL)


def each_file(data, action):
  """Runs action(direction, file); returns the failures."""
  bad = []
  for direction, entry in sorted(data['directions'].items()):
    for file in entry['files']:
      problem = action(direction, file)
      if problem:
        bad.append('%s %s: %s' % (direction, file['type'], problem))
  return bad


def verify(data):
  def action(direction, file):
    if checked(file) is None:
      return 'hash'
    print('ok %s %s' % (direction, file['type']))
  return each_file(data, action)


def upload_all(data):
  def action(direction, file):
    if mirrored(file):
      return None
    compressed = checked(file)
    if compressed is None:
      return 'hash'
    upload(file, compressed)
    print('uploaded %s %s' % (direction, file['type']))
  return each_file(data, action)


def main():
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('--verify', action='store_true')
  parser.add_argument('--upload', action='store_true')
  parser.add_argument('--records')
  args = parser.parse_args()
  if args.records:
    with open(args.records, encoding='utf-8') as f:
      raw = json.load(f)
  else:
    raw = json.loads(fetch(RECORDS_URL))
  records = raw['data'] if isinstance(raw, dict) else raw
  data = manifest(select(records))
  if args.verify or args.upload:
    bad = upload_all(data) if args.upload else verify(data)
    if bad:
      sys.exit('\n'.join(bad))
  with open(OUTPUT, 'w', encoding='utf-8') as f:
    json.dump(data, f, indent=1, sort_keys=True)
    f.write('\n')
  print('%d directions in %s' % (len(data['directions']), OUTPUT))


if __name__ == '__main__':
  main()
