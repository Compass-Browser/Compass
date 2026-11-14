# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import base64
import contextlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compass_tool import play  # noqa: E402
from compass_tool.cli import main, parser  # noqa: E402
from compass_tool.process import CommandError  # noqa: E402


def b64url_json(part: str) -> dict:
  return json.loads(base64.urlsafe_b64decode(part + '=' * (-len(part) % 4)))


INFO = {
    'type': 'service_account',
    'client_email': 'uploader@example.iam.gserviceaccount.com',
    'private_key': 'PEM',
    'private_key_id': 'k1',
    'token_uri': 'https://oauth2.googleapis.com/token',
}


class VersionCodeTest(unittest.TestCase):

  def test_example(self):
    self.assertEqual(1263181530, play.version_code('20261114.1530'))

  def test_grows_with_the_version(self):
    versions = ['20261114.1530', '20261114.1531', '20261114.1600',
                '20261115.0000', '20261231.2359', '20270101.0000',
                '20280229.1200', '20991231.2359']
    codes = [play.version_code(v) for v in versions]
    self.assertEqual(sorted(codes), codes)
    self.assertEqual(len(set(codes)), len(codes))

  def test_stays_in_plays_range_and_above_chromiums(self):
    self.assertLess(play.version_code('20991231.2359'), play.MAX_VERSION_CODE)
    # Chromium's own versionCode for 157.0.8103.51, which the first APKs had.
    self.assertGreater(play.version_code('20260101.0000'), 810305100)

  def test_rejects_other_versions(self):
    for bad in ('157.0.8103.51', '20261114', '20261114.153', '20261332.1200',
                '20261114.2460', '21000101.0000', '19991231.2359'):
      with self.assertRaises(CommandError, msg=bad):
        play.version_code(bad)


class ReleaseNotesTest(unittest.TestCase):

  def setUp(self):
    self.dir = Path(tempfile.mkdtemp())
    self.addCleanup(shutil.rmtree, self.dir)

  def test_reads_each_language(self):
    (self.dir / 'whatsnew-en-US.txt').write_text('  Spaces and sync.\n')
    (self.dir / 'whatsnew-it-IT.txt').write_text('Spazi e sync.')
    (self.dir / 'full-description.txt').write_text('not notes')
    self.assertEqual([{'language': 'en-US', 'text': 'Spaces and sync.'},
                      {'language': 'it-IT', 'text': 'Spazi e sync.'}],
                     play.release_notes(self.dir))

  def test_limit(self):
    (self.dir / 'whatsnew-en-US.txt').write_text('x' * 500)
    play.release_notes(self.dir)
    (self.dir / 'whatsnew-en-US.txt').write_text('x' * 501)
    with self.assertRaisesRegex(CommandError, '501 characters'):
      play.release_notes(self.dir)

  def test_empty_or_missing(self):
    with self.assertRaisesRegex(CommandError, 'no whatsnew'):
      play.release_notes(self.dir)
    (self.dir / 'whatsnew-en-US.txt').write_text('\n')
    with self.assertRaisesRegex(CommandError, 'empty'):
      play.release_notes(self.dir)

  def test_bad_name(self):
    (self.dir / 'whatsnew-english.txt.txt').write_text('x')
    with self.assertRaisesRegex(CommandError, 'whatsnew-<language>'):
      play.release_notes(self.dir)

  def test_repository_notes_fit(self):
    notes = play.release_notes(ROOT / 'android' / 'play')
    self.assertIn('en-US', [n['language'] for n in notes])


class TrackReleaseTest(unittest.TestCase):
  NOTES = [{'language': 'en-US', 'text': 'New.'}]

  def test_body(self):
    self.assertEqual({
        'track': 'beta',
        'releases': [{
            'name': '20261114.1530',
            'versionCodes': ['1263181530'],
            'status': 'completed',
            'releaseNotes': self.NOTES,
        }],
    }, play.track_release('beta', '20261114.1530', 1263181530, 'completed',
                          self.NOTES))

  def test_staged_rollout(self):
    body = play.track_release('production', 'v', 1, 'inProgress', self.NOTES,
                              0.1)
    self.assertEqual(0.1, body['releases'][0]['userFraction'])
    with self.assertRaises(CommandError):
      play.track_release('production', 'v', 1, 'inProgress', self.NOTES)
    with self.assertRaises(CommandError):
      play.track_release('beta', 'v', 1, 'completed', self.NOTES, 0.5)
    with self.assertRaises(CommandError):
      play.track_release('beta', 'v', 1, 'inProgress', self.NOTES, 1.0)

  def test_status(self):
    with self.assertRaises(CommandError):
      play.track_release('beta', 'v', 1, 'live', self.NOTES)


class CredentialsTest(unittest.TestCase):

  def setUp(self):
    self.dir = Path(tempfile.mkdtemp())
    self.addCleanup(shutil.rmtree, self.dir)

  def test_missing_names_the_path(self):
    path = self.dir / 'play-service-account.json'
    with self.assertRaisesRegex(CommandError, str(path)):
      play.load_credentials(path)

  def test_checks_the_kind(self):
    path = self.dir / 'key.json'
    path.write_text(json.dumps({'type': 'authorized_user'}))
    with self.assertRaisesRegex(CommandError, 'not a service account'):
      play.load_credentials(path)
    path.write_text(json.dumps({'type': 'service_account',
                                'client_email': 'a@b'}))
    with self.assertRaisesRegex(CommandError, 'private_key'):
      play.load_credentials(path)
    path.write_text('{')
    with self.assertRaisesRegex(CommandError, 'not JSON'):
      play.load_credentials(path)
    path.write_text(json.dumps(INFO))
    self.assertEqual(INFO, play.load_credentials(path))


class TokenTest(unittest.TestCase):

  def test_signing_input(self):
    header, claims = play.jwt_signing_input(INFO, now=1000).split('.')
    self.assertEqual({'alg': 'RS256', 'typ': 'JWT', 'kid': 'k1'},
                     b64url_json(header))
    self.assertEqual({
        'iss': INFO['client_email'],
        'scope': 'https://www.googleapis.com/auth/androidpublisher',
        'aud': 'https://oauth2.googleapis.com/token',
        'iat': 1000,
        'exp': 4600,
    }, b64url_json(claims))

  def test_default_audience(self):
    info = dict(INFO)
    del info['token_uri']
    claims = play.jwt_signing_input(info, now=0).split('.')[1]
    self.assertEqual(play.DEFAULT_TOKEN_URI, b64url_json(claims)['aud'])

  def test_assertion(self):
    seen = []

    def sign(key, data):
      seen.append((key, data))
      return b'\xff\xfe'

    assertion = play.make_assertion(INFO, now=1000, sign=sign)
    self.assertEqual([('PEM', play.jwt_signing_input(INFO, now=1000))], seen)
    self.assertTrue(assertion.endswith('.__4'))
    body = play.token_request_body(assertion).decode()
    self.assertTrue(body.startswith(
        'grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&'
        'assertion='))

  @unittest.skipUnless(shutil.which('openssl'), 'needs openssl')
  def test_openssl_signature_verifies(self):
    work = Path(tempfile.mkdtemp())
    self.addCleanup(shutil.rmtree, work)
    key = work / 'key.pem'
    subprocess.run(['openssl', 'genrsa', '-out', str(key), '2048'],
                   check=True, capture_output=True)
    subprocess.run(['openssl', 'rsa', '-in', str(key), '-pubout', '-out',
                    str(work / 'pub.pem')], check=True, capture_output=True)
    data = play.jwt_signing_input(INFO, now=1000)
    (work / 'sig').write_bytes(play.sign_rs256(key.read_text(), data))
    (work / 'data').write_text(data)
    result = subprocess.run(
        ['openssl', 'dgst', '-sha256', '-verify', str(work / 'pub.pem'),
         '-signature', str(work / 'sig'), str(work / 'data')],
        capture_output=True)
    self.assertEqual(0, result.returncode, result.stderr)

  def test_bad_key(self):
    if not shutil.which('openssl'):
      self.skipTest('needs openssl')
    with self.assertRaisesRegex(CommandError, 'openssl'):
      play.sign_rs256('not a key', 'data')


class FakePlay:
  """The edits API: records calls, answers like Play."""

  def __init__(self, uploaded_code=1263181530, fail_on=None):
    self.calls = []
    self.uploaded_code = uploaded_code
    self.fail_on = fail_on

  def __call__(self, method, url, body, headers, timeout):
    self.assertion = headers['Authorization']
    path = url.split('/applications/')[1]
    self.calls.append((method, path))
    if self.fail_on and self.fail_on in path:
      raise play.ApiError(403, method, url, json.dumps(
          {'error': {'message': 'The caller does not have permission'}}))
    if path.endswith('/edits'):
      return {'id': 'e1'}
    if '/bundles' in path:
      self.bundle = body
      return {'versionCode': self.uploaded_code, 'sha256': 'x'}
    if '/tracks/' in path:
      self.track = json.loads(body)
      return self.track
    return {}


class UploadTest(unittest.TestCase):

  def setUp(self):
    work = Path(tempfile.mkdtemp())
    self.addCleanup(shutil.rmtree, work)
    self.aab = work / 'Compass.aab'
    self.aab.write_bytes(b'PK')
    self.body = play.track_release('beta', '20261114.1530', 1263181530,
                                   'completed', [{'language': 'en-US',
                                                  'text': 'New.'}])

  def run_upload(self, api, **kwargs):
    client = play.Client(lambda: 'tok', transport=api)
    return play.upload(client, self.aab, self.body, log=lambda _: None,
                       **kwargs)

  def test_commits_one_edit(self):
    api = FakePlay()
    self.assertEqual(1263181530, self.run_upload(api))
    self.assertEqual([
        ('POST', 'it.cipi1965.compass/edits'),
        ('POST', 'it.cipi1965.compass/edits/e1/bundles?uploadType=media'),
        ('PUT', 'it.cipi1965.compass/edits/e1/tracks/beta'),
        ('POST', 'it.cipi1965.compass/edits/e1:commit'),
    ], api.calls)
    self.assertEqual(self.aab, api.bundle)
    self.assertEqual(self.body, api.track)
    self.assertEqual('Bearer tok', api.assertion)

  def test_not_sent_for_review(self):
    api = FakePlay()
    self.run_upload(api, not_sent_for_review=True)
    self.assertEqual(('POST', 'it.cipi1965.compass/edits/e1:commit?'
                      'changesNotSentForReview=true'), api.calls[-1])

  def test_validate_only_throws_the_edit_away(self):
    api = FakePlay()
    self.run_upload(api, commit=False)
    self.assertEqual([('POST', 'it.cipi1965.compass/edits/e1:validate'),
                      ('DELETE', 'it.cipi1965.compass/edits/e1')],
                     api.calls[-2:])

  def test_wrong_version_code_stops_before_the_track(self):
    api = FakePlay(uploaded_code=810305100)
    with self.assertRaisesRegex(CommandError, 'expected 1263181530'):
      self.run_upload(api)
    self.assertNotIn('tracks', ' '.join(p for _, p in api.calls))
    self.assertEqual(('DELETE', 'it.cipi1965.compass/edits/e1'), api.calls[-1])

  def test_error_deletes_the_edit(self):
    api = FakePlay(fail_on='/tracks/')
    with self.assertRaises(play.ApiError) as e:
      self.run_upload(api)
    self.assertEqual(('DELETE', 'it.cipi1965.compass/edits/e1'), api.calls[-1])
    self.assertIn('Release to testing tracks', play.explain(e.exception))


class ExplainTest(unittest.TestCase):

  def error(self, status, message):
    return play.ApiError(status, 'PUT', 'u', json.dumps(
        {'error': {'message': message}}))

  def test_hints(self):
    self.assertIn('draft', play.explain(self.error(
        400, 'Only releases with status draft may be created on draft app.')))
    self.assertIn('create the app', play.explain(self.error(
        404, 'Package not found: it.cipi1965.compass.')))
    self.assertIn('new versionCode', play.explain(self.error(
        403, 'APK specifies a version code that has already been used.')))
    self.assertEqual('', play.explain(self.error(500, 'Backend error')))

  def test_message_from_plain_text(self):
    e = play.ApiError(502, 'POST', 'u', 'Bad gateway')
    self.assertEqual('Bad gateway', e.message)


class CliTest(unittest.TestCase):

  def test_parses(self):
    args = parser().parse_args(['play', 'upload', '--aab', 'x.aab',
                                '--version', '20261114.1530'])
    self.assertEqual(('upload', 'beta', 'completed'),
                     (args.action, args.track, args.status))

  def test_dry_run_sends_nothing(self):
    work = Path(tempfile.mkdtemp())
    self.addCleanup(shutil.rmtree, work)
    aab = work / 'Compass.aab'
    aab.write_bytes(b'PK')
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
      code = main(['play', 'upload', '--aab', str(aab), '--version',
                   '20261114.1530', '--dry-run', '--credentials',
                   str(work / 'none.json')])
    self.assertEqual(0, code)
    text = out.getvalue()
    self.assertIn('not there yet', text)
    self.assertIn('"versionCodes": [\n        "1263181530"', text)
    self.assertIn(':commit', text)

  def test_upload_without_key_names_the_path(self):
    work = Path(tempfile.mkdtemp())
    self.addCleanup(shutil.rmtree, work)
    aab = work / 'Compass.aab'
    aab.write_bytes(b'PK')
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
      code = main(['play', 'upload', '--aab', str(aab), '--version',
                   '20261114.1530', '--credentials', str(work / 'k.json')])
    self.assertEqual(1, code)
    self.assertIn(str(work / 'k.json'), err.getvalue())


if __name__ == '__main__':
  unittest.main()
