# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compass_tool import mac_sign  # noqa: E402
from compass_tool.cli import parser  # noqa: E402

LISTING = '''\
  1) CD4B4E5FCA35C736F27E0D0FC4341719E18E3573 "Developer ID Application: Example SAS (4M33B5HNDS)"
  2) C02EAAB85048E861971346EE81B542B994FD632E "Apple Development: Created via API (9M378WNY2V)"
     2 valid identities found
'''
SECOND = ('  3) 0123456789ABCDEF0123456789ABCDEF01234567 '
          '"Developer ID Application: Other Ltd (ABCDE12345)"\n')


class MacSignTest(unittest.TestCase):

  def test_defaults(self):
    args = parser().parse_args(['sign'])
    self.assertEqual(args.config, 'release')
    self.assertEqual(args.keychain_profile, 'compass')
    self.assertFalse(args.no_notarize)

  def test_only_developer_id_identities(self):
    self.assertEqual(mac_sign.developer_id_identities(LISTING), [
        ('CD4B4E5FCA35C736F27E0D0FC4341719E18E3573',
         'Developer ID Application: Example SAS (4M33B5HNDS)')
    ])

  def test_picks_the_only_one(self):
    identity, name = mac_sign.pick_identity(LISTING)
    self.assertEqual(identity, 'CD4B4E5FCA35C736F27E0D0FC4341719E18E3573')
    self.assertEqual(mac_sign.team_id(name), '4M33B5HNDS')

  def test_several_need_a_choice(self):
    with self.assertRaises(ValueError):
      mac_sign.pick_identity(LISTING + SECOND)
    self.assertEqual(
        mac_sign.pick_identity(LISTING + SECOND, 'Other Ltd')[0],
        '0123456789ABCDEF0123456789ABCDEF01234567')
    self.assertEqual(
        mac_sign.pick_identity(LISTING + SECOND,
                               'cd4b4e5fca35c736f27e0d0fc4341719e18e3573')[0],
        'CD4B4E5FCA35C736F27E0D0FC4341719E18E3573')

  def test_none(self):
    with self.assertRaises(ValueError):
      mac_sign.pick_identity('     0 valid identities found\n')
    with self.assertRaises(ValueError):
      mac_sign.pick_identity(LISTING, 'Nobody')

  def test_sign_command(self):
    out = Path('/src/out/Release')
    cmd = mac_sign.sign_command('py', out, 'Compass', out / 'signed', 'ABC',
                                None)
    self.assertEqual(cmd, [
        'py', str(out / 'Compass Packaging' / 'sign_chrome.py'), '--input',
        str(out), '--output', str(out / 'signed'), '--identity', 'ABC'
    ])
    notarized = mac_sign.sign_command('py', out, 'Compass', out / 'signed',
                                      'ABC', 'compass')
    self.assertEqual(notarized[len(cmd):], [
        '--notarize', 'staple', '--notary-arg=--keychain-profile',
        '--notary-arg=compass'
    ])


if __name__ == '__main__':
  unittest.main()
