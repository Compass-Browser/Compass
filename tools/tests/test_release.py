# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import importlib.util
import json
import plistlib
import sys
import tempfile
import unittest
from unittest import mock
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


def _on_github(r):
  """|r| as published since archives moved to GitHub releases."""
  r = json.loads(json.dumps(r))
  for archive in [r['mac'], *r['mac']['deltas'], r.get('win')]:
    if archive:
      archive['url'] = release.github_url(r['version'], archive['file'])
  return r


class GitHubReleaseTest(unittest.TestCase):

  MIXED = {'releases': [
      _release('20261102.1000', 'stable', win=True),
      _on_github(_release('20261103.0215', 'beta', deltas=['20261102.1000'],
                          win=True)),
  ]}
  DOWNLOADS = 'https://github.com/Compass-Browser/Compass/releases/download/'

  def test_github_url(self):
    self.assertEqual(self.DOWNLOADS + 'v20261103.0215/Compass-20261103.0215.dmg',
                     release.github_url('20261103.0215',
                                        'mac/Compass-20261103.0215.dmg'))

  def test_feeds_mix_server_and_github_archives(self):
    stable = release.channel_manifest(self.MIXED, 'stable', 'https://u')
    beta = release.channel_manifest(self.MIXED, 'beta', 'https://u')
    # Released before the move: still on the update server.
    self.assertEqual('https://u/win/CompassSetup-20261102.1000.exe',
                     stable['win']['url'])
    self.assertEqual(
        self.DOWNLOADS + 'v20261103.0215/CompassSetup-20261103.0215.exe',
        beta['win']['url'])
    feed = ET.fromstring(release.appcast(self.MIXED, 'https://u'))
    new, old = feed.findall('./channel/item')
    self.assertEqual(self.DOWNLOADS + 'v20261103.0215/Compass-20261103.0215.dmg',
                     new.find('enclosure').get('url'))
    self.assertEqual(
        self.DOWNLOADS + 'v20261103.0215/Compass-20261102.1000-20261103.0215.delta',
        new.find(SPARKLE + 'deltas').find('enclosure').get('url'))
    self.assertEqual('https://u/mac/Compass-20261102.1000.dmg',
                     old.find('enclosure').get('url'))

  def test_release_args(self):
    beta = dict(_release('20261103.0215', 'beta'), notes='Faster tabs.')
    args = release.github_release_args(beta, 'f' * 40)
    self.assertEqual(['release', 'create', 'v20261103.0215', '--title',
                      'Compass 20261103.0215'], args[:5])
    self.assertIn('Faster tabs.', args[args.index('--notes') + 1])
    self.assertIn('--prerelease', args)
    self.assertEqual('f' * 40, args[args.index('--target') + 1])
    stable = release.github_release_args(_release('20261102.1000', 'stable'),
                                         None)
    self.assertIn('--latest', stable)
    self.assertNotIn('--prerelease', stable)
    self.assertNotIn('--target', stable)

  def _gh_calls(self, fn, existing_release=False, has_commit=True):
    calls = []

    def fake_run(cmd, **kwargs):
      calls.append(cmd)
      code = 0
      if cmd[:3] == ['gh', 'release', 'view']:
        code = 0 if existing_release else 1
      if cmd[:2] == ['gh', 'api']:
        code = 0 if has_commit else 1
      return mock.Mock(returncode=code)

    with mock.patch.object(release.subprocess, 'run', side_effect=fake_run):
      fn()
    return calls

  def test_publish_creates_the_release_then_uploads(self):
    r = self.MIXED['releases'][1]
    calls = self._gh_calls(
        lambda: release.publish_assets(r, [Path('/s/a.dmg')], 'c' * 40))
    self.assertEqual(['view', 'api', 'create', 'upload'],
                     [c[1] if c[1] != 'release' else c[2] for c in calls])
    self.assertIn('c' * 40, calls[2])
    self.assertEqual(['gh', 'release', 'upload', 'v20261103.0215', '--clobber',
                      '/s/a.dmg', '--repo', 'Compass-Browser/Compass'],
                     calls[3])

  def test_publish_to_an_existing_release_only_uploads(self):
    r = self.MIXED['releases'][1]
    calls = self._gh_calls(
        lambda: release.publish_assets(r, [Path('/s/x.exe')], 'c' * 40),
        existing_release=True)
    self.assertEqual(['view', 'upload'], [c[2] for c in calls])

  def test_commit_not_on_github_tags_the_default_branch(self):
    r = self.MIXED['releases'][1]
    calls = self._gh_calls(
        lambda: release.publish_assets(r, [Path('/s/a.dmg')], 'c' * 40),
        has_commit=False)
    create = [c for c in calls if c[1:3] == ['release', 'create']][0]
    self.assertNotIn('--target', create)

  def test_promote(self):
    calls = self._gh_calls(
        lambda: release.promote_on_github(self.MIXED['releases'][1]))
    self.assertEqual(['gh', 'release', 'edit', 'v20261103.0215',
                      '--prerelease=false', '--latest', '--repo',
                      'Compass-Browser/Compass'], calls[0])
    # Released before the move: nothing on GitHub to promote.
    self.assertEqual([], self._gh_calls(
        lambda: release.promote_on_github(self.MIXED['releases'][0])))


class UploadTargetTest(unittest.TestCase):

  def _target(self, text):
    with tempfile.TemporaryDirectory() as d:
      config = Path(d) / 'upload.json'
      if text is not None:
        config.write_text(text)
      return release.upload_target(config)

  def test_host_port_and_key(self):
    host, ssh = self._target(
        '{"host": "u@example.org", "port": 2222, "key": "/k/deploy"}')
    self.assertEqual('u@example.org', host)
    self.assertEqual('ssh -p 2222 -i /k/deploy -o IdentityAgent=none '
                     '-o IdentitiesOnly=yes', ssh)

  def test_defaults(self):
    self.assertEqual(('u@example.org', 'ssh -p 22'),
                     self._target('{"host": "u@example.org"}'))

  def test_missing_or_bad_config(self):
    for text in (None, 'not json', '{}', '{"host": ""}'):
      with self.assertRaises(release.ReleaseError):
        self._target(text)


if __name__ == '__main__':
  unittest.main()
