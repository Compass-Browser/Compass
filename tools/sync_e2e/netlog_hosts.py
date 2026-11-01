#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Lists the URLs a browser requested, from a --log-net-log file.

  netlog_hosts.py NETLOG.json [--urls]

Prints each host with its request count (and with --urls, each distinct URL
without its query). Hosts under google.com, googleapis.com, gstatic.com,
gvt1.com and similar are marked GOOGLE. Tolerates a log cut off by a crash.
"""

import json
import re
import sys
import urllib.parse
from collections import Counter

GOOGLE = re.compile(r'(^|\.)(google|googleapis|gstatic|gvt1|gvt2|googleusercontent|'
                    r'doubleclick|googlesyndication|youtube|ggpht|withgoogle)'
                    r'\.[a-z.]+$')


def _close(text):
  """Tries the endings a cut-off net log can be missing."""
  text = text.rstrip().rstrip(',')
  # SIGTERM: the events array is closed but the final "}" isn't written.
  for suffix in ('', '}', ']}', '{}]}', ']}}'):
    try:
      return json.loads(text + suffix)
    except json.JSONDecodeError:
      pass
  return None


def load(path):
  text = open(path, encoding='utf-8', errors='replace').read()
  log = _close(text)
  if log is not None:
    return log
  # Cut mid-event: drop partial lines from the end until it closes.
  while '\n' in text:
    text = text[:text.rfind('\n')]
    log = _close(text)
    if log is not None:
      return log
  raise ValueError('not a net log: ' + path)


def main(argv):
  if not argv:
    print(__doc__, file=sys.stderr)
    return 2
  log = load(argv[0])
  urls = Counter()
  for event in log.get('events', []):
    params = event.get('params') or {}
    url = params.get('url')
    if isinstance(url, str) and '://' in url:
      urls[url.split('?', 1)[0]] += 1
  hosts = Counter()
  for url, n in urls.items():
    hosts[urllib.parse.urlparse(url).netloc] += n
  for host, n in hosts.most_common():
    name = host.split(':')[0]
    print('%-6s %5d  %s' % ('GOOGLE' if GOOGLE.search(name) else '', n, host))
  if '--urls' in argv:
    print()
    for url, n in sorted(urls.items()):
      print('%5d  %s' % (n, url))
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
