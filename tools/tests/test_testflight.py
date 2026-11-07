# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import base64
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compass_tool import testflight  # noqa: E402
from compass_tool.cli import parser  # noqa: E402
from compass_tool.process import CommandError  # noqa: E402


class FakeApi:
  """App Store Connect with one app, one internal and one external group."""

  def __init__(self, states=('PROCESSING', 'VALID'), localizations=(),
               review=None):
    self.calls = []
    self.paths = []
    self.states = list(states)
    self.localizations = list(localizations)
    self.review = review

  def __call__(self, method, path, body):
    self.calls.append((method, path.split('?')[0], body))
    self.paths.append(path)
    if path.startswith('/v1/apps?'):
      return {'data': [{'id': 'APP'}]}
    if path.startswith('/v1/apps/APP/betaGroups'):
      return {'data': [
          {'id': 'INT', 'attributes': {'name': 'Team', 'isInternalGroup': True}},
          {'id': 'EXT', 'attributes': {'name': 'Friends',
                                       'isInternalGroup': False}}]}
    if path.startswith('/v1/builds?'):
      state = self.states.pop(0) if len(self.states) > 1 else self.states[0]
      return {'data': [{'id': 'B1', 'attributes': {
          'version': '20261107.1556', 'processingState': state,
          'usesNonExemptEncryption': False}}]}
    if path.endswith('/betaBuildLocalizations') and method == 'GET':
      return {'data': self.localizations}
    if path.endswith('/betaAppReviewSubmission'):
      if self.review is None:
        raise testflight.ApiError(404, method, path, '{}')
      return {'data': {'attributes': {'betaReviewState': self.review}}}
    return None


def run(api, **kwargs):
  logs = []
  kwargs.setdefault('group_name', None)
  number = testflight.submit_external(
      testflight.Client(lambda: 't', api), 'it.cipi1965.compass',
      kwargs.pop('build_number', None), kwargs.pop('group_name'), 'Try pins',
      sleep=lambda s: None, log=logs.append, **kwargs)
  return number, logs


class SubmitExternalTest(unittest.TestCase):

  def test_waits_then_adds_and_submits(self):
    api = FakeApi()
    number, logs = run(api)
    self.assertEqual(number, '20261107.1556')
    writes = [(m, p, b) for m, p, b in api.calls if m != 'GET']
    self.assertEqual([(m, p) for m, p, _ in writes], [
        ('POST', '/v1/betaBuildLocalizations'),
        ('POST', '/v1/betaGroups/EXT/relationships/builds'),
        ('POST', '/v1/betaAppReviewSubmissions'),
    ])
    loc = writes[0][2]['data']
    self.assertEqual(loc['attributes'], {'locale': 'en-US',
                                         'whatsNew': 'Try pins'})
    self.assertEqual(loc['relationships']['build']['data']['id'], 'B1')
    self.assertIn('waiting for build (newest): PROCESSING', logs)

  def test_updates_existing_what_to_test_and_skips_a_second_submission(self):
    api = FakeApi(states=('VALID',), review='WAITING_FOR_REVIEW',
                  localizations=[{'id': 'L1', 'attributes': {'locale': 'en-US'}}])
    _, logs = run(api)
    writes = [(m, p) for m, p, _ in api.calls if m != 'GET']
    self.assertEqual(writes, [
        ('PATCH', '/v1/betaBuildLocalizations/L1'),
        ('POST', '/v1/betaGroups/EXT/relationships/builds'),
    ])
    self.assertTrue(any('already submitted' in line for line in logs))

  def test_group_by_name(self):
    with self.assertRaisesRegex(CommandError, 'external groups: Friends'):
      run(FakeApi(), group_name='Team')  # internal: not offered
    run(FakeApi(), group_name='Friends')

  def test_build_number_filters(self):
    api = FakeApi(states=('VALID',))
    run(api, build_number='20261107.1556')
    builds = [p for p in api.paths if p.startswith('/v1/builds?')]
    self.assertIn('filter%5Bversion%5D=20261107.1556', builds[0])

  def test_gives_up(self):
    with self.assertRaisesRegex(CommandError, 'still PROCESSING'):
      run(FakeApi(states=('PROCESSING',)), wait=0)

  def test_invalid_build(self):
    with self.assertRaisesRegex(CommandError, 'INVALID'):
      run(FakeApi(states=('INVALID',)))


class TokenTest(unittest.TestCase):

  def test_der_to_raw(self):
    r = b'\x80' + b'\x01' * 31  # high bit: DER adds a 0 byte
    s = b'\x00\x02' + b'\x03' * 30  # leading zero: DER drops it
    der = (b'\x30\x45' + b'\x02\x21\x00' + r + b'\x02\x1f' + s[1:])
    self.assertEqual(testflight.der_to_raw_signature(der), r + s)

  @unittest.skipUnless(shutil.which('openssl'), 'needs openssl')
  def test_token_verifies(self):
    with tempfile.TemporaryDirectory() as tmp:
      key = Path(tmp) / 'AuthKey.p8'
      pub = Path(tmp) / 'pub.pem'
      subprocess.run(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey',
                      '-noout', '-out', key], check=True)
      subprocess.run(['openssl', 'ec', '-in', key, '-pubout', '-out', pub],
                     check=True, capture_output=True)
      token = testflight.make_token(key, 'KID', 'ISS', now=1000)
      head, claims, sig = token.split('.')
      pad = lambda x: x + '=' * (-len(x) % 4)  # noqa: E731
      self.assertEqual(json.loads(base64.urlsafe_b64decode(pad(head))),
                       {'alg': 'ES256', 'kid': 'KID', 'typ': 'JWT'})
      self.assertEqual(json.loads(base64.urlsafe_b64decode(pad(claims)))['exp'],
                       1900)
      raw = base64.urlsafe_b64decode(pad(sig))
      self.assertEqual(len(raw), 64)
      # Back to DER, and openssl verifies it.
      def der_int(b):
        b = b.lstrip(b'\0')
        if b[0] & 0x80:
          b = b'\0' + b
        return b'\x02' + bytes([len(b)]) + b
      body = der_int(raw[:32]) + der_int(raw[32:])
      der = b'\x30' + bytes([len(body)]) + body
      sig_file = Path(tmp) / 'sig'
      sig_file.write_bytes(der)
      result = subprocess.run(
          ['openssl', 'dgst', '-sha256', '-verify', pub, '-signature',
           sig_file], input=(head + '.' + claims).encode(),
          capture_output=True)
      self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class CliTest(unittest.TestCase):

  def test_external_args(self):
    args = parser().parse_args(['ios', 'external', '--group', 'Friends',
                                '--what-to-test', 'x'])
    self.assertEqual((args.action, args.group, args.build),
                     ('external', 'Friends', None))
    args = parser().parse_args(['ios', 'archive', '--upload', '--external',
                                '--what-to-test-file', 'f'])
    self.assertTrue(args.external and args.upload)


if __name__ == '__main__':
  unittest.main()
