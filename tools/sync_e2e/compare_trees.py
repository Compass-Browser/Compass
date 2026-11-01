#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Compares sidebar dumps (sidebar.js op=dump) of two or more instances.

  compare_trees.py A.json B.json [C.json...]

Compares what COMPASS_SPACES syncs: the spaces in order (name, color,
archive delay) with their pinned trees and Today (empty in older dumps), and
the favorites. Not the space
IDs (compared too, but reported separately), the active space, or live
state. Titles of tabs that were never loaded on an instance can differ
(the page title comes from the page), so tabs compare by URL and custom
title. Prints the differences; exit status 1 if there are any.
"""

import json
import sys


def node(n):
  out = {'type': n['type']}
  if n['type'] == 'tab':
    out['url'] = n.get('url')
    if n.get('customTitle'):
      out['customTitle'] = n['customTitle']
  else:
    out['title'] = n.get('title')
  if n.get('collapsed'):
    out['collapsed'] = True
  if n.get('children'):
    out['children'] = [node(c) for c in n['children']]
  return out


def local_only(n):
  """New tab and blank pages stay on their device, and chrome:// pages the
  steps open for a moment (sync-internals) come and go."""
  url = n.get('url') or ''
  return n['type'] == 'tab' and (url.startswith('about:') or
                                 url.startswith('chrome://'))


def today(nodes):
  return [node(n) for n in nodes if not local_only(n)]


def synced(dump):
  return {
      'spaces': [{
          'name': s['name'],
          'color': s['color'],
          'archiveAfterHours': s.get('archiveAfterHours'),
          'pinned': [node(n) for n in s.get('pinned', [])],
          'today': today(s.get('today', [])),
      } for s in dump['spaces']],
      'favorites': [node(n) for n in dump.get('favorites', [])],
  }


def main(paths):
  dumps = [json.load(open(p)) for p in paths]
  first = synced(dumps[0])
  ids = [[s['id'] for s in d['spaces']] for d in dumps]
  status = 0
  for path, dump, space_ids in zip(paths[1:], dumps[1:], ids[1:]):
    other = synced(dump)
    if other != first:
      status = 1
      print('DIFFERENT: %s vs %s' % (paths[0], path))
      print(json.dumps(first, indent=1))
      print(json.dumps(other, indent=1))
    else:
      print('same trees: %s and %s' % (paths[0], path))
    if space_ids != ids[0]:
      status = 1
      print('different space IDs: %s vs %s' % (ids[0], space_ids))
  return status


if __name__ == '__main__':
  if len(sys.argv) < 3:
    print(__doc__, file=sys.stderr)
    sys.exit(2)
  sys.exit(main(sys.argv[1:]))
