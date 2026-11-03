# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import importlib.util
import json
import plistlib
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compass_tool import release, sparkle  # noqa: E402
from compass_tool.cli import parser, set_gn_arg  # noqa: E402

SPARKLE = '{http://www.andymatuschak.org/xml-namespaces/sparkle}'


def _release(version, channel, deltas=(), win=False):
  r = {
      'version': version,
      'channel': channel,
      'commit': 'abc1234',
      'chromium': '156.0.8078.48',
      'date': '2026-11-03T01:15:00Z',
      'notes': '',
      'mac': {
          'file': 'mac/Compass-%s.dmg' % version,
          'size': 1000,
          'sha256': 'ff',
          'ed_signature': 'sig' + version,
          'minimum_system_version': '12.0',
          'deltas': [{
              'from': d,
              'file': 'mac/Compass-%s-%s.delta' % (d, version),
              'size': 10,
              'ed_signature': 'dsig',
          } for d in deltas],
      },
  }
  if win:
    r['win'] = {'file': 'win/CompassSetup-%s.exe' % version, 'size': 5,
                'sha256': 'ee', 'ed_signature': 'w'}
  return r


MANIFEST = {
    'schema': 1,
    'releases': [
        _release('20261102.1000', 'stable'),
        _release('20261103.0215', 'beta', deltas=['20261102.1000']),
    ],
}


class ReleaseTest(unittest.TestCase):

  def test_new_version_is_date_dot_time(self):
    import time
    when = time.mktime((2026, 11, 3, 2, 15, 0, 0, 0, -1))
    self.assertEqual('20261103.0215', release.new_version(when))
    self.assertGreater(release.version_key('20261103.0215'),
                       release.version_key('20261102.2359'))

  def test_beta_users_get_stable_releases_too(self):
    self.assertEqual('20261102.1000',
                     release.latest(MANIFEST, 'stable', 'mac')['version'])
    self.assertEqual('20261103.0215',
                     release.latest(MANIFEST, 'beta', 'mac')['version'])
    newer_stable = {'releases': MANIFEST['releases'] +
                    [_release('20261104.0900', 'stable')]}
    self.assertEqual('20261104.0900',
                     release.latest(newer_stable, 'beta', 'mac')['version'])
    self.assertIsNone(release.latest(MANIFEST, 'beta', 'win'))

  def test_channel_manifest(self):
    stable = release.channel_manifest(MANIFEST, 'stable', 'https://u')
    self.assertEqual('stable', stable['channel'])
    self.assertEqual('https://u/mac/Compass-20261102.1000.dmg',
                     stable['mac']['url'])
    self.assertNotIn('win', stable)

  def test_appcast(self):
    feed = ET.fromstring(release.appcast(MANIFEST, 'https://u'))
    items = feed.findall('./channel/item')
    # Newest first.
    self.assertEqual(['20261103.0215', '20261102.1000'],
                     [i.find(SPARKLE + 'version').text for i in items])
    beta, stable = items
    self.assertEqual('beta', beta.find(SPARKLE + 'channel').text)
    self.assertIsNone(stable.find(SPARKLE + 'channel'))
    self.assertEqual('156.0.8078.48',
                     beta.find(SPARKLE + 'shortVersionString').text)
    enclosure = beta.find('enclosure')
    self.assertEqual('https://u/mac/Compass-20261103.0215.dmg',
                     enclosure.get('url'))
    self.assertEqual('sig20261103.0215',
                     enclosure.get(SPARKLE + 'edSignature'))
    delta = beta.find(SPARKLE + 'deltas').find('enclosure')
    self.assertEqual('20261102.1000', delta.get(SPARKLE + 'deltaFrom'))
    self.assertIsNone(stable.find(SPARKLE + 'deltas'))

  def test_write_feeds(self):
    with tempfile.TemporaryDirectory() as tmp:
      site = Path(tmp)
      manifest = json.loads(json.dumps(MANIFEST))
      written = release.write_feeds(manifest, site)
      self.assertEqual(['releases.json', 'stable.json', 'beta.json',
                        'appcast.xml'], [p.name for p in written])
      # The feeds go up last, after the archives they name.
      self.assertEqual(written[-1], site / 'mac' / 'appcast.xml')
      self.assertEqual(manifest, release.load(site))

  def test_channel_manifest_has_windows_signature(self):
    manifest = {'releases': [_release('20261104.0900', 'stable', win=True)]}
    stable = release.channel_manifest(manifest, 'stable', 'https://u')
    self.assertEqual('https://u/win/CompassSetup-20261104.0900.exe',
                     stable['win']['url'])
    self.assertEqual('w', stable['win']['ed_signature'])

  def test_add_win_release(self):
    signer = ROOT / 'third_party' / 'sparkle' / 'bin'
    key = release.KEY_FILE
    if not (signer / 'sign_update').is_file() or not key.is_file():
      self.skipTest('needs Sparkle and the release key')
    with tempfile.TemporaryDirectory() as tmp:
      site, installer = Path(tmp) / 'site', Path(tmp) / 'mini_installer.exe'
      installer.write_bytes(b'MZ installer')
      manifest = json.loads(json.dumps(MANIFEST))
      added = release.add_win_release(manifest, installer, '20261103.0215',
                                      '', 'beta', 'abc', signer, site)
      self.assertIs(added, release.find(manifest, '20261103.0215'))
      self.assertEqual('win/CompassSetup-20261103.0215.exe',
                       added['win']['file'])
      self.assertEqual(12, added['win']['size'])
      with self.assertRaises(release.ReleaseError):
        release.add_win_release(manifest, installer, '20261103.0215', '',
                                'beta', 'abc', signer, site)
      with self.assertRaises(release.ReleaseError):
        release.add_win_release(manifest, installer, '20261105.0100', '',
                                'beta', 'abc', signer, site)

  def test_parse_signature(self):
    sig, length = release.parse_signature(
        'sparkle:edSignature="abc+/=" length="1234"\n')
    self.assertEqual(('abc+/=', 1234), (sig, length))
    # What Sparkle 2.10's sign_update prints.
    sig, length = release.parse_signature(
        'sparkle:edSignature="MoU/zk==" sparkle:length="20"\n')
    self.assertEqual(('MoU/zk==', 20), (sig, length))
    with self.assertRaises(release.ReleaseError):
      release.parse_signature('nope')

  def test_find(self):
    self.assertEqual('beta', release.find(MANIFEST, '20261103.0215')['channel'])
    with self.assertRaises(release.ReleaseError):
      release.find(MANIFEST, '1')

  def test_release_files(self):
    files = release.release_files(MANIFEST['releases'][1])
    self.assertEqual(['Compass-20261103.0215.dmg',
                      'Compass-20261102.1000-20261103.0215.delta'],
                     [f.name for f in files])

  def test_parser(self):
    args = parser().parse_args(['release'])
    self.assertEqual(('publish', 'beta', 'release'),
                     (args.action, args.channel, args.config))
    args = parser().parse_args(['release', 'promote', '20261103.0215'])
    self.assertEqual(('promote', '20261103.0215'), (args.action,
                                                     args.version))

  def test_set_gn_arg_replaces(self):
    with tempfile.TemporaryDirectory() as tmp:
      args = Path(tmp) / 'args.gn'
      args.write_text('import("//x.gn")\ncompass_version = "1"\n')
      set_gn_arg(args, 'compass_version', '20261103.0215')
      self.assertEqual('import("//x.gn")\ncompass_version = "20261103.0215"\n',
                       args.read_text())


class SparkleTest(unittest.TestCase):

  def test_prune_keeps_the_code(self):
    with tempfile.TemporaryDirectory() as tmp:
      fw = Path(tmp) / 'Sparkle.framework'
      b = fw / 'Versions' / 'B'
      for d in ('Headers', 'XPCServices/Installer.xpc', 'Updater.app',
                'Resources'):
        (b / d).mkdir(parents=True)
      (b / 'Autoupdate').write_text('x')
      (fw / 'Versions' / 'Current').symlink_to('B')
      (fw / 'Headers').symlink_to('Versions/Current/Headers')
      (fw / 'XPCServices').symlink_to('Versions/Current/XPCServices')
      (fw / 'Autoupdate').symlink_to('Versions/Current/Autoupdate')
      sparkle.prune(fw)
      self.assertEqual(['Autoupdate', 'Resources', 'Updater.app'],
                       sorted(p.name for p in b.iterdir()))
      self.assertEqual(['Autoupdate', 'Versions'],
                       sorted(p.name for p in fw.iterdir()))


def _load_stamp():
  path = ROOT / 'browser' / 'updates' / 'stamp_app_plist.py'
  spec = importlib.util.spec_from_file_location('stamp_app_plist', path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


class StampAppPlistTest(unittest.TestCase):

  def test_dev_build_keeps_upstream_version(self):
    stamp = _load_stamp()
    plist = stamp.stamp({'CFBundleVersion': '8078.48'}, '', '', '')
    self.assertEqual({'CFBundleVersion': '8078.48'}, plist)

  def test_release_build(self):
    stamp = _load_stamp()
    plist = stamp.stamp({'CFBundleVersion': '8078.48',
                         'CFBundleShortVersionString': '156.0.8078.48'},
                        '20261103.0215', 'https://u/mac/appcast.xml', 'KEY')
    self.assertEqual('20261103.0215', plist['CFBundleVersion'])
    self.assertEqual('20261103.0215', plist['CompassVersion'])
    self.assertEqual('156.0.8078.48', plist['CFBundleShortVersionString'])
    self.assertEqual('https://u/mac/appcast.xml', plist['SUFeedURL'])
    self.assertEqual('KEY', plist['SUPublicEDKey'])
    self.assertTrue(plist['SUAutomaticallyUpdate'])
    self.assertFalse(plist['SUEnableSystemProfiling'])

  def test_rejects_bad_input(self):
    stamp = _load_stamp()
    with self.assertRaises(ValueError):
      stamp.stamp({}, '2026-11-03', '', '')
    with self.assertRaises(ValueError):
      stamp.stamp({}, '', 'https://u', '')

  def test_main_round_trip(self):
    stamp = _load_stamp()
    with tempfile.TemporaryDirectory() as tmp:
      src, out = Path(tmp) / 'in.plist', Path(tmp) / 'out.plist'
      src.write_bytes(plistlib.dumps({'CFBundleVersion': '1.2'}))
      self.assertEqual(0, stamp.main(['--input', str(src), '--output',
                                      str(out), '--version=3.4']))
      self.assertEqual('3.4', plistlib.loads(out.read_bytes())
                       ['CFBundleVersion'])


if __name__ == '__main__':
  unittest.main()
