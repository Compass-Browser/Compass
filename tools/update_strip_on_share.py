#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Updates third_party/strip_on_share/ from Firefox's main branch.

  update_strip_on_share.py [--commit SHA]

Downloads both strip-on-share lists at the last Firefox commit that changed
them (or SHA), checks they still have the format clean_url.cc reads, writes
them unmodified and puts the commit in the README. Prints the commit, or
"unchanged". The weekly workflow (.github/workflows/strip-on-share.yml) runs
it and opens a pull request when the files changed.
"""

import argparse
import json
import os
import re
import sys
import urllib.request

REPO = 'mozilla-firefox/firefox'
SOURCE_DIR = 'toolkit/components/antitracking/StripOnShareLists'
# Firefox's path under SOURCE_DIR -> ours under third_party/strip_on_share.
FILES = {
    'MPL2/StripOnShare.json': 'MPL2/StripOnShare.json',
    'LGPL/StripOnShareLGPL.json': 'LGPL/StripOnShareLGPL.json',
}
ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    'third_party', 'strip_on_share')
COMMIT_LINE = re.compile(r'commit\n`[0-9a-f]{40}` \(\d{4}-\d{2}-\d{2}\)')


def fetch(url):
  request = urllib.request.Request(url, headers={'User-Agent': 'compass-tools'})
  token = os.environ.get('GITHUB_TOKEN')
  if token and url.startswith('https://api.github.com/'):
    request.add_header('Authorization', 'Bearer ' + token)
  with urllib.request.urlopen(request, timeout=60) as response:
    return response.read()


def latest_commit():
  """The last commit on main that changed the lists: (sha, YYYY-MM-DD)."""
  data = json.loads(
      fetch('https://api.github.com/repos/%s/commits?path=%s&per_page=1' %
            (REPO, SOURCE_DIR)))
  return data[0]['sha'], data[0]['commit']['committer']['date'][:10]


def check_list(name, text):
  """Raises ValueError unless |text| is a list clean_url.cc can read."""
  try:
    rules = json.loads(text)
  except ValueError as e:
    raise ValueError('%s: not JSON: %s' % (name, e))
  if not isinstance(rules, dict) or not rules:
    raise ValueError('%s: not a non-empty JSON object' % name)
  for key, rule in rules.items():
    if not isinstance(rule, dict):
      raise ValueError('%s: rule %r is not an object' % (name, key))
    params = rule.get('queryParams')
    if not isinstance(params, list) or not all(
        isinstance(p, str) for p in params):
      raise ValueError('%s: rule %r has no queryParams list' % (name, key))
    for field in ('hosts', 'schemelessSites'):
      value = rule.get(field, [])
      if not isinstance(value, list) or not all(
          isinstance(h, str) for h in value):
        raise ValueError('%s: rule %r: %s is not a list' % (name, key, field))
    if not rule.get('isGlobal') and not rule.get('hosts') and not rule.get(
        'schemelessSites'):
      raise ValueError('%s: rule %r applies nowhere' % (name, key))
  if not any(rule.get('isGlobal') for rule in rules.values()):
    raise ValueError('%s: no global rule' % name)


def with_commit(readme, sha, date):
  """|readme| naming |sha| and |date| as the source commit."""
  updated, count = COMMIT_LINE.subn('commit\n`%s` (%s)' % (sha, date), readme)
  if count != 1:
    raise ValueError('README.md: no single "commit `<sha>` (<date>)" line')
  return updated


def main():
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('--commit', help='a Firefox commit (default: the last '
                      'one that changed the lists)')
  args = parser.parse_args()

  if args.commit:
    sha = args.commit
    data = json.loads(
        fetch('https://api.github.com/repos/%s/commits/%s' % (REPO, sha)))
    date = data['commit']['committer']['date'][:10]
  else:
    sha, date = latest_commit()

  downloads = {}
  for source, ours in FILES.items():
    raw = fetch('https://raw.githubusercontent.com/%s/%s/%s/%s' %
                (REPO, sha, SOURCE_DIR, source))
    check_list(source, raw.decode('utf-8'))
    downloads[ours] = raw

  changed = False
  for ours, raw in downloads.items():
    path = os.path.join(ROOT, ours)
    with open(path, 'rb') as f:
      if f.read() == raw:
        continue
    with open(path, 'wb') as f:
      f.write(raw)
    changed = True

  if not changed:
    print('unchanged')
    return 0

  readme_path = os.path.join(ROOT, 'README.md')
  with open(readme_path, encoding='utf-8') as f:
    readme = f.read()
  with open(readme_path, 'w', encoding='utf-8') as f:
    f.write(with_commit(readme, sha, date))
  print('%s %s' % (sha, date))
  return 0


if __name__ == '__main__':
  sys.exit(main())
