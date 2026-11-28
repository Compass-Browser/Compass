# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compass_tool import monitor  # noqa: E402
from compass_tool.monitor import (DONE, FAILED, RUNNING, WAITING,  # noqa: E402
                                  Rate, Step)


class ParseTest(unittest.TestCase):

  def test_ninja_takes_the_last_status_line(self):
    text = ('[10/20356] 28.65s S CXX a.obj\r\n'
            '[799/801] 11m03.01s S LINK compass.dll\r\n')
    self.assertEqual(monitor.parse_ninja(text), (799, 801, 663.01))
    self.assertEqual(monitor.parse_ninja('[5/9] 1h02m03.5s F CXX x.o'),
                     (5, 9, 3723.5))
    self.assertIsNone(monitor.parse_ninja('no build here'))

  def test_siso_ps(self):
    text = ('\tSiso is running in /src/out/Release-x64 '
            '[200/581 (remaining: 381)]\n\t  DURATION PHASE DESC\n')
    self.assertEqual(monitor.parse_siso_ps(text), (200, 581))
    self.assertIsNone(monitor.parse_siso_ps('siso is not running'))

  def test_etime(self):
    self.assertEqual(monitor.parse_etime('05:07'), 307)
    self.assertEqual(monitor.parse_etime('01:02:03'), 3723)
    self.assertEqual(monitor.parse_etime('2-01:00:00'), 176400)

  def test_nettop(self):
    text = (',bytes_out,\nkernel_task.0,15478164,\n'
            'gh.4359,265482240,\nCompass Helper.812,10,\n')
    self.assertEqual(monitor.parse_nettop(text),
                     {0: 15478164, 4359: 265482240, 812: 10})

  def test_lsof_offsets_hex_and_decimal(self):
    text = 'p4359\nf3\no0x6494000\nn/a/b.flatpak\nf4\no0t50\nn/c.dmg\n'
    self.assertEqual(monitor.parse_lsof_offsets(text),
                     {'/a/b.flatpak': 0x6494000, '/c.dmg': 50})

  def test_upload_files_keeps_paths_with_spaces(self):
    with tempfile.TemporaryDirectory() as tmp:
      a = Path(tmp) / 'Application Support' / 'Compass-1.dmg'
      a.parent.mkdir()
      a.write_bytes(b'x')
      b = Path(tmp) / 'Compass-1-2.delta'
      b.write_bytes(b'y')
      args = 'gh release upload v1 --clobber %s %s %s/missing.exe' % (a, b,
                                                                     tmp)
      self.assertEqual(monitor.upload_files(args), [a, b])

  def test_version_and_job_times(self):
    self.assertEqual(monitor.version_time('20261127.2339'),
                     monitor.job_time('compass-linux-20261127-233900'))
    self.assertEqual(monitor.version_time('nope'), 0.0)

  def test_pick_job_takes_the_first_since_the_version(self):
    jobs = [{'id': 'compass-linux-20261127-184244', 'recipe': 'linux'},
            {'id': 'compass-linux-20261127-234003', 'recipe': 'linux'},
            {'id': 'compass-linux-20261128-090000', 'recipe': 'linux'},
            {'id': 'compass-win-release-build-20261127-233959',
             'recipe': 'win-release-build'}]
    self.assertEqual(
        monitor.pick_job(jobs, 'linux', '20261127.2339')['id'],
        'compass-linux-20261127-234003')
    self.assertIsNone(monitor.pick_job(jobs, 'linux', '20261129.0000'))


class StepsTest(unittest.TestCase):

  def states(self, started, alive, ok):
    steps = [Step(str(i)) for i in range(len(started))]
    monitor.settle(steps, started, alive, ok)
    return [s.state for s in steps]

  def test_a_later_step_finishes_the_earlier_ones(self):
    # The log can lag: a step that left no trace is done once a later one ran.
    self.assertEqual(self.states([True, False, True, False], True, False),
                     [DONE, DONE, RUNNING, WAITING])

  def test_a_run_that_stopped_fails_its_step(self):
    self.assertEqual(self.states([True, True, False], False, False),
                     [DONE, FAILED, 'absent'])

  def test_a_finished_run_is_done_throughout(self):
    self.assertEqual(self.states([True, False, False], False, True),
                     [DONE, DONE, DONE])

  def test_overall(self):
    def steps(*states):
      out = [Step('s') for _ in states]
      for step, state in zip(out, states):
        step.state = state
      return out
    self.assertEqual(monitor.overall(steps(DONE, RUNNING)), RUNNING)
    self.assertEqual(monitor.overall(steps(DONE, DONE)), DONE)
    self.assertEqual(monitor.overall(steps(WAITING, WAITING)), WAITING)
    self.assertEqual(monitor.overall(steps(DONE, FAILED)), FAILED)


class RateTest(unittest.TestCase):

  def test_eta_from_the_recent_rate(self):
    rate = Rate(window=60)
    rate.add(100, now=0)
    rate.add(160, now=30)
    self.assertAlmostEqual(rate.per_second(), 2.0)
    self.assertAlmostEqual(rate.eta(40), 20.0)

  def test_no_eta_until_it_has_moved(self):
    rate = Rate()
    rate.add(5, now=0)
    self.assertIsNone(rate.eta(10))
    rate.add(5, now=30)
    self.assertIsNone(rate.eta(10))

  def test_a_counter_that_restarts_starts_over(self):
    rate = Rate()
    rate.add(500, now=0)
    rate.add(10, now=10)
    self.assertIsNone(rate.per_second())


class FitTest(unittest.TestCase):

  def test_fit(self):
    self.assertEqual(monitor.fit('left', '9:59 ', 12), 'left   9:59 ')
    self.assertEqual(monitor.fit('a long title', '1:00 ', 10), 'a lo…1:00 ')
    self.assertEqual(monitor.fit('x', 'toolong', 4), 'long')


if __name__ == '__main__':
  unittest.main()
