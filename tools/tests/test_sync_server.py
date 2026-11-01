# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compass_tool import sync_server  # noqa: E402
from compass_tool.cli import parser  # noqa: E402

ROOT = Path('/repo')


class SyncServerTest(unittest.TestCase):

  def test_defaults(self):
    args = parser().parse_args(['sync-server'])
    self.assertEqual(args.port, 8296)
    self.assertFalse(args.podman)

  def test_go_run(self):
    cmd, cwd, env = sync_server.go_command(ROOT, 8296)
    self.assertEqual(cmd, ['go', 'run', '.'])
    self.assertEqual(cwd, ROOT / 'server')
    self.assertEqual(env['COMPASS_SYNC_ADDR'], ':8296')
    self.assertEqual(env['COMPASS_SYNC_DB'],
                     str(ROOT / '.dev/sync-server/compass-sync.db'))

  def test_podman_uses_its_own_names(self):
    (build, _), (run, _) = sync_server.podman_commands(ROOT, 9000)
    self.assertIn(sync_server.IMAGE, build)
    self.assertIn('9000:8295', run)
    # Never the test server's names (gosync-*) or port 8295 on the host.
    self.assertTrue(all('gosync' not in a for a in build + run))
    self.assertTrue(sync_server.CONTAINER.startswith('compass-sync-dev'))
    self.assertTrue(sync_server.VOLUME.startswith('compass-sync-dev'))


if __name__ == '__main__':
  unittest.main()
