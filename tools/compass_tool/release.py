# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Releases and update feeds (`compass release`, docs/design/updates.md).

The release folder (~/Library/Application Support/Compass Release) keeps:

- sparkle_ed25519.key: Sparkle's private key (never in the repository);
- site/: a mirror of https://updates.compassbrowser.org, the update server:
  - releases.json: every release, the source of truth for the rest;
  - stable.json, beta.json: the newest release on each channel;
  - mac/appcast.xml: Sparkle's feed, both channels;
  - mac/Compass-<version>.dmg and mac/Compass-<from>-<to>.delta.

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
from pathlib import Path
from typing import Dict, List, Optional
from xml.sax.saxutils import escape, quoteattr

BASE_URL = 'https://updates.compassbrowser.org'
CHANNELS = ('stable', 'beta')
SCHEMA = 1
# Deltas are made from this many earlier releases.
DELTA_COUNT = 3

RELEASE_DIR = Path.home() / 'Library' / 'Application Support' / \
    'Compass Release'
KEY_FILE = RELEASE_DIR / 'sparkle_ed25519.key'
SITE_DIR = RELEASE_DIR / 'site'

# The rsync-only account on the update server (rrsync, rooted at
# /var/www/compass-updates), with its own key: no 1Password needed.
UPLOAD_HOST = 'user@update-server'
UPLOAD_PORT = 22
UPLOAD_KEY = Path.home() / '.ssh' / 'compass_updates_deploy'


class ReleaseError(Exception):
  pass


# Versions ------------------------------------------------------------------


def new_version(now: Optional[float] = None) -> str:
  """Date.time, e.g. 20261103.0215, like the iOS build numbers."""
  return time.strftime('%Y%m%d.%H%M', time.localtime(now))


def version_key(version: str) -> tuple:
  return tuple(int(part) for part in version.split('.'))


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


def latest(manifest: Dict, channel: str, platform: str) -> Optional[Dict]:
  """The newest release a |channel| user gets on |platform|: beta users get
  stable releases too."""
  eligible = [
      r for r in manifest['releases']
      if r.get(platform) and (channel == 'beta' or r['channel'] == 'stable')
  ]
  return max(eligible, key=lambda r: version_key(r['version']), default=None)


def channel_manifest(manifest: Dict, channel: str,
                     base_url: str = BASE_URL) -> Dict:
  """stable.json / beta.json: what to download, per platform."""
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
        'url': '%s/%s' % (base_url, archive['file']),
        'size': archive['size'],
        'sha256': archive['sha256'],
        'ed_signature': archive['ed_signature'],
    }
  return out


def appcast(manifest: Dict, base_url: str = BASE_URL) -> str:
  """Sparkle's feed. Beta releases carry <sparkle:channel>beta; promoted
  ones carry none, so every user sees them."""
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
                 (quoteattr('%s/%s' % (base_url, mac['file'])), mac['size'],
                  quoteattr(mac['ed_signature'])))
    if mac.get('deltas'):
      lines.append('      <sparkle:deltas>')
      for delta in mac['deltas']:
        lines.append(
            '        <enclosure url=%s sparkle:deltaFrom=%s length="%d" '
            'type="application/octet-stream" sparkle:edSignature=%s/>' %
            (quoteattr('%s/%s' % (base_url, delta['file'])),
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


def add_win_release(manifest: Dict, installer: Path, version: str,
                    chromium: str, channel: str, commit: str, tools: Path,
                    site: Path = SITE_DIR) -> Dict:
  """Adds a Windows installer (Chromium's mini_installer.exe, built on a
  Windows machine) to release |version|, or makes that release. The Windows
  updater checks its size, SHA-256 and Ed25519 signature
  (browser/updates/update_manifest.cc) before running it."""
  if not re.fullmatch(r'\d+(\.\d+)*', version):
    raise ReleaseError('bad version %r' % version)
  win_dir = site / 'win'
  win_dir.mkdir(parents=True, exist_ok=True)
  archive = win_dir / ('CompassSetup-%s.exe' % version)
  shutil.copyfile(installer, archive)
  win = {
      'file': 'win/' + archive.name,
      'size': archive.stat().st_size,
      'sha256': sha256(archive),
      'ed_signature': ed_sign(tools / 'sign_update', archive),
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
          'size': delta.stat().st_size,
          'ed_signature': ed_sign(tools / 'sign_update', delta),
      })
  return deltas


# Upload --------------------------------------------------------------------


def upload(paths: List[Path], site: Path = SITE_DIR) -> None:
  """rsyncs |paths| (inside |site|) to the update server, in order."""
  ssh = 'ssh -i %s -o IdentityAgent=none -o IdentitiesOnly=yes -p %d' % (
      UPLOAD_KEY, UPLOAD_PORT)
  relative = [str(p.relative_to(site)) for p in paths]
  subprocess.run(['rsync', '-t', '--relative', '--partial', '-e', ssh] +
                 relative + [UPLOAD_HOST + ':'],
                 cwd=site, check=True)


def release_files(release: Dict) -> List[Path]:
  mac = release.get('mac') or {}
  files = [mac['file']] if mac else []
  files += [d['file'] for d in mac.get('deltas', [])]
  win = release.get('win') or {}
  if win:
    files.append(win['file'])
  return [SITE_DIR / f for f in files]
