# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compass_tool import flatpak_remote  # noqa: E402
from compass_tool.cli import main, parser  # noqa: E402
from compass_tool.flatpak_remote import (Image, RemoteError,  # noqa: E402
                                         digest_of)

REF = 'app/io.github.Compass_Browser.Compass/x86_64/canary'
LAYER_TYPE = 'application/vnd.oci.image.layer.v1.tar+gzip'


def write_blob(oci: Path, data: bytes) -> str:
  digest = digest_of(data)
  path = oci / 'blobs' / 'sha256' / digest.split(':')[1]
  path.parent.mkdir(parents=True, exist_ok=True)
  path.write_bytes(data)
  return digest


def make_image(root: Path, ref: str = REF, labels_in_config: bool = True,
               layer: bytes = b'layer bytes') -> Path:
  """What `flatpak build-bundle --oci` writes, in small."""
  oci = root / 'Compass-1-linux-x64.oci'
  labels = {'org.flatpak.ref': ref, 'org.flatpak.metadata': '[Application]'}
  config = {'architecture': 'amd64', 'os': 'linux',
            'config': {'Labels': labels if labels_in_config else {}}}
  config_digest = write_blob(oci, json.dumps(config).encode())
  layer_digest = write_blob(oci, layer)
  manifest = {
      'schemaVersion': 2,
      'config': {'mediaType': 'application/vnd.oci.image.config.v1+json',
                 'digest': config_digest, 'size': 1},
      'layers': [{'mediaType': LAYER_TYPE, 'digest': layer_digest,
                  'size': len(layer)}],
  }
  if not labels_in_config:
    manifest['annotations'] = labels
  manifest_digest = write_blob(oci, json.dumps(manifest).encode())
  (oci / 'index.json').write_text(json.dumps({
      'schemaVersion': 2,
      'manifests': [{'mediaType': flatpak_remote.MANIFEST_TYPE,
                     'digest': manifest_digest, 'size': 1}]}))
  return oci


class ImageTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.TemporaryDirectory()
    self.root = Path(self.tmp.name)

  def tearDown(self):
    self.tmp.cleanup()

  def test_layers_point_at_the_release_assets(self):
    image = Image(make_image(self.root), '20261114.0100')
    manifest = json.loads(image.manifest)
    self.assertEqual(manifest['mediaType'], flatpak_remote.MANIFEST_TYPE)
    name = 'Compass-20261114.0100-linux-x64.oci-layer.tar.gz'
    self.assertEqual(manifest['layers'][0]['urls'], [
        'https://github.com/Compass-Browser/Compass/releases/download/'
        'v20261114.0100/' + name])
    self.assertEqual(image.layers[0][0], name)
    self.assertEqual(image.layers[0][1].read_bytes(), b'layer bytes')
    # Served by digest: the digest is the new manifest's.
    self.assertEqual(image.digest, digest_of(image.manifest))
    self.assertEqual(image.ref, REF)

  def test_labels_from_annotations(self):
    image = Image(make_image(self.root, labels_in_config=False), '1')
    self.assertEqual(image.ref, REF)
    entry = image.index_entry(['latest'])
    self.assertEqual(entry['Annotations']['org.flatpak.ref'], REF)

  def test_refuses_a_corrupt_blob(self):
    oci = make_image(self.root)
    for blob in (oci / 'blobs' / 'sha256').iterdir():
      if blob.read_bytes() == b'layer bytes':
        continue
      blob.write_bytes(blob.read_bytes() + b' ')
      break
    with self.assertRaises(RemoteError):
      Image(oci, '1')

  def test_refuses_what_isnt_a_flatpak(self):
    with self.assertRaises(RemoteError):
      Image(make_image(self.root, ref=''), '1')

  def test_more_layers_get_numbered(self):
    self.assertEqual(
        flatpak_remote.layer_asset_name('1', 1, 'application/x-tar'),
        'Compass-1-linux-x64.oci-layer-2.tar')


class IndexTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.TemporaryDirectory()
    self.root = Path(self.tmp.name)

  def tearDown(self):
    self.tmp.cleanup()

  def test_index_lists_the_image_in_the_site_registry(self):
    image = Image(make_image(self.root), '2')
    index = json.loads(flatpak_remote.merge_index(None, image, ['latest']))
    self.assertEqual(index['Registry'], flatpak_remote.SITE_URL)
    [result] = index['Results']
    self.assertEqual(result['Name'], flatpak_remote.IMAGE_NAME)
    [entry] = result['Images']
    self.assertEqual(entry['Digest'], image.digest)
    self.assertEqual(entry['OS'], 'linux')
    self.assertEqual(entry['Architecture'], 'amd64')
    self.assertEqual(entry['Labels']['org.flatpak.ref'], REF)

  def test_a_release_replaces_its_branch_only(self):
    old_canary = Image(make_image(self.root / 'a', layer=b'old'), '1')
    stable_ref = REF.replace('/canary', '/stable')
    stable = Image(make_image(self.root / 'b', ref=stable_ref,
                              layer=b'stable'), '1')
    index = flatpak_remote.merge_index(None, old_canary, ['latest'])
    index = flatpak_remote.merge_index(index, stable, ['latest'])
    new_canary = Image(make_image(self.root / 'c', layer=b'new'), '2')
    index = json.loads(flatpak_remote.merge_index(index, new_canary,
                                                  ['latest']))
    digests = {e['Labels']['org.flatpak.ref']: e['Digest']
               for e in index['Results'][0]['Images']}
    self.assertEqual(digests, {REF: new_canary.digest,
                               stable_ref: stable.digest})

  def test_a_broken_index_is_replaced(self):
    image = Image(make_image(self.root), '2')
    index = json.loads(flatpak_remote.merge_index(b'not json', image, []))
    self.assertEqual(len(index['Results'][0]['Images']), 1)

  def test_site_files(self):
    image = Image(make_image(self.root), '2')
    files = flatpak_remote.site_files(image, None, 'canary')
    self.assertEqual(files['v2/compass/manifests/' + image.digest],
                     image.manifest)
    self.assertEqual(files['v2/compass/blobs/' + image.config_digest],
                     image.config)
    self.assertIn('index/static', files)
    ref = files['compass-canary.flatpakref'].decode()
    self.assertIn('Url=' + flatpak_remote.REMOTE_URL, ref)
    self.assertIn('Branch=canary', ref)
    self.assertIn('Name=io.github.Compass_Browser.Compass', ref)
    self.assertNotIn('GPGKey', ref)

  def test_remote_url_is_where_flatpak_finds_the_index(self):
    # Flatpak appends index/static to the remote's path.
    self.assertTrue(flatpak_remote.REMOTE_URL.startswith('oci+https://'))
    self.assertTrue(flatpak_remote.REMOTE_URL.endswith('/'))
    self.assertEqual(flatpak_remote.REMOTE_URL[len('oci+'):],
                     flatpak_remote.SITE_URL)


class FakeGitHub:

  def __init__(self, head=None):
    self.head = head
    self.calls = []

  def api(self, method, path, payload=None, missing_ok=False):
    self.calls.append((method, path, payload))
    if path.startswith('git/ref/heads/'):
      return {'object': {'sha': self.head}} if self.head else None
    if path.startswith('git/commits/'):
      return {'tree': {'sha': 'tree0'}}
    if path == 'git/blobs':
      return {'sha': 'blob%d' % len(self.calls)}
    if path == 'git/trees':
      return {'sha': 'tree1'}
    if path == 'git/commits':
      return {'sha': 'commit1'}
    return {}


class UpdateSiteTest(unittest.TestCase):

  def test_adds_a_commit_on_the_branch(self):
    github = FakeGitHub(head='commit0')
    commit = flatpak_remote.update_site(github, {'index/static': b'{}'}, 'm')
    self.assertEqual(commit, 'commit1')
    methods = {(m, p): body for m, p, body in github.calls}
    self.assertEqual(methods[('POST', 'git/trees')]['base_tree'], 'tree0')
    self.assertEqual(methods[('POST', 'git/trees')]['tree'][0]['path'],
                     'index/static')
    self.assertEqual(methods[('POST', 'git/commits')]['parents'],
                     ['commit0'])
    self.assertEqual(methods[('PATCH', 'git/refs/heads/flatpak')],
                     {'sha': 'commit1', 'force': False})

  def test_creates_the_branch(self):
    github = FakeGitHub()
    flatpak_remote.update_site(github, {'index/static': b'{}'}, 'm')
    methods = {(m, p): body for m, p, body in github.calls}
    self.assertNotIn('base_tree', methods[('POST', 'git/trees')])
    self.assertEqual(methods[('POST', 'git/commits')]['parents'], [])
    self.assertEqual(methods[('POST', 'git/refs')],
                     {'ref': 'refs/heads/flatpak', 'sha': 'commit1'})


class PublishLinuxTest(unittest.TestCase):

  def test_parses(self):
    args = parser().parse_args(
        ['release', 'publish-linux', '--tarball', 'C.tar.xz'])
    self.assertEqual(args.action, 'publish-linux')
    self.assertEqual(args.tarball, 'C.tar.xz')

  def test_no_upload_lists_what_it_would_publish(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = Path(tmp)
      oci = make_image(root)
      tarball = root / 'Compass-1-linux-x64.tar.xz'
      tarball.write_bytes(b'tar')
      (root / 'Compass-1-linux-x64.flatpak').write_bytes(b'bundle')
      (root / 'Compass-1-linux-x64.json').write_text(json.dumps({
          'version': '1', 'chromium': '157.0.1.2', 'channel': 'canary',
          'commit': 'abc', 'bundle': 'Compass-1-linux-x64.flatpak',
          'oci': oci.name}))
      out = io.StringIO()
      with redirect_stdout(out):
        main(['release', 'publish-linux', '--tarball', str(tarball),
              '--no-upload'])
      text = out.getvalue()
      for name in ('Compass-1-linux-x64.tar.xz',
                   'Compass-1-linux-x64.flatpak',
                   'Compass-1-linux-x64.oci-layer.tar.gz',
                   'compass-canary.flatpakref'):
        self.assertIn('would upload %s' % name, text)
      self.assertIn('would write index/static on the flatpak branch', text)


if __name__ == '__main__':
  unittest.main()
