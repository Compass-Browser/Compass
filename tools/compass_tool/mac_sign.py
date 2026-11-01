# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Developer ID signing and notarization of a macOS build (`compass sign`).

Uses Chromium's own signing driver, which the //chrome/installer/mac target
copies into "<out>/<Product> Packaging": it signs every helper, framework
and dylib with the right entitlements and options, builds the .dmg, and with
--notarize submits the app and the .dmg with `xcrun notarytool`, waits, and
staples the tickets.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import List, Optional

PACKAGING_TARGET = 'chrome/installer/mac'
DEFAULT_NOTARY_PROFILE = 'compass'
IDENTITY_KIND = 'Developer ID Application'

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
