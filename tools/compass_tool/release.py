# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Releases and update feeds (`compass release`, docs/design/updates.md).

The release folder (~/Library/Application Support/Compass Release) keeps:

- sparkle_ed25519.key: Sparkle's private key (never in the repository);
- upload.json: where `upload()` rsyncs to, e.g.
  {"host": "user@server", "port": 22, "key": "~/.ssh/compass_updates_deploy"}
  ("port" and "key" are optional; never in the repository);
- site/: a mirror of https://updates.compassbrowser.org, the update server:
  - releases.json: every release, the source of truth for the rest;
  - stable.json, beta.json, canary.json: the newest release on each channel;
  - mac/appcast.xml: Sparkle's feed, every channel;
  - mac/Compass-<version>.dmg and mac/Compass-<from>-<to>.delta.

Archives (.dmg, deltas, Windows installers) are assets of a GitHub release
of Compass, tagged v<version>; the feeds stay on the update server. Releases
made before the move have their archives on the update server: an archive
entry with a "url" is on GitHub, one without is at BASE_URL/<file>.

Publishing uploads archives first and feeds last, so a feed never names a
file that isn't there yet.
"""

from __future__ import annotations

import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import tempfile
import time
from email.utils import formatdate
from pathlib import Path, PurePosixPath
from typing import Dict, List, Optional
from xml.sax.saxutils import escape, quoteattr

BASE_URL = 'https://updates.compassbrowser.org'
# Where release archives are published (`gh release`).
GITHUB_REPO = 'Compass-Browser/Compass'
# From the most to the least tested. Each channel also gets the releases of
# the channels before it, and promoting a release moves it one step left.
CHANNELS = ('stable', 'beta', 'canary')
SCHEMA = 1
# Deltas are made from this many earlier releases.
DELTA_COUNT = 3

RELEASE_DIR = Path.home() / 'Library' / 'Application Support' / \
    'Compass Release'
KEY_FILE = RELEASE_DIR / 'sparkle_ed25519.key'
SITE_DIR = RELEASE_DIR / 'site'

# The update server's rsync target (docs/design/updates.md).
UPLOAD_CONFIG = RELEASE_DIR / 'upload.json'


class ReleaseError(Exception):
  pass


# Versions ------------------------------------------------------------------


def new_version(now: Optional[float] = None) -> str:
  """Date.time, e.g. 20261103.0215, like the iOS build numbers."""
  return time.strftime('%Y%m%d.%H%M', time.localtime(now))


def version_key(version: str) -> tuple:
  return tuple(int(part) for part in version.split('.'))


def github_tag(version: str) -> str:
  return 'v' + version


def github_url(version: str, file: str) -> str:
  """Where |file| (e.g. mac/Compass-<version>.dmg) of release |version| is
  downloaded from once it's an asset of that GitHub release."""
  return 'https://github.com/%s/releases/download/%s/%s' % (
      GITHUB_REPO, github_tag(version), PurePosixPath(file).name)


def archive_url(archive: Dict, base_url: str = BASE_URL) -> str:
  """An archive entry's download URL: GitHub's, or the update server's for
  releases made before archives moved there."""
  return archive.get('url') or '%s/%s' % (base_url, archive['file'])


# Manifest ------------------------------------------------------------------


def load(site: Path = SITE_DIR) -> Dict:
  path = site / 'releases.json'
  if not path.is_file():
    return {'schema': SCHEMA, 'releases': []}
  return json.loads(path.read_text())


def save(manifest: Dict, site: Path = SITE_DIR) -> None:
  manifest['releases'].sort(key=lambda r: version_key(r['version']))
  site.mkdir(parents=True, exist_ok=True)
  (site / 'releases.json').write_text(json.dumps(manifest, indent=2) + '\n')


def find(manifest: Dict, version: str) -> Dict:
  for release in manifest['releases']:
    if release['version'] == version:
      return release
  raise ReleaseError('no release %s' % version)


def gets(channel: str, release_channel: str) -> bool:
  """Whether |channel|'s users get releases on |release_channel|: their own
  and those of every more tested channel (canary users get beta and stable
  releases too)."""
  return CHANNELS.index(release_channel) <= CHANNELS.index(channel)


def promotion(channel: str) -> str:
  """The channel a release on |channel| is promoted to by default: the next
  more tested one."""
  index = CHANNELS.index(channel)
  if index == 0:
    raise ReleaseError('nothing comes after %s' % channel)
  return CHANNELS[index - 1]


def latest(manifest: Dict, channel: str, platform: str) -> Optional[Dict]:
  """The newest release a |channel| user gets on |platform|."""
  eligible = [
      r for r in manifest['releases']
      if r.get(platform) and gets(channel, r['channel'])
  ]
  return max(eligible, key=lambda r: version_key(r['version']), default=None)


def channel_manifest(manifest: Dict, channel: str,
                     base_url: str = BASE_URL) -> Dict:
  """stable.json, beta.json, canary.json: what to download, per platform."""
  out = {'schema': SCHEMA, 'channel': channel}
  for platform in ('mac', 'win'):
    release = latest(manifest, channel, platform)
    if not release:
      continue
    archive = release[platform]
    out[platform] = {
        'version': release['version'],
        'chromium': release['chromium'],
        'commit': release['commit'],
        'date': release['date'],
        'url': archive_url(archive, base_url),
        'size': archive['size'],
        'sha256': archive['sha256'],
        'ed_signature': archive['ed_signature'],
    }
    # Windows: what the website offers, a download with a window; the
    # updater keeps reading url (mini_installer).
    if archive.get('installer'):
      windowed = archive['installer']
      out[platform]['installer'] = {
          'url': archive_url(windowed, base_url),
          'size': windowed['size'],
          'sha256': windowed['sha256'],
      }
  return out


def appcast(manifest: Dict, base_url: str = BASE_URL) -> str:
  """Sparkle's feed. Beta and canary releases carry their <sparkle:channel>;
  stable ones carry none, so every user sees them. The app picks which
  channels it reads (browser/updates/sparkle_updater_mac.mm)."""
  items = []
  releases = sorted((r for r in manifest['releases'] if r.get('mac')),
                    key=lambda r: version_key(r['version']), reverse=True)
  for release in releases:
    mac = release['mac']
    lines = [
        '    <item>',
        '      <title>Compass %s</title>' % escape(release['version']),
        '      <pubDate>%s</pubDate>' % formatdate(
            _timestamp(release['date']), usegmt=True),
        '      <sparkle:version>%s</sparkle:version>' % escape(
            release['version']),
        # Upstream's version: the browser shows it, and Compass's upgrade
        # detector compares it (browser/updates/sparkle_updater_mac.mm).
        '      <sparkle:shortVersionString>%s</sparkle:shortVersionString>' %
        escape(release['chromium']),
    ]
    if mac.get('minimum_system_version'):
      lines.append('      <sparkle:minimumSystemVersion>%s'
                   '</sparkle:minimumSystemVersion>' %
                   escape(mac['minimum_system_version']))
    if release['channel'] != 'stable':
      lines.append('      <sparkle:channel>%s</sparkle:channel>' %
                   escape(release['channel']))
    if release.get('critical'):
      lines.append('      <sparkle:criticalUpdate></sparkle:criticalUpdate>')
    lines.append('      <enclosure url=%s length="%d" '
                 'type="application/octet-stream" sparkle:edSignature=%s/>' %
                 (quoteattr(archive_url(mac, base_url)), mac['size'],
                  quoteattr(mac['ed_signature'])))
    if mac.get('deltas'):
      lines.append('      <sparkle:deltas>')
      for delta in mac['deltas']:
        lines.append(
            '        <enclosure url=%s sparkle:deltaFrom=%s length="%d" '
            'type="application/octet-stream" sparkle:edSignature=%s/>' %
            (quoteattr(archive_url(delta, base_url)),
             quoteattr(delta['from']), delta['size'],
             quoteattr(delta['ed_signature'])))
      lines.append('      </sparkle:deltas>')
    lines.append('    </item>')
    items.append('\n'.join(lines))
  return ('<?xml version="1.0" encoding="utf-8"?>\n'
          '<rss version="2.0" '
          'xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">\n'
          '  <channel>\n'
          '    <title>Compass</title>\n'
          '    <link>%s/mac/appcast.xml</link>\n'
          '%s\n'
          '  </channel>\n'
          '</rss>\n' % (base_url, '\n'.join(items)))


def _timestamp(iso: str) -> float:
  import calendar
  return calendar.timegm(time.strptime(iso, '%Y-%m-%dT%H:%M:%SZ'))


def write_feeds(manifest: Dict, site: Path = SITE_DIR) -> List[Path]:
  """Writes every feed from |manifest|; returns them, for upload."""
  save(manifest, site)
  written = [site / 'releases.json']
  for channel in CHANNELS:
    path = site / ('%s.json' % channel)
    path.write_text(json.dumps(channel_manifest(manifest, channel), indent=2) +
                    '\n')
    written.append(path)
  (site / 'mac').mkdir(parents=True, exist_ok=True)
  path = site / 'mac' / 'appcast.xml'
  path.write_text(appcast(manifest))
  written.append(path)
  return written


# Archives ------------------------------------------------------------------


def sha256(path: Path) -> str:
  digest = hashlib.sha256()
  with path.open('rb') as f:
    for block in iter(lambda: f.read(1 << 20), b''):
      digest.update(block)
  return digest.hexdigest()


_SIGNATURE = re.compile(
    r'sparkle:edSignature="([^"]+)"\s+(?:sparkle:)?length="(\d+)"')


def parse_signature(sign_update_output: str) -> tuple:
  match = _SIGNATURE.search(sign_update_output)
  if not match:
    raise ReleaseError('unexpected sign_update output: %r' %
                       sign_update_output)
  return match.group(1), int(match.group(2))


def ed_sign(sign_update: Path, path: Path, key_file: Path = KEY_FILE) -> str:
  if not key_file.is_file():
    raise ReleaseError('no Sparkle key at %s' % key_file)
  out = subprocess.run(
      [str(sign_update), '--ed-key-file', str(key_file), str(path)],
      check=True, capture_output=True, text=True).stdout
  signature, length = parse_signature(out)
  if length != path.stat().st_size:
    raise ReleaseError('sign_update measured %d bytes of %s' % (length, path))
  return signature


class MountedApp:
  """The app inside a .dmg, mounted read-only for as long as it's needed."""

  def __init__(self, dmg: Path):
    self.dmg = dmg
    self.mount = Path(tempfile.mkdtemp(prefix='compass-dmg-'))

  def __enter__(self) -> Path:
    subprocess.run(['hdiutil', 'attach', '-nobrowse', '-readonly', '-quiet',
                    '-mountpoint', str(self.mount), str(self.dmg)], check=True)
    apps = sorted(self.mount.glob('*.app'))
    if len(apps) != 1:
      self.__exit__(None, None, None)
      raise ReleaseError('%s holds %d apps' % (self.dmg, len(apps)))
    return apps[0]

  def __exit__(self, *_):
    subprocess.run(['hdiutil', 'detach', '-quiet', str(self.mount)],
                   check=False)
    shutil.rmtree(self.mount, ignore_errors=True)


def app_info(app: Path) -> Dict:
  with (app / 'Contents' / 'Info.plist').open('rb') as f:
    return plistlib.load(f)


def add_mac_release(manifest: Dict, dmg: Path, channel: str, commit: str,
                    tools: Path, site: Path = SITE_DIR,
                    notes: str = '') -> Dict:
  """Copies |dmg| into the site, makes deltas from the last releases, signs
  everything and records the release in |manifest|."""
  with MountedApp(dmg) as app:
    info = app_info(app)
  version = info.get('CompassVersion')
  if not version:
    raise ReleaseError('%s has no CompassVersion: build with compass_version '
                       'set (compass release does)' % dmg)
  if info.get('SUFeedURL') is None:
    raise ReleaseError('%s has no SUFeedURL: not a release build' % dmg)
  if any(r['version'] == version for r in manifest['releases']):
    raise ReleaseError('release %s exists already' % version)

  mac_dir = site / 'mac'
  mac_dir.mkdir(parents=True, exist_ok=True)
  archive = mac_dir / ('Compass-%s.dmg' % version)
  shutil.copyfile(dmg, archive)
  sign_update = tools / 'sign_update'
  mac = {
      'file': 'mac/' + archive.name,
      'url': github_url(version, archive.name),
      'size': archive.stat().st_size,
      'sha256': sha256(archive),
      'ed_signature': ed_sign(sign_update, archive),
      'minimum_system_version': info.get('LSMinimumSystemVersion', ''),
      'deltas': make_deltas(manifest, archive, version, tools, site),
  }
  release = {
      'version': version,
      'channel': channel,
      'commit': commit,
      'chromium': info['CFBundleShortVersionString'],
      'date': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
      'notes': notes,
      'mac': mac,
  }
  manifest['releases'].append(release)
  return release


def win_builds(manifest: Dict) -> List[int]:
  return [r['win'].get('build', 0) for r in manifest['releases']
          if r.get('win')]


def next_win_build(manifest: Dict) -> int:
  """The next Windows build number, Chromium's MINOR version in the
  installer (compass_win_build). Chromium's installer only updates in place
  when its version goes up, and two releases can share a Chromium version,
  so each one gets a new number."""
  return max(win_builds(manifest), default=0) + 1


def fetch_manifest(base_url: str = BASE_URL) -> Dict:
  """releases.json from the update server, for a machine without the site
  mirror (the Windows build machine)."""
  import urllib.request
  # The server's CDN turns away Python's default User-Agent (403).
  request = urllib.request.Request(base_url + '/releases.json',
                                   headers={'User-Agent': 'compass-release'})
  with urllib.request.urlopen(request, timeout=30) as r:
    return json.loads(r.read().decode('utf-8'))


def win_installer_name(version: str) -> str:
  """The installer with a window (installer/win) that the website offers;
  CompassSetup-<version>.exe is Chromium's mini_installer, which the Windows
  updater runs."""
  return 'CompassInstaller-%s.exe' % version


def check_against_server(local: Dict, server: Dict) -> None:
  """Refuses |local| (the release machine's releases.json) when the
  server's has a release, or a platform of one, that it lacks: writing the
  feeds from it would take them off the server. That happened once, with
  two release machines' copies."""
  have = {r['version']: r for r in local['releases']}
  missing = []
  for r in server['releases']:
    mine = have.get(r['version'])
    for platform in ('mac', 'win'):
      if r.get(platform) and not (mine and mine.get(platform)):
        missing.append('%s (%s)' % (r['version'], platform))
  if missing:
    raise ReleaseError(
        'the local releases.json (%s) lacks releases the server has: %s. '
        'Start from the server\'s: curl -o "<that file>" %s/releases.json, '
        'then run this again.' % (SITE_DIR / 'releases.json',
                                  ', '.join(missing), BASE_URL))


def add_win_release(manifest: Dict, installer: Path, version: str,
                    chromium: str, channel: str, commit: str, tools: Path,
                    site: Path = SITE_DIR, win_build: int = 0,
                    windowed: Optional[Path] = None) -> Dict:
  """Adds a Windows installer (Chromium's mini_installer.exe, built on a
  Windows machine with compass_win_build = |win_build|) to release
  |version|, or makes that release. The Windows updater checks its size,
  SHA-256 and Ed25519 signature (browser/updates/update_manifest.cc) before
  running it."""
  if not re.fullmatch(r'\d+(\.\d+)*', version):
    raise ReleaseError('bad version %r' % version)
  if win_build < 1:
    raise ReleaseError('a Windows release needs its build number '
                       '(compass_win_build), from `compass release build-win`')
  if win_build <= max(win_builds(manifest), default=0):
    raise ReleaseError(
        'Windows build %d isn\'t newer than the last one (%d): Chromium\'s '
        'installer wouldn\'t update to it. Build again with '
        '`compass release build-win`.' % (win_build, max(win_builds(manifest))))
  win_dir = site / 'win'
  win_dir.mkdir(parents=True, exist_ok=True)
  archive = win_dir / ('CompassSetup-%s.exe' % version)
  shutil.copyfile(installer, archive)
  win = {
      'file': 'win/' + archive.name,
      'url': github_url(version, archive.name),
      'size': archive.stat().st_size,
      'sha256': sha256(archive),
      'ed_signature': ed_sign(tools / 'sign_update', archive),
      'build': win_build,
  }
  if windowed:
    copy = win_dir / win_installer_name(version)
    shutil.copyfile(windowed, copy)
    win['installer'] = {
        'file': 'win/' + copy.name,
        'url': github_url(version, copy.name),
        'size': copy.stat().st_size,
        'sha256': sha256(copy),
    }
  existing = [r for r in manifest['releases'] if r['version'] == version]
  if existing:
    release = existing[0]
    if release.get('win'):
      raise ReleaseError('release %s has a Windows installer already' %
                         version)
    if chromium and chromium != release['chromium']:
      raise ReleaseError('release %s is Chromium %s, not %s' %
                         (version, release['chromium'], chromium))
  else:
    if not chromium:
      raise ReleaseError('a new release needs its Chromium version')
    release = {
        'version': version,
        'channel': channel,
        'commit': commit,
        'chromium': chromium,
        'date': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'notes': '',
    }
    manifest['releases'].append(release)
  release['win'] = win
  return release


def make_deltas(manifest: Dict, archive: Path, version: str, tools: Path,
                site: Path) -> List[Dict]:
  earlier = sorted((r for r in manifest['releases']
                    if r.get('mac') and (site / r['mac']['file']).is_file()),
                   key=lambda r: version_key(r['version']))[-DELTA_COUNT:]
  deltas = []
  if not earlier:
    return deltas
  with MountedApp(archive) as new_app:
    for release in earlier:
      delta = site / 'mac' / ('Compass-%s-%s.delta' %
                              (release['version'], version))
      with MountedApp(site / release['mac']['file']) as old_app:
        print('delta %s -> %s' % (release['version'], version))
        subprocess.run([str(tools / 'BinaryDelta'), 'create', str(old_app),
                        str(new_app), str(delta)], check=True)
      deltas.append({
          'from': release['version'],
          'file': 'mac/' + delta.name,
          'url': github_url(version, delta.name),
          'size': delta.stat().st_size,
          'ed_signature': ed_sign(tools / 'sign_update', delta),
      })
  return deltas


# Upload --------------------------------------------------------------------


def upload_target(config: Path = UPLOAD_CONFIG) -> tuple:
  """(host, ssh command) from |config|."""
  try:
    data = json.loads(config.read_text())
  except FileNotFoundError:
    raise ReleaseError('no upload target: write %s, e.g. {"host": '
                       '"user@server", "port": 22, "key": "~/.ssh/key"}' %
                       config)
  except ValueError as e:
    raise ReleaseError('%s: %s' % (config, e))
  host = data.get('host')
  if not isinstance(host, str) or not host:
    raise ReleaseError('%s: no "host"' % config)
  ssh = ['ssh', '-p', str(int(data.get('port', 22)))]
  if data.get('key'):
    ssh += ['-i', str(Path(data['key']).expanduser()), '-o',
            'IdentityAgent=none', '-o', 'IdentitiesOnly=yes']
  return host, ' '.join(ssh)


def upload(paths: List[Path], site: Path = SITE_DIR) -> None:
  """rsyncs |paths| (inside |site|) to the update server, in order."""
  host, ssh = upload_target()
  # One rsync per folder, in order: macOS's openrsync can't send --relative
  # through rrsync.
  for path in paths:
    folder = path.parent.relative_to(site).as_posix()
    dest = '' if folder == '.' else folder + '/'
    subprocess.run(['rsync', '-t', '--partial', '-e', ssh,
                    str(path.relative_to(site)), host + ':' + dest],
                   cwd=site, check=True)


def release_files(release: Dict) -> List[Path]:
  mac = release.get('mac') or {}
  files = [mac['file']] if mac else []
  files += [d['file'] for d in mac.get('deltas', [])]
  return [SITE_DIR / f for f in files] + win_release_files(release)


def win_release_files(release: Dict) -> List[Path]:
  """|release|'s Windows files in the site mirror: mini_installer, and the
  installer with a window when there is one."""
  win = release.get('win') or {}
  files = [win['file']] if win else []
  if win.get('installer'):
    files.append(win['installer']['file'])
  return [SITE_DIR / f for f in files]


# GitHub releases -----------------------------------------------------------


def _gh(args: List[str], check: bool = True) -> subprocess.CompletedProcess:
  try:
    return subprocess.run(['gh'] + args + ['--repo', GITHUB_REPO],
                          check=check, capture_output=not check, text=True)
  except FileNotFoundError:
    raise ReleaseError('needs the GitHub CLI (gh), signed in with write '
                       'access to %s' % GITHUB_REPO)
  except subprocess.CalledProcessError as e:
    raise ReleaseError('gh %s failed (%d)' % (' '.join(args[:2]),
                                               e.returncode))


def github_commit(commit: str) -> Optional[str]:
  """|commit|'s full hash if GitHub has it, else None. A release's target
  must be a full hash or a branch; build sidecars record short ones."""
  done = subprocess.run(
      ['gh', 'api', 'repos/%s/commits/%s' % (GITHUB_REPO, commit), '--jq',
       '.sha'], capture_output=True, text=True)
  sha = done.stdout.strip()
  return sha if done.returncode == 0 and len(sha) == 40 else None


def github_release_args(release: Dict, target: Optional[str]) -> List[str]:
  """`gh release create` arguments for a release that isn't on GitHub yet."""
  notes = 'Compass %s, based on Chromium %s.' % (release['version'],
                                                 release['chromium'])
  if release.get('notes'):
    notes += '\n\n' + release['notes']
  args = ['release', 'create', github_tag(release['version']), '--title',
          'Compass %s' % release['version'], '--notes', notes]
  args.append('--latest' if release['channel'] == 'stable' else '--prerelease')
  if target:
    args += ['--target', target]
  return args


def publish_assets(release: Dict, paths: List[Path], commit: str = '') -> None:
  """Uploads |paths| as assets of |release|'s GitHub release, creating it
  first (tagging |commit| when GitHub has it; the default branch
  otherwise)."""
  tag = github_tag(release['version'])
  if _gh(['release', 'view', tag], check=False).returncode != 0:
    target = github_commit(commit) if commit else None
    _gh(github_release_args(release, target))
  _gh(['release', 'upload', tag, '--clobber'] + [str(p) for p in paths])


def promote_on_github(release: Dict) -> None:
  """A release promoted to stable stops being a prerelease and becomes the
  latest release (if its archives are on GitHub). Canary to beta changes
  nothing there: both are prereleases."""
  if release['channel'] != 'stable':
    return
  archives = [release.get('mac') or {}, release.get('win') or {}]
  if not any(a.get('url') for a in archives):
    return
  _gh(['release', 'edit', github_tag(release['version']),
       '--prerelease=false', '--latest'])
