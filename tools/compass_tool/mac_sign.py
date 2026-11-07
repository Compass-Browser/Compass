# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Developer ID signing and notarization of a macOS build (`compass sign`).

Uses Chromium's own signing driver, which the //chrome/installer/mac target
copies into "<out>/<Product> Packaging": it signs every helper, framework
and dylib with the right entitlements and options, builds the .dmg, and with
--notarize submits the app and the .dmg with `xcrun notarytool`, waits, and
staples the tickets.

The app is also signed with com.apple.application-identifier, which needs a
Developer ID provisioning profile embedded in the bundle. macOS only hands
ASWebAuthenticationSession logins (other apps' "Sign in with Google", e.g.
Spark) to a default browser that has an application identifier: without one,
SafariLaunchAgent drops the connection and the login never opens. The
profile is not in the repository; it lives next to the release key
(PROVISIONING_PROFILE). Development and test builds, signed with a
self-signed identity, never get the entitlement: macOS refuses to launch an
app that has it without a matching profile.
"""

from __future__ import annotations

import datetime
import plistlib
import re
import shutil
import subprocess
from pathlib import Path
from typing import Callable, List, Optional

from .release import RELEASE_DIR

PACKAGING_TARGET = 'chrome/installer/mac'
DEFAULT_NOTARY_PROFILE = 'compass'
IDENTITY_KIND = 'Developer ID Application'

PROVISIONING_PROFILE = RELEASE_DIR / 'Compass.provisionprofile'
APP_ENTITLEMENTS = 'app-entitlements.plist'
EMBEDDED_PROFILE = 'embedded.provisionprofile'
ENT_APP_ID = 'com.apple.application-identifier'

_IDENTITY_LINE = re.compile(r'^\s*\d+\)\s+([0-9A-F]{40})\s+"(.+)"\s*$')


def developer_id_identities(find_identity_output: str) -> List[tuple]:
  """(hash, name) of each Developer ID Application identity listed by
  `security find-identity -v -p codesigning`, in listed order."""
  found = []
  for line in find_identity_output.splitlines():
    match = _IDENTITY_LINE.match(line)
    if match and match.group(2).startswith(IDENTITY_KIND + ':'):
      found.append((match.group(1), match.group(2)))
  return found


def pick_identity(find_identity_output: str,
                  wanted: Optional[str] = None) -> tuple:
  """The identity to sign with: |wanted| (a hash, or part of a name) or the
  only Developer ID Application identity. Raises ValueError otherwise."""
  identities = developer_id_identities(find_identity_output)
  if wanted:
    matches = [(h, n) for h, n in identities
               if h == wanted.upper() or wanted in n]
    if len(matches) == 1:
      return matches[0]
    raise ValueError('%s Developer ID identities match "%s"' %
                     ('No' if not matches else 'Several', wanted))
  if len(identities) == 1:
    return identities[0]
  if not identities:
    raise ValueError('no "%s" certificate in the keychain' % IDENTITY_KIND)
  raise ValueError('several Developer ID identities; pick one with '
                   '--identity: ' + ', '.join(n for _, n in identities))


def packaging_dir(out_dir: Path, product: str) -> Path:
  return out_dir / (product + ' Packaging')


def sign_command(python: str, out_dir: Path, product: str, output: Path,
                 identity: str, notary_profile: Optional[str]) -> List[str]:
  """The signing driver's command line. With |notary_profile| (a
  `notarytool store-credentials` keychain profile) it notarizes and
  staples."""
  cmd = [python, packaging_dir(out_dir, product) / 'sign_chrome.py',
         '--input', out_dir, '--output', output, '--identity', identity]
  if notary_profile:
    cmd += ['--notarize', 'staple',
            '--notary-arg=--keychain-profile',
            '--notary-arg=' + notary_profile]
  return [str(c) for c in cmd]


def store_credentials_hint(profile: str, team_id: Optional[str]) -> str:
  team = team_id or '<TEAM_ID>'
  return ('no notarytool credentials in the keychain profile "%s". Store '
          'them once, with an app-specific password from '
          'account.apple.com:\n'
          '  xcrun notarytool store-credentials %s --apple-id <APPLE_ID> '
          '--team-id %s\n'
          'or with an App Store Connect API key:\n'
          '  xcrun notarytool store-credentials %s --key <AuthKey_ID.p8> '
          '--key-id <KEY_ID> --issuer <ISSUER_ID>' %
          (profile, profile, team, profile))


def team_id(identity_name: str) -> Optional[str]:
  """The team ID in parentheses at the end of an identity's name."""
  match = re.search(r'\(([A-Z0-9]{10})\)\s*$', identity_name)
  return match.group(1) if match else None


class ProfileError(Exception):
  pass


def decode_profile(path: Path) -> dict:
  """The plist inside a .provisionprofile (a CMS-signed message)."""
  result = subprocess.run(['security', 'cms', '-D', '-i', str(path)],
                          capture_output=True, check=False)
  if result.returncode != 0:
    raise ProfileError('%s is not a provisioning profile: %s' %
                       (path, result.stderr.decode(errors='replace').strip()))
  return plistlib.loads(result.stdout)


def check_profile(profile: dict, app_id: str,
                  now: Optional[datetime.datetime] = None) -> None:
  """Raises ProfileError unless |profile| is a Developer ID profile, valid
  now, for |app_id| (<team>.<bundle id>)."""
  name = profile.get('Name', '?')
  found = profile.get('Entitlements', {}).get(ENT_APP_ID)
  if found != app_id:
    raise ProfileError('provisioning profile "%s" is for %s, not %s' %
                       (name, found, app_id))
  if not profile.get('ProvisionsAllDevices'):
    raise ProfileError('provisioning profile "%s" is not a Developer ID '
                       'profile (it lists devices)' % name)
  expires = profile.get('ExpirationDate')
  now = now or datetime.datetime.now(datetime.timezone.utc)
  if expires is not None:
    if expires.tzinfo is None:
      expires = expires.replace(tzinfo=datetime.timezone.utc)
    if expires <= now:
      raise ProfileError('provisioning profile "%s" expired on %s' %
                         (name, expires.date()))


def bundle_id(app: Path) -> str:
  with open(app / 'Contents' / 'Info.plist', 'rb') as f:
    return plistlib.load(f)['CFBundleIdentifier']


def prepare_app_identifier(
    app: Path, packaging: Path, team: Optional[str],
    profile_path: Optional[Path],
    decode: Callable[[Path], dict] = decode_profile) -> Optional[str]:
  """Before the signing driver runs: with |profile_path|, embeds it in |app|
  and adds the application identifier to the outer app's entitlements in
  |packaging|; returns the identifier. Without, takes both out (a previous
  run's may still be there) and returns None."""
  embedded = app / 'Contents' / EMBEDDED_PROFILE
  entitlements_path = packaging / APP_ENTITLEMENTS
  with open(entitlements_path, 'rb') as f:
    entitlements = plistlib.load(f)
  app_id = None
  if profile_path:
    if not team:
      raise ProfileError('no team ID in the signing identity\'s name')
    app_id = '%s.%s' % (team, bundle_id(app))
    check_profile(decode(profile_path), app_id)
    shutil.copyfile(profile_path, embedded)
    entitlements[ENT_APP_ID] = app_id
  else:
    if embedded.exists():
      embedded.unlink()
    entitlements.pop(ENT_APP_ID, None)
  with open(entitlements_path, 'wb') as f:
    plistlib.dump(entitlements, f)
  return app_id
