# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import base64
import io
import os
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compass_tool import package_managers as pm  # noqa: E402
from compass_tool.cli import parser  # noqa: E402

SHA = 'ab' * 32
WIN_SHA = '77f23da12c5647d281843e6c7da52ef19ebcb8e5137dba31cf47b47c9603ebb9'


def mac_release(version='20261115.1657'):
  return {'version': version, 'channel': 'canary', 'chromium': '157.0.8103.51',
          'date': '2026-11-15T17:01:51Z',
          'mac': {'sha256': SHA, 'minimum_system_version': '13.0'}}


def win_release(version='20261115.1718', build=31):
  return {
      'version': version, 'channel': 'canary', 'chromium': '157.0.8103.51',
      'date': '2026-11-15T17:03:47Z',
      'win': {
          'url': 'https://github.com/Compass-Browser/Compass/releases/'
                 'download/v%s/CompassSetup-%s.exe' % (version, version),
          'sha256': WIN_SHA, 'build': build,
      },
  }


class FakeApi:
  """Answers `gh api` calls from |replies| (method, path) -> reply, and
  records them."""

  def __init__(self, replies):
    self.replies = replies
    self.calls = []

  def __call__(self, method, path, payload=None, missing_ok=False):
    self.calls.append((method, path, payload))
    # The longest matching prefix wins; a prefix ends at a / or ?.
    for (m, prefix), reply in sorted(self.replies.items(),
                                     key=lambda item: -len(item[0][1])):
      if m == method and path.startswith(prefix):
        if prefix != path and prefix[-1] != '/' and \
            path[len(prefix)] not in '?/':
          continue
        if isinstance(reply, Exception):
          raise reply
        return reply
    if missing_ok:
      return None
    raise AssertionError('unexpected %s %s' % (method, path))

  def made(self, method, prefix):
    return [c for c in self.calls if c[0] == method and
            c[1].startswith(prefix)]


class CaskTest(unittest.TestCase):

  def test_cask(self):
    text = pm.cask('20261115.1657', SHA)
    self.assertTrue(text.startswith('cask "compass" do\n'))
    self.assertIn('  version "20261115.1657"\n', text)
    self.assertIn('  sha256 "%s"\n' % SHA, text)
    self.assertIn('releases/download/v#{version}/Compass-#{version}.dmg"',
                  text)
    self.assertIn('  auto_updates true\n', text)
    self.assertIn('  depends_on macos: :ventura\n', text)
    self.assertIn('  app "Compass.app"\n', text)
    self.assertIn('json.dig("mac", "version")', text)
    self.assertIn('"~/Library/Preferences/it.cipi1965.compass.plist",', text)
    self.assertTrue(text.endswith('end\n'))

  def test_zap_paths_are_sorted(self):
    # brew style's Cask/ArrayAlphabetization.
    text = pm.cask('1.0', SHA)
    zap = text.split('zap trash: [\n')[1].split('  ]')[0]
    paths = [line.strip().strip(',"') for line in zap.splitlines()]
    self.assertEqual(paths, sorted(paths, key=str.lower))

  def test_macos_requirement(self):
    self.assertEqual(pm.macos_requirement('13.0'), ':ventura')
    self.assertEqual(pm.macos_requirement('14.2'), ':sonoma')
    self.assertEqual(pm.macos_requirement(''), ':ventura')
    with self.assertRaises(pm.PackageError):
      pm.macos_requirement('12.0')

  def test_bad_input(self):
    with self.assertRaises(pm.PackageError):
      pm.cask('20261115.1657"; system "x', SHA)
    with self.assertRaises(pm.PackageError):
      pm.cask('20261115.1657', 'nothex')

  def test_cask_version(self):
    self.assertEqual(pm.cask_version(pm.cask('20261115.1657', SHA)),
                     '20261115.1657')
    self.assertIsNone(pm.cask_version('cask "x" do\nend\n'))


class WingetManifestTest(unittest.TestCase):

  def manifests(self, **kwargs):
    files = pm.winget_manifests(win_release(**kwargs))
    folder = 'manifests/c/Compass/Browser/20261115.1718/'
    return {path[len(folder):]: text for path, text in files.items()
            if path.startswith(folder)}

  def test_paths(self):
    self.assertEqual(sorted(self.manifests()), [
        'Compass.Browser.installer.yaml',
        'Compass.Browser.locale.en-US.yaml',
        'Compass.Browser.yaml',
    ])

  def test_every_manifest(self):
    for name, text in self.manifests().items():
      self.assertIn('\nPackageIdentifier: Compass.Browser\n', text, name)
      self.assertIn('\nPackageVersion: 20261115.1718\n', text, name)
      self.assertTrue(text.endswith('ManifestVersion: 1.28.0\n'), name)
      self.assertIn('$schema=https://aka.ms/winget-manifest.', text)
      self.assertNotIn('\t', text)

  def test_installer(self):
    text = self.manifests()['Compass.Browser.installer.yaml']
    for line in ('InstallerType: exe', 'Scope: user',
                 '  Silent: --do-not-launch-chrome',
                 '  SilentWithProgress: --do-not-launch-chrome',
                 '  Log: --verbose-logging --log-file="<LOGPATH>"',
                 '- InstallerReturnCode: 4', '  ReturnResponse: downgrade',
                 'UpgradeBehavior: install', 'ProductCode: Compass',
                 'ReleaseDate: 2026-11-15', '- Architecture: x64',
                 '  InstallerSha256: ' + WIN_SHA.upper(),
                 '  DisplayVersion: 157.31.8103.51',
                 '  Publisher: The Compass Authors',
                 '  InstallerUrl: https://github.com/Compass-Browser/Compass/'
                 'releases/download/v20261115.1718/'
                 'CompassSetup-20261115.1718.exe',
                 'ManifestType: installer'):
      self.assertIn('\n' + line + '\n', text)

  def test_locale(self):
    text = self.manifests()['Compass.Browser.locale.en-US.yaml']
    for line in ('PackageLocale: en-US', 'Publisher: The Compass Authors',
                 'PackageName: Compass', 'License: BSD-3-Clause',
                 'ReleaseNotesUrl: https://compassbrowser.org/changelog/'
                 'windows', 'ManifestType: defaultLocale'):
      self.assertIn('\n' + line + '\n', text)

  def test_version(self):
    text = self.manifests()['Compass.Browser.yaml']
    self.assertIn('\nDefaultLocale: en-US\nManifestType: version\n', text)

  def test_display_version(self):
    self.assertEqual(pm.win_display_version('157.0.8103.51', 31),
                     '157.31.8103.51')
    with self.assertRaises(pm.PackageError):
      pm.win_display_version('157.0.8103.51', 0)
    with self.assertRaises(pm.PackageError):
      pm.win_display_version('157.0', 3)

  def test_refuses_what_it_cannot_describe(self):
    with self.assertRaises(pm.PackageError):
      pm.winget_manifests(mac_release())
    bad = win_release()
    bad['win']['url'] = 'http://example.com/x.exe'
    with self.assertRaises(pm.PackageError):
      pm.winget_manifests(bad)
    bad = win_release()
    bad['win']['sha256'] = 'ee'
    with self.assertRaises(pm.PackageError):
      pm.winget_manifests(bad)

  def test_pr_text(self):
    self.assertEqual(pm.winget_pr_title('1.2', True),
                     'New package: Compass.Browser version 1.2')
    self.assertEqual(pm.winget_pr_title('1.2', False),
                     'Update: Compass.Browser to 1.2')
    body = pm.winget_pr_body('1.2')
    self.assertIn('- [x] This PR only modifies one (1) manifest', body)
    self.assertIn('- [ ] Tested manifest locally', body)
    self.assertIn('- [x] Tested manifest locally',
                  pm.winget_pr_body('1.2', tested=True))


class TapTest(unittest.TestCase):

  def current(self, version):
    text = pm.cask(version, SHA)
    return {'sha': 'blob1',
            'content': base64.b64encode(text.encode()).decode()}

  def test_updates_an_older_cask(self):
    api = FakeApi({
        ('GET', 'repos/Compass-Browser/homebrew-tap/contents/'):
            self.current('20261114.0100'),
        ('PUT', 'repos/Compass-Browser/homebrew-tap/contents/'):
            {'commit': {'sha': 'c0ffee'}},
    })
    self.assertEqual(pm.update_tap(mac_release(), api), 'c0ffee')
    (_, path, payload), = api.made('PUT', 'repos/')
    self.assertEqual(path, 'repos/Compass-Browser/homebrew-tap/contents/'
                     'Casks/compass.rb')
    self.assertEqual(payload['sha'], 'blob1')
    self.assertEqual(payload['message'], 'compass 20261115.1657')
    self.assertEqual(base64.b64decode(payload['content']).decode(),
                     pm.cask('20261115.1657', SHA))

  def test_creates_the_cask(self):
    api = FakeApi({('PUT', 'repos/'): {'commit': {'sha': 'c0ffee'}}})
    pm.update_tap(mac_release(), api)
    self.assertNotIn('sha', api.made('PUT', 'repos/')[0][2])

  def test_leaves_a_newer_cask(self):
    api = FakeApi({('GET', 'repos/'): self.current('20261116.0100')})
    self.assertIsNone(pm.update_tap(mac_release(), api))
    self.assertFalse(api.made('PUT', 'repos/'))


class WingetSubmitTest(unittest.TestCase):

  def api(self, extra=None):
    replies = {
        ('GET', 'user'): {'login': 'someone'},
        ('GET', 'repos/microsoft/winget-pkgs/pulls'): [],
        ('GET', 'repos/someone/winget-pkgs'):
            {'parent': {'full_name': 'microsoft/winget-pkgs'}},
        ('GET', 'repos/someone/winget-pkgs/git/'): None,
        ('GET', 'repos/microsoft/winget-pkgs/git/ref/heads/master'):
            {'object': {'sha': 'base'}},
        ('GET', 'repos/microsoft/winget-pkgs/git/commits/base'):
            {'tree': {'sha': 'basetree'}},
        ('POST', 'repos/someone/winget-pkgs/git/trees'): {'sha': 'tree'},
        ('POST', 'repos/someone/winget-pkgs/git/commits'): {'sha': 'commit'},
        ('POST', 'repos/someone/winget-pkgs/git/refs'): {},
        ('POST', 'repos/microsoft/winget-pkgs/pulls'):
            {'html_url': 'https://github.com/microsoft/winget-pkgs/pull/1'},
    }
    replies.update(extra or {})
    return FakeApi(replies)

  def test_opens_a_pull_request(self):
    api = self.api()
    self.assertEqual(pm.submit_winget(win_release(), api),
                     'https://github.com/microsoft/winget-pkgs/pull/1')
    (_, _, tree), = api.made('POST', 'repos/someone/winget-pkgs/git/trees')
    self.assertEqual(tree['base_tree'], 'basetree')
    self.assertEqual(sorted(e['path'] for e in tree['tree']), sorted(
        pm.winget_manifests(win_release())))
    (_, _, commit), = api.made('POST', 'repos/someone/winget-pkgs/git/commits')
    self.assertEqual(commit['parents'], ['base'])
    self.assertEqual(commit['message'],
                     'New package: Compass.Browser version 20261115.1718')
    (_, _, ref), = api.made('POST', 'repos/someone/winget-pkgs/git/refs')
    self.assertEqual(ref, {'ref': 'refs/heads/Compass.Browser-20261115.1718',
                           'sha': 'commit'})
    (_, _, pr), = api.made('POST', 'repos/microsoft/winget-pkgs/pulls')
    self.assertEqual(pr['head'], 'someone:Compass.Browser-20261115.1718')
    self.assertEqual(pr['base'], 'master')

  def test_an_update(self):
    # The package's folder exists, this version's doesn't.
    api = self.api({
        ('GET', 'repos/microsoft/winget-pkgs/contents/manifests/c/Compass/'
         'Browser'): [{'name': '20261115.0451'}],
        ('GET', 'repos/microsoft/winget-pkgs/contents/manifests/c/Compass/'
         'Browser/20261115.1718'): None,
    })
    pm.submit_winget(win_release(), api)
    (_, _, pr), = api.made('POST', 'repos/microsoft/winget-pkgs/pulls')
    self.assertEqual(pr['title'], 'Update: Compass.Browser to 20261115.1718')

  def test_nothing_when_the_version_is_there(self):
    api = self.api({
        ('GET', 'repos/microsoft/winget-pkgs/contents/manifests/c/Compass/'
         'Browser/20261115.1718'): [{'name': 'Compass.Browser.yaml'}],
    })
    self.assertIsNone(pm.submit_winget(win_release(), api))
    self.assertFalse(api.made('POST', 'repos/'))

  def test_nothing_when_a_pull_request_is_open(self):
    api = self.api({
        ('GET', 'repos/microsoft/winget-pkgs/pulls'): [{'number': 1}],
    })
    self.assertIsNone(pm.submit_winget(win_release(), api))
    self.assertFalse(api.made('POST', 'repos/'))

  def test_forks_when_needed(self):
    api = self.api({
        ('POST', 'repos/microsoft/winget-pkgs/forks'):
            {'full_name': 'someone/winget-pkgs'},
    })
    del api.replies[('GET', 'repos/someone/winget-pkgs')]
    polls = []

    def ready_on_the_third_poll(_seconds):
      polls.append(1)
      if len(polls) == 2:
        api.replies[('GET', 'repos/someone/winget-pkgs/git/ref/heads/'
                     'master')] = {'object': {'sha': 'base'}}

    with mock.patch.object(pm.time, 'sleep', ready_on_the_third_poll):
      pm.submit_winget(win_release(), api)
    self.assertTrue(api.made('POST', 'repos/microsoft/winget-pkgs/forks'))
    self.assertEqual(len(polls), 2)

  def test_gives_up_on_a_fork_that_never_comes(self):
    api = self.api({
        ('POST', 'repos/microsoft/winget-pkgs/forks'):
            {'full_name': 'someone/winget-pkgs'},
    })
    del api.replies[('GET', 'repos/someone/winget-pkgs')]
    with mock.patch.object(pm.time, 'sleep'), \
        self.assertRaises(pm.PackageError):
      pm.submit_winget(win_release(), api)


class AfterReleaseTest(unittest.TestCase):

  def run_quietly(self, fn, *args, **kwargs):
    out = io.StringIO()
    with redirect_stdout(out):
      fn(*args, **kwargs)
    return out.getvalue()

  def test_never_raises(self):
    def broken(*_args, **_kwargs):
      raise pm.PackageError('no network')
    out = self.run_quietly(pm.after_mac_release, mac_release(), api=broken)
    self.assertIn('warning: updating the Homebrew cask failed: no network',
                  out)
    with mock.patch.object(pm, 'WINGET_ENABLED', True):
      out = self.run_quietly(pm.after_win_release, win_release(), api=broken)
    self.assertIn('warning: the winget pull request failed', out)

  def test_winget_paused(self):
    # While WINGET_ENABLED is off, a Windows release sends nothing to
    # winget-pkgs; Homebrew still runs.
    def unused(*_args, **_kwargs):
      raise AssertionError('called')
    with mock.patch.object(pm, 'WINGET_ENABLED', False):
      out = self.run_quietly(pm.after_win_release, win_release(), api=unused)
    self.assertIn('paused', out)

  def test_skipped(self):
    def unused(*_args, **_kwargs):
      raise AssertionError('called')
    out = self.run_quietly(pm.after_mac_release, mac_release(), skip=True,
                           api=unused)
    self.assertIn('skipped', out)
    with mock.patch.dict(os.environ, {pm.SKIP_ENV: '1'}):
      out = self.run_quietly(pm.after_win_release, win_release(), api=unused)
    self.assertIn('skipped', out)
    with mock.patch.dict(os.environ, {pm.SKIP_ENV: '0'}):
      self.assertFalse(pm.skipped())

  def test_after_release_does_each_platform(self):
    release = mac_release()
    release['win'] = win_release()['win']
    with mock.patch.object(pm, 'after_mac_release') as mac, \
        mock.patch.object(pm, 'after_win_release') as win:
      self.assertEqual(pm.after_release(release), ['mac', 'win'])
    mac.assert_called_once()
    win.assert_called_once()


class CliTest(unittest.TestCase):

  def test_arguments(self):
    args = parser().parse_args(['release', 'packages', '20261115.1718',
                                '--no-package-managers'])
    self.assertEqual((args.action, args.version, args.no_package_managers),
                     ('packages', '20261115.1718', True))
    self.assertFalse(parser().parse_args(['release']).no_package_managers)


if __name__ == '__main__':
  unittest.main()
