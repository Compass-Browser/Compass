# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""`compass sync-server`: the sync server (server/) for development."""

from __future__ import annotations

import os
from pathlib import Path

DEFAULT_PORT = 8296
IMAGE = 'localhost/compass-sync-dev:latest'
CONTAINER = 'compass-sync-dev'
VOLUME = 'compass-sync-dev-data'


def data_dir(root: Path) -> Path:
  """Where `go run` keeps the database (.dev/ is ignored by git)."""
  return root / '.dev' / 'sync-server'


def go_command(root: Path, port: int):
  """(cmd, cwd, env) running the server from source."""
  env = dict(os.environ)
  env['COMPASS_SYNC_ADDR'] = ':%d' % port
  env['COMPASS_SYNC_DB'] = str(data_dir(root) / 'compass-sync.db')
  return ['go', 'run', '.'], root / 'server', env


def podman_commands(root: Path, port: int):
  """[(cmd, cwd)] building and running the image, its data in a volume."""
  server = root / 'server'
  return [
      (['podman', 'build', '-t', IMAGE, '.'], server),
      (['podman', 'run', '--rm', '--name', CONTAINER,
        '-p', '%d:8295' % port, '-v', '%s:/data' % VOLUME, IMAGE], server),
  ]
