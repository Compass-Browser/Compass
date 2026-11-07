# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import datetime
import plistlib
import sys
import tempfile
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

  def test_provisioning_profile_arguments(self):
    for command in (['sign'], ['release', 'publish']):
      args = parser().parse_args(command)
      self.assertIsNone(args.provisioning_profile)
      self.assertFalse(args.no_provisioning_profile)
    args = parser().parse_args(['sign', '--provisioning-profile', 'x'])
    self.assertEqual(args.provisioning_profile, 'x')


APP_ID = '4M33B5HNDS.it.cipi1965.compass'
NOW = datetime.datetime(2026, 11, 8, tzinfo=datetime.timezone.utc)


def profile(**changes):
  data = {
      'Name': 'Compass',
      'Entitlements': {mac_sign.ENT_APP_ID: APP_ID},
      'ProvisionsAllDevices': True,
      'ExpirationDate': datetime.datetime(2044, 11, 2, 22, 59, 37),
  }
  data.update(changes)
  return data


class ProfileTest(unittest.TestCase):

  def test_good(self):
    mac_sign.check_profile(profile(), APP_ID, NOW)

  def test_other_app(self):
    with self.assertRaisesRegex(mac_sign.ProfileError, 'is for ' + APP_ID):
      mac_sign.check_profile(profile(), 'ABCDE12345.it.cipi1965.compass', NOW)
    with self.assertRaises(mac_sign.ProfileError):
      mac_sign.check_profile(profile(Entitlements={}), APP_ID, NOW)

  def test_development_profile(self):
    with self.assertRaisesRegex(mac_sign.ProfileError, 'Developer ID'):
      mac_sign.check_profile(profile(ProvisionsAllDevices=None), APP_ID, NOW)

  def test_expired(self):
    with self.assertRaisesRegex(mac_sign.ProfileError, 'expired'):
      mac_sign.check_profile(
          profile(ExpirationDate=datetime.datetime(2026, 1, 1)), APP_ID, NOW)


class PrepareAppIdentifierTest(unittest.TestCase):

  def setUp(self):
    self.dir = tempfile.TemporaryDirectory()
    root = Path(self.dir.name)
    self.app = root / 'Compass.app'
    (self.app / 'Contents').mkdir(parents=True)
    with open(self.app / 'Contents' / 'Info.plist', 'wb') as f:
      plistlib.dump({'CFBundleIdentifier': 'it.cipi1965.compass'}, f)
    self.packaging = root / 'Compass Packaging'
    self.packaging.mkdir()
    self.entitlements = self.packaging / mac_sign.APP_ENTITLEMENTS
    with open(self.entitlements, 'wb') as f:
      plistlib.dump({'com.apple.security.device.camera': True}, f)
    self.profile = root / 'Compass.provisionprofile'
    self.profile.write_bytes(b'signed profile')
    self.embedded = self.app / 'Contents' / mac_sign.EMBEDDED_PROFILE

  def tearDown(self):
    self.dir.cleanup()

  def read_entitlements(self):
    with open(self.entitlements, 'rb') as f:
      return plistlib.load(f)

  def prepare(self, profile_path, team='4M33B5HNDS', decoded=None):
    return mac_sign.prepare_app_identifier(
        self.app, self.packaging, team, profile_path,
        decode=lambda path: decoded or profile())

  def test_embeds_and_entitles(self):
    self.assertEqual(self.prepare(self.profile), APP_ID)
    self.assertEqual(self.embedded.read_bytes(), b'signed profile')
    self.assertEqual(self.read_entitlements(), {
        'com.apple.security.device.camera': True,
        mac_sign.ENT_APP_ID: APP_ID,
    })

  def test_without_takes_both_out(self):
    self.prepare(self.profile)
    self.assertIsNone(self.prepare(None))
    self.assertFalse(self.embedded.exists())
    self.assertEqual(self.read_entitlements(),
                     {'com.apple.security.device.camera': True})

  def test_wrong_profile_changes_nothing(self):
    with self.assertRaises(mac_sign.ProfileError):
      self.prepare(self.profile, team='ABCDE12345')
    with self.assertRaises(mac_sign.ProfileError):
      self.prepare(self.profile, team=None)
    self.assertFalse(self.embedded.exists())
    self.assertNotIn(mac_sign.ENT_APP_ID, self.read_entitlements())


if __name__ == '__main__':
  unittest.main()
