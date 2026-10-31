#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Reads or edits the profiles in an instance's Local State, while it's stopped.

Sync end-to-end helper (docs/design/sync-slice-5-steps.md). Compass has no
headless way to name a profile, so the steps name them here.

  local_state.py USER_DATA_DIR list
      prints {directory: name} for every profile
  local_state.py USER_DATA_DIR rename DIRECTORY NAME
      names profile DIRECTORY ("Default", "Profile 1") NAME
  local_state.py USER_DATA_DIR prefs DIRECTORY
      prints the profile's compass.sync prefs (the seed left out)
"""

import json
import os
import sys


def main(argv):
  if len(argv) < 3:
    print(__doc__)
    return 2
  user_data_dir, command = argv[1], argv[2]
  path = os.path.join(user_data_dir, 'Local State')
  with open(path) as f:
    state = json.load(f)
  cache = state.setdefault('profile', {}).setdefault('info_cache', {})
  if command == 'list':
    print(json.dumps({d: e.get('name') for d, e in cache.items()}, indent=1))
    return 0
  if command == 'rename' and len(argv) == 5:
    directory, name = argv[3], argv[4]
    entry = cache.setdefault(directory, {})
    entry['name'] = name
    entry['is_using_default_name'] = False
    with open(path + '.tmp', 'w') as f:
      json.dump(state, f)
    os.replace(path + '.tmp', path)
    print('%s is "%s"' % (directory, name))
    return 0
  if command == 'prefs' and len(argv) == 4:
    with open(os.path.join(user_data_dir, argv[3], 'Preferences')) as f:
      prefs = json.load(f)
    sync = dict(prefs.get('compass', {}).get('sync', {}))
    if sync.get('seed'):
      sync['seed'] = '<%d chars>' % len(sync['seed'])
    sync['local_sync_backend'] = prefs.get('sync', {}).get(
        'enable_local_sync_backend')
    print(json.dumps(sync, indent=1))
    return 0
  print(__doc__)
  return 2


if __name__ == '__main__':
  sys.exit(main(sys.argv))
