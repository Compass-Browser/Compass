#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Compares profile dumps (profiles.js op=dump) of two or more instances.

  compare_profiles.py A.json B.json [C.json...]

Checks the switcher's order across profiles, and per profile name its
spaces (name, pinned tree) and favorites. Not the profiles' directories
(they differ between devices) or titles of tabs never loaded (tabs compare
by URL). Prints the differences; exit status 1 if there are any.
"""

import json
import sys


def node(n):
  out = {'type': n['type']}
  if n['type'] == 'tab':
    out['url'] = n.get('url')
  else:
    out['title'] = n.get('title')
  if n.get('children'):
    out['children'] = [node(c) for c in n['children']]
  return out


def synced(dump):
  return {
      'order': dump['order'],
      'profiles': {
          name: {
              'spaces': [{
                  'name': s['name'],
                  'pinned': [node(n) for n in (s.get('pinned') or [])],
              } for s in profile['spaces']],
              'favorites': [node(n) for n in profile.get('favorites', [])],
          } for name, profile in dump['profiles'].items()
      },
  }


def main(paths):
  if len(paths) < 2:
    print(__doc__)
    return 2
  dumps = []
  for path in paths:
    with open(path) as f:
      dumps.append(synced(json.load(f)))
  differ = False
  for path, dump in zip(paths[1:], dumps[1:]):
    if dump != dumps[0]:
      differ = True
      print('%s differs from %s:' % (path, paths[0]))
      print('  %s: %s' % (paths[0], json.dumps(dumps[0], sort_keys=True)))
      print('  %s: %s' % (path, json.dumps(dump, sort_keys=True)))
  if not differ:
    print('same profiles: ' + ', '.join(
        '%s (%d spaces)' % (name, len(p['spaces']))
        for name, p in sorted(dumps[0]['profiles'].items())))
  return 1 if differ else 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
