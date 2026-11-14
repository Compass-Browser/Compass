# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Compass for Android on Google Play (docs/building.md, "Android releases
(Google Play)"): the versionCode of a release, and the upload of its app
bundle to a testing track through the Play Developer Publishing API.

Only the standard library: the service account's token request (a JWT,
RS256) is signed with `openssl`, as testflight.py signs App Store Connect's.
The upload is one edit: edits.insert, edits.bundles.upload,
edits.tracks.update, edits.commit (or edits.validate and edits.delete for a
check that changes nothing).
"""

from __future__ import annotations

import base64
import json
import os
import re
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Callable, Dict, List, Optional

from .process import CommandError

PACKAGE = 'it.cipi1965.compass'
SCOPE = 'https://www.googleapis.com/auth/androidpublisher'
API = 'https://androidpublisher.googleapis.com/androidpublisher/v3/applications/'
UPLOAD_API = ('https://androidpublisher.googleapis.com/upload/androidpublisher/'
              'v3/applications/')
DEFAULT_TOKEN_URI = 'https://oauth2.googleapis.com/token'
# The tracks Play has for every app: internal testing, closed testing
# ("alpha"), open testing ("beta") and production. Closed testing can have
# more tracks, under names of their own.
STANDARD_TRACKS = ('internal', 'alpha', 'beta', 'production')
# A release's status. An app that has never been published only takes
# draft releases.
STATUSES = ('completed', 'draft', 'halted', 'inProgress')
# Play's limit on release notes, per language.
NOTES_LIMIT = 500
NOTES_FILE = re.compile(r'^whatsnew-([A-Za-z]{2,3}(?:-[A-Za-z0-9]+)*)\.txt$')
# Google's limit; versionCode is a 32-bit int.
MAX_VERSION_CODE = 2100000000


# versionCode ---------------------------------------------------------------


def version_code(version: str) -> int:
  """The versionCode of release |version| (YYYYMMDD.HHMM, release.py's
  new_version()): 1, then the year's last two digits, the day of the year
  (3 digits), the hour and the minute. 20261114.1530 is 1263181530.

  It grows with the version within 2000-2099 (a later year adds 10^7, more
  than any day and time), stays under Play's limit (2099's last minute is
  1993652359), and stays above Chromium's own codes for its 157 builds
  (about 8.1e8), which the first Android APKs carried: an APK from GitHub
  updates to the next one either way."""
  m = re.fullmatch(r'(\d{4})(\d{2})(\d{2})\.(\d{2})(\d{2})', version)
  if not m:
    raise CommandError('version %r is not YYYYMMDD.HHMM' % version)
  year, month, day, hour, minute = (int(g) for g in m.groups())
  if not 2000 <= year <= 2099:
    raise CommandError('version %s: the versionCode scheme covers the years '
                       '2000 to 2099' % version)
  try:
    t = time.strptime('%04d%02d%02d' % (year, month, day), '%Y%m%d')
  except ValueError:
    raise CommandError('version %s is not a real date' % version)
  if hour > 23 or minute > 59:
    raise CommandError('version %s is not a real time' % version)
  code = (1000000000 + (year - 2000) * 10000000 + t.tm_yday * 10000 +
          hour * 100 + minute)
  assert code < MAX_VERSION_CODE
  return code


# Release notes and the track's release ---------------------------------------


def release_notes(notes_dir: Path) -> List[Dict[str, str]]:
  """"What's new" from |notes_dir|'s whatsnew-<language>.txt files
  (android/play/whatsnew-en-US.txt), sorted by language. Play takes 500
  characters per language."""
  notes = []
  for path in sorted(notes_dir.glob('whatsnew-*.txt')):
    m = NOTES_FILE.match(path.name)
    if not m:
      raise CommandError('%s: name it whatsnew-<language>.txt, e.g. '
                         'whatsnew-en-US.txt' % path)
    text = path.read_text(encoding='utf-8').strip()
    if not text:
      raise CommandError('%s is empty' % path)
    if len(text) > NOTES_LIMIT:
      raise CommandError('%s has %d characters; Play takes %d' %
                         (path, len(text), NOTES_LIMIT))
    notes.append({'language': m.group(1), 'text': text})
  if not notes:
    raise CommandError('no whatsnew-<language>.txt in %s' % notes_dir)
  return notes


def track_release(track: str, name: str, code: int, status: str,
                  notes: List[Dict[str, str]],
                  user_fraction: Optional[float] = None) -> dict:
  """The body of edits.tracks.update: |track| with one release, |code|,
  named |name| (the Compass version: Play Console lists releases by it)."""
  if status not in STATUSES:
    raise CommandError('status %r: one of %s' % (status, ', '.join(STATUSES)))
  if (status == 'inProgress') != (user_fraction is not None):
    raise CommandError('a staged rollout (inProgress) needs a user fraction, '
                       'and only it takes one')
  release = {
      'name': name,
      'versionCodes': [str(code)],
      'status': status,
      'releaseNotes': notes,
  }
  if user_fraction is not None:
    if not 0 < user_fraction < 1:
      raise CommandError('user fraction %s: between 0 and 1' % user_fraction)
    release['userFraction'] = user_fraction
  return {'track': track, 'releases': [release]}


# Credentials -----------------------------------------------------------------


def load_credentials(path: Path) -> dict:
  """A service account's JSON key, as Google Cloud's console downloads it."""
  if not path.is_file():
    raise CommandError(
        'no Play service account key at %s. Create one (docs/building.md, '
        '"Android releases (Google Play)") and put its JSON there.' % path)
  try:
    info = json.loads(path.read_text())
  except ValueError as e:
    raise CommandError('%s is not JSON: %s' % (path, e))
  if info.get('type') != 'service_account':
    raise CommandError('%s is not a service account key (type is %r)' %
                       (path, info.get('type')))
  for key in ('client_email', 'private_key'):
    if not info.get(key):
      raise CommandError('%s has no %s' % (path, key))
  return info


def _b64url(data: bytes) -> str:
  return base64.urlsafe_b64encode(data).rstrip(b'=').decode()


def jwt_signing_input(info: dict, now: Optional[float] = None) -> str:
  """The header and claims of the token request's assertion (RFC 7523), an
  hour long, for the androidpublisher scope."""
  issued = int(time.time() if now is None else now)
  header = {'alg': 'RS256', 'typ': 'JWT'}
  if info.get('private_key_id'):
    header['kid'] = info['private_key_id']
  claims = {
      'iss': info['client_email'],
      'scope': SCOPE,
      'aud': info.get('token_uri') or DEFAULT_TOKEN_URI,
      'iat': issued,
      'exp': issued + 3600,
  }
  return (_b64url(json.dumps(header, separators=(',', ':')).encode()) + '.' +
          _b64url(json.dumps(claims, separators=(',', ':')).encode()))


def sign_rs256(private_key_pem: str, data: str) -> bytes:
  """RSASSA-PKCS1-v1_5 with SHA-256, by `openssl dgst` (LibreSSL's too). The
  key goes through a file only the user can read (mkstemp makes it so),
  removed right after."""
  fd, key_path = tempfile.mkstemp(suffix='.pem')
  try:
    with os.fdopen(fd, 'w') as f:
      f.write(private_key_pem)
    result = subprocess.run(['openssl', 'dgst', '-sha256', '-sign', key_path],
                            input=data.encode(), capture_output=True)
  finally:
    os.unlink(key_path)
  if result.returncode != 0:
    raise CommandError('openssl could not sign the token request: %s' %
                       result.stderr.decode(errors='replace').strip())
  return result.stdout


def make_assertion(info: dict, now: Optional[float] = None,
                   sign: Callable[[str, str], bytes] = sign_rs256) -> str:
  signing_input = jwt_signing_input(info, now)
  return signing_input + '.' + _b64url(sign(info['private_key'], signing_input))


def token_request_body(assertion: str) -> bytes:
  return urllib.parse.urlencode({
      'grant_type': 'urn:ietf:params:oauth:grant-type:jwt-bearer',
      'assertion': assertion,
  }).encode()


# The API ---------------------------------------------------------------------


class ApiError(CommandError):

  def __init__(self, status: int, method: str, url: str, body: str):
    message = body
    try:
      message = json.loads(body)['error']['message']
    except (ValueError, KeyError, TypeError):
      pass
    super().__init__('%s %s: HTTP %d: %s' % (method, url, status, message))
    self.status = status
    self.message = message


def explain(error: ApiError) -> str:
  """A hint for the errors a first upload runs into."""
  text = error.message.lower()
  if 'draft app' in text:
    return ('The app has never been published, so Play only takes draft '
            'releases: run again with status "draft" (PLAY_STATUS=draft), '
            'then roll the release out in Play Console.')
  if 'version code' in text:
    return ('Play has this versionCode already: build a new release (a new '
            'version, a new versionCode).')
  if 'package not found' in text or error.status == 404:
    return ('Play has no app %s that this service account can see: create '
            'the app in Play Console, upload its first bundle there, and '
            'invite the service account under Users and permissions.' %
            PACKAGE)
  if error.status in (401, 403):
    return ('The service account lacks access: in Play Console, Users and '
            'permissions, give it "Release to testing tracks" for the app.')
  return ''


class Client:
  """The edits API for one app. |transport|(method, url, body, headers,
  timeout) returns the response's JSON; tests pass a fake."""

  def __init__(self, token: Callable[[], str], package: str = PACKAGE,
               transport: Optional[Callable] = None):
    self._token = token
    self.package = package
    self._transport = transport or _urllib

  def _call(self, method: str, url: str, body=None, content_type=None,
            timeout: int = 120):
    headers = {'Authorization': 'Bearer ' + self._token()}
    if content_type:
      headers['Content-Type'] = content_type
    return self._transport(method, url, body, headers, timeout)

  def _edit_url(self, edit: str, rest: str = '') -> str:
    return '%s%s/edits/%s%s' % (API, self.package, edit, rest)

  def insert_edit(self) -> str:
    return self._call('POST', '%s%s/edits' % (API, self.package),
                      b'{}', 'application/json')['id']

  def upload_bundle(self, edit: str, aab: Path) -> int:
    url = '%s%s/edits/%s/bundles?uploadType=media' % (UPLOAD_API,
                                                      self.package, edit)
    # A large bundle takes a while on a slow uplink.
    response = self._call('POST', url, aab, 'application/octet-stream',
                          timeout=1800)
    return int(response['versionCode'])

  def update_track(self, edit: str, body: dict) -> dict:
    return self._call('PUT', self._edit_url(edit, '/tracks/' +
                                            urllib.parse.quote(body['track'])),
                      json.dumps(body).encode(), 'application/json')

  def validate(self, edit: str) -> None:
    self._call('POST', self._edit_url(edit, ':validate'))

  def commit(self, edit: str, not_sent_for_review: bool = False) -> None:
    query = '?changesNotSentForReview=true' if not_sent_for_review else ''
    self._call('POST', self._edit_url(edit, ':commit' + query))

  def delete_edit(self, edit: str) -> None:
    self._call('DELETE', self._edit_url(edit))


def _urllib(method: str, url: str, body, headers: dict, timeout: int):
  data = body
  if isinstance(body, Path):
    headers = dict(headers, **{'Content-Length': str(body.stat().st_size)})
    data = open(body, 'rb')
  request = urllib.request.Request(url, data=data, method=method,
                                   headers=headers)
  try:
    with urllib.request.urlopen(request, timeout=timeout) as response:
      text = response.read().decode()
  except urllib.error.HTTPError as e:
    raise ApiError(e.code, method, url.split('?')[0],
                   e.read().decode(errors='replace'))
  finally:
    if isinstance(body, Path):
      data.close()
  return json.loads(text) if text.strip() else {}


def service_account_token(info: dict) -> str:
  """An access token for |info|'s service account (OAuth 2.0 JWT bearer)."""
  url = info.get('token_uri') or DEFAULT_TOKEN_URI
  request = urllib.request.Request(
      url, data=token_request_body(make_assertion(info)), method='POST',
      headers={'Content-Type': 'application/x-www-form-urlencoded'})
  try:
    with urllib.request.urlopen(request, timeout=60) as response:
      return json.loads(response.read().decode())['access_token']
  except urllib.error.HTTPError as e:
    raise ApiError(e.code, 'POST', url, e.read().decode(errors='replace'))


def upload(client: Client, aab: Path, body: dict, commit: bool = True,
           not_sent_for_review: bool = False,
           log: Callable[[str], None] = print) -> int:
  """Uploads |aab| and puts it on the track |body| describes, in one edit.
  Without |commit| the edit is only validated, then thrown away. Returns
  the versionCode Play read from the bundle."""
  expected = int(body['releases'][0]['versionCodes'][0])
  edit = client.insert_edit()
  log('edit %s' % edit)
  done = False
  try:
    log('uploading %s (%d MB)' % (aab.name, aab.stat().st_size >> 20))
    code = client.upload_bundle(edit, aab)
    if code != expected:
      raise CommandError('Play read versionCode %d from %s, expected %d' %
                         (code, aab, expected))
    log('versionCode %d uploaded' % code)
    client.update_track(edit, body)
    log('track %s: release %s (%s)' % (body['track'],
                                       body['releases'][0]['name'],
                                       body['releases'][0]['status']))
    if commit:
      client.commit(edit, not_sent_for_review)
      done = True
      log('committed')
    else:
      client.validate(edit)
      log('validated; nothing committed')
  finally:
    if not done:
      try:
        client.delete_edit(edit)
      except CommandError:
        pass
  return code


def describe(package: str, aab: Path, body: dict) -> List[str]:
  """What an upload would do, for a dry run."""
  return [
      'POST %s%s/edits' % (API, package),
      'POST %s%s/edits/<edit>/bundles?uploadType=media  < %s' % (
          UPLOAD_API, package, aab),
      'PUT  %s%s/edits/<edit>/tracks/%s' % (API, package, body['track']),
      json.dumps(body, indent=2, ensure_ascii=False),
      'POST %s%s/edits/<edit>:commit' % (API, package),
  ]
