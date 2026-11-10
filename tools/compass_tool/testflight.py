# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""TestFlight external testing through the App Store Connect API.

`compass ios external` takes an uploaded build (the newest, or one by its
build number), waits for App Store Connect to process it, sets its "What to
Test", adds it to an external beta group and submits it for Beta App Review.
Only the standard library: the API token (ES256) is signed with `openssl`, so
build Macs need no Python packages.
"""

from __future__ import annotations

import base64
import json
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Callable, Dict, Optional, Union

from .process import CommandError

API = 'https://api.appstoreconnect.apple.com'
LOCALE = 'en-US'


def _b64url(data: bytes) -> str:
  return base64.urlsafe_b64encode(data).rstrip(b'=').decode()


def der_to_raw_signature(der: bytes, size: int = 32) -> bytes:
  """An ECDSA signature as openssl writes it (DER SEQUENCE of r, s) in the
  JOSE form ES256 needs: r and s, each `size` bytes, big endian."""
  def read_len(i: int):
    first = der[i]
    if first < 0x80:
      return first, i + 1
    n = first & 0x7f
    return int.from_bytes(der[i + 1:i + 1 + n], 'big'), i + 1 + n

  if not der or der[0] != 0x30:
    raise ValueError('not a DER sequence')
  _, i = read_len(1)
  out = b''
  for _ in range(2):
    if der[i] != 0x02:
      raise ValueError('not a DER integer')
    length, i = read_len(i + 1)
    value = der[i:i + length].lstrip(b'\0')
    if len(value) > size:
      raise ValueError('integer too long')
    out += value.rjust(size, b'\0')
    i += length
  return out


def make_token(key_path: Path, key_id: str, issuer: str,
               now: Optional[float] = None) -> str:
  """An App Store Connect API token, valid for 15 minutes."""
  now = time.time() if now is None else now
  header = {'alg': 'ES256', 'kid': key_id, 'typ': 'JWT'}
  claims = {'iss': issuer, 'iat': int(now), 'exp': int(now) + 15 * 60,
            'aud': 'appstoreconnect-v1'}
  signing_input = (_b64url(json.dumps(header, separators=(',', ':')).encode())
                   + '.' +
                   _b64url(json.dumps(claims, separators=(',', ':')).encode()))
  result = subprocess.run(
      ['openssl', 'dgst', '-sha256', '-sign', str(key_path)],
      input=signing_input.encode(), capture_output=True)
  if result.returncode != 0:
    raise CommandError('openssl could not sign with %s: %s' %
                       (key_path, result.stderr.decode().strip()))
  return signing_input + '.' + _b64url(der_to_raw_signature(result.stdout))


class ApiError(CommandError):

  def __init__(self, status: int, method: str, path: str, body: str):
    self.status = status
    detail = body
    try:
      errors = json.loads(body).get('errors', [])
      detail = '; '.join('%s: %s' % (e.get('title', ''), e.get('detail', ''))
                         for e in errors) or body
    except ValueError:
      pass
    super().__init__('App Store Connect: %s %s: HTTP %d: %s' %
                     (method, path, status, detail))


Transport = Callable[[str, str, Optional[dict]], Optional[dict]]


class Client:
  """The few App Store Connect calls external testing needs."""

  def __init__(self, token: Callable[[], str],
               transport: Optional[Transport] = None):
    self._token = token
    self._transport = transport or self._urllib

  def _urllib(self, method: str, path: str, body: Optional[dict]):
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(API + path, data=data, method=method)
    request.add_header('Authorization', 'Bearer ' + self._token())
    if data is not None:
      request.add_header('Content-Type', 'application/json')
    try:
      with urllib.request.urlopen(request, timeout=60) as response:
        raw = response.read()
    except urllib.error.HTTPError as e:
      raise ApiError(e.code, method, path, e.read().decode(errors='replace'))
    return json.loads(raw) if raw else None

  def call(self, method: str, path: str, body: Optional[dict] = None):
    return self._transport(method, path, body)

  def get(self, path: str, **query) -> list:
    if query:
      path += '?' + urllib.parse.urlencode(query)
    return (self.call('GET', path) or {}).get('data', [])

  # The steps, each one idempotent.

  def app_id(self, bundle_id: str) -> str:
    apps = self.get('/v1/apps', **{'filter[bundleId]': bundle_id})
    if not apps:
      raise CommandError('no app %s in App Store Connect' % bundle_id)
    return apps[0]['id']

  def find_build(self, app: str, number: Optional[str]) -> Optional[dict]:
    query = {'filter[app]': app, 'sort': '-uploadedDate', 'limit': '1'}
    if number:
      query['filter[version]'] = number
    builds = self.get('/v1/builds', **query)
    return builds[0] if builds else None

  def external_group(self, app: str, name: Optional[str]) -> dict:
    groups = [g for g in self.get('/v1/apps/%s/betaGroups' % app, limit='200')
              if not g['attributes'].get('isInternalGroup')]
    names = ', '.join(g['attributes']['name'] for g in groups) or 'none'
    if name:
      for g in groups:
        if g['attributes']['name'] == name:
          return g
      raise CommandError('no external group "%s" (external groups: %s)' %
                         (name, names))
    if len(groups) != 1:
      raise CommandError('pass --group: external groups: %s' % names)
    return groups[0]

  def set_what_to_test(self, build: str, text: str,
                       locale: str = LOCALE) -> None:
    localizations = self.get('/v1/builds/%s/betaBuildLocalizations' % build)
    for loc in localizations:
      if loc['attributes'].get('locale') == locale:
        self.call('PATCH', '/v1/betaBuildLocalizations/' + loc['id'], {
            'data': {'type': 'betaBuildLocalizations', 'id': loc['id'],
                     'attributes': {'whatsNew': text}}})
        return
    self.call('POST', '/v1/betaBuildLocalizations', {
        'data': {'type': 'betaBuildLocalizations',
                 'attributes': {'locale': locale, 'whatsNew': text},
                 'relationships': {'build': {'data': {
                     'type': 'builds', 'id': build}}}}})

  def add_to_group(self, group: str, build: str) -> None:
    self.call('POST', '/v1/betaGroups/%s/relationships/builds' % group,
              {'data': [{'type': 'builds', 'id': build}]})

  def review_state(self, build: str) -> Optional[str]:
    try:
      result = self.call(
          'GET', '/v1/builds/%s/betaAppReviewSubmission' % build)
    except ApiError as e:
      if e.status == 404:
        return None
      raise
    data = (result or {}).get('data')
    return data['attributes'].get('betaReviewState') if data else None

  def submit_for_review(self, build: str) -> None:
    self.call('POST', '/v1/betaAppReviewSubmissions', {
        'data': {'type': 'betaAppReviewSubmissions',
                 'relationships': {'build': {'data': {
                     'type': 'builds', 'id': build}}}}})


def submit_external(client: Client, bundle_id: str, build_number: Optional[str],
                    group_name: Optional[str],
                    what_to_test: Union[str, Dict[str, str]],
                    wait: float = 30 * 60, poll: float = 30,
                    sleep: Callable[[float], None] = time.sleep,
                    log: Callable[[str], None] = print) -> str:
  """Sends a build to external testers; returns its build number.
  `what_to_test` is the en-US text, or the text per App Store Connect
  locale ({'en-US': ..., 'it': ...})."""
  if isinstance(what_to_test, str):
    what_to_test = {LOCALE: what_to_test}
  app = client.app_id(bundle_id)
  group = client.external_group(app, group_name)
  deadline = time.monotonic() + wait
  while True:
    build = client.find_build(app, build_number)
    state = build['attributes'].get('processingState') if build else None
    if state == 'VALID':
      break
    if state in ('FAILED', 'INVALID'):
      raise CommandError('build %s is %s in App Store Connect' %
                         (build['attributes'].get('version'), state))
    if time.monotonic() >= deadline:
      raise CommandError('build %s still %s after %d min' %
                         (build_number or '(newest)', state or 'not there',
                          wait // 60))
    log('waiting for build %s: %s' % (build_number or '(newest)',
                                      state or 'not there yet'))
    sleep(poll)
  build_id = build['id']
  number = build['attributes'].get('version', build_number or '?')
  if build['attributes'].get('usesNonExemptEncryption') is None:
    log('note: build %s has no export compliance answer; review may wait for '
        'it (ITSAppUsesNonExemptEncryption in Info.plist)' % number)
  for locale, text in what_to_test.items():
    client.set_what_to_test(build_id, text, locale)
  client.add_to_group(group['id'], build_id)
  log('build %s: in external group "%s"' % (number, group['attributes']['name']))
  state = client.review_state(build_id)
  if state:
    log('build %s: already submitted for Beta App Review (%s)' % (number, state))
  else:
    client.submit_for_review(build_id)
    log('build %s: submitted for Beta App Review' % number)
  return number
