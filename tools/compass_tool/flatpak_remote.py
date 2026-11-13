# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""The Flatpak remote installed Compass Flatpaks update from, kept on GitHub
(docs/building.md, "Linux releases").

An OSTree repository is a tree of small files, which GitHub release assets
can't hold. Flatpak's other kind of remote, OCI (`oci+https://`), needs only:

- a static index, `<remote>/index/static`, listing each image with its
  labels (the app's ref and metadata) and the registry it's in;
- in that registry, each image's manifest and config:
  `v2/<name>/manifests/<digest>` and `v2/<name>/blobs/<digest>`;
- its layers, which Flatpak downloads from the URLs the manifest lists for
  them when it has any (OCI descriptors' `urls`).

So the layer, the only big file, is an asset of the release's GitHub
release, next to the tarball and the bundle, and the index, manifests and
configs (a few KB) are files on the repository's `flatpak` branch, served
by raw.githubusercontent.com. Flatpak checks every manifest, config and layer
against its digest; there's no GPG for OCI remotes, the index comes over
HTTPS from GitHub.

This module turns `flatpak build-bundle --oci`'s image into those files.
"""

from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path
from typing import Dict, List, Tuple

from . import linux_package

GITHUB_REPO = 'Compass-Browser/Compass'
# The branch whose files raw.githubusercontent.com serves as the remote.
SITE_BRANCH = 'flatpak'
SITE_URL = 'https://raw.githubusercontent.com/%s/%s/' % (GITHUB_REPO,
                                                         SITE_BRANCH)
# What `flatpak remote-add` and the bundles get. Flatpak appends
# index/static.
REMOTE_URL = 'oci+' + SITE_URL
# The image's name in the index and the registry paths.
IMAGE_NAME = 'compass'
# The Flatpak branch of each release channel, so canary and a later stable
# install side by side.
FLATPAK_BRANCHES = {'canary': 'canary', 'beta': 'beta', 'stable': 'stable'}
RUNTIME_REPO = 'https://dl.flathub.org/repo/flathub.flatpakrepo'
HOMEPAGE = 'https://compassbrowser.org'

MANIFEST_TYPE = 'application/vnd.oci.image.manifest.v1+json'
GZIP_LAYER_TYPES = ('application/vnd.oci.image.layer.v1.tar+gzip',
                    'application/vnd.docker.image.rootfs.diff.tar.gzip')


class RemoteError(Exception):
  pass


def digest_of(data: bytes) -> str:
  return 'sha256:' + hashlib.sha256(data).hexdigest()


def blob_path(oci_dir: Path, digest: str) -> Path:
  algorithm, _, hex_digest = digest.partition(':')
  if algorithm != 'sha256' or not hex_digest:
    raise RemoteError('unsupported digest %s' % digest)
  return oci_dir / 'blobs' / 'sha256' / hex_digest


def read_blob(oci_dir: Path, digest: str) -> bytes:
  data = blob_path(oci_dir, digest).read_bytes()
  if digest_of(data) != digest:
    raise RemoteError('%s doesn\'t match its digest' % digest)
  return data


def layer_asset_name(version: str, index: int, media_type: str) -> str:
  """The GitHub release asset a layer is uploaded as."""
  ext = 'tar.gz' if media_type in GZIP_LAYER_TYPES else 'tar'
  suffix = '' if index == 0 else '-%d' % (index + 1)
  return 'Compass-%s-linux-x64.oci-layer%s.%s' % (version, suffix, ext)


def asset_url(version: str, name: str) -> str:
  return 'https://github.com/%s/releases/download/v%s/%s' % (GITHUB_REPO,
                                                             version, name)


def canonical_json(value) -> bytes:
  return json.dumps(value, separators=(',', ':')).encode()


class Image:
  """`flatpak build-bundle --oci`'s image, made to be served from GitHub."""

  def __init__(self, oci_dir: Path, version: str):
    self.oci_dir = oci_dir
    self.version = version
    index = json.loads((oci_dir / 'index.json').read_text())
    manifests = index.get('manifests') or []
    if len(manifests) != 1:
      raise RemoteError('%s: expected one image, found %d' %
                        (oci_dir, len(manifests)))
    original = json.loads(read_blob(oci_dir, manifests[0]['digest']))
    self.config_digest = original['config']['digest']
    self.config = read_blob(oci_dir, self.config_digest)
    config = json.loads(self.config)
    self.labels: Dict[str, str] = dict(
        (config.get('config') or {}).get('Labels') or {})
    self.annotations: Dict[str, str] = dict(original.get('annotations') or {})
    if not (self.labels.get('org.flatpak.ref') or
            self.annotations.get('org.flatpak.ref')):
      raise RemoteError('%s isn\'t a Flatpak image (no org.flatpak.ref)' %
                        oci_dir)
    self.architecture = config.get('architecture', 'amd64')
    self.os = config.get('os', 'linux')
    # The layers, each with the URL of its release asset.
    self.layers: List[Tuple[str, Path]] = []
    manifest = dict(original)
    manifest.setdefault('mediaType', MANIFEST_TYPE)
    layers = []
    for i, layer in enumerate(original['layers']):
      name = layer_asset_name(version, i, layer.get('mediaType', ''))
      self.layers.append((name, blob_path(oci_dir, layer['digest'])))
      layers.append(dict(layer, urls=[asset_url(version, name)]))
    manifest['layers'] = layers
    self.manifest = canonical_json(manifest)
    self.digest = digest_of(self.manifest)

  @property
  def ref(self) -> str:
    return (self.labels.get('org.flatpak.ref') or
            self.annotations['org.flatpak.ref'])

  def index_entry(self, tags: List[str]) -> Dict:
    entry = {
        'Digest': self.digest,
        'MediaType': MANIFEST_TYPE,
        'OS': self.os,
        'Architecture': self.architecture,
        'Labels': self.labels,
        'Tags': tags,
    }
    if self.annotations:
      entry['Annotations'] = self.annotations
    return entry


def index(images: List[Dict]) -> bytes:
  """The static index: the registry is the site itself."""
  return json.dumps({
      'Registry': SITE_URL,
      'Results': [{'Name': IMAGE_NAME, 'Images': images}],
  }, indent=1, sort_keys=True).encode()


def merge_index(old: bytes | None, image: Image, tags: List[str]) -> bytes:
  """The index with |image| in place of the image of the same ref: one
  image per channel's branch."""
  images = []
  if old:
    try:
      for result in json.loads(old).get('Results') or []:
        if result.get('Name') != IMAGE_NAME:
          continue
        for entry in result.get('Images') or []:
          labels = entry.get('Labels') or {}
          annotations = entry.get('Annotations') or {}
          ref = (labels.get('org.flatpak.ref') or
                 annotations.get('org.flatpak.ref'))
          if ref != image.ref:
            images.append(entry)
    except ValueError:
      images = []
  images.append(image.index_entry(tags))
  images.sort(key=lambda e: (e.get('Labels') or {}).get('org.flatpak.ref',
                                                        ''))
  return index(images)


def flatpakref(channel: str) -> str:
  """What `flatpak install <file>` (or a click on it) installs from."""
  branch = FLATPAK_BRANCHES[channel]
  title = 'Compass' if channel == 'stable' else 'Compass %s' % channel.title()
  return '\n'.join([
      '[Flatpak Ref]',
      'Name=%s' % linux_package.APP_ID,
      'Branch=%s' % branch,
      'Title=%s' % title,
      'Url=%s' % REMOTE_URL,
      'RuntimeRepo=%s' % RUNTIME_REPO,
      'IsRuntime=false',
      'SuggestRemoteName=compass',
      'Homepage=%s' % HOMEPAGE,
  ]) + '\n'


def flatpakrepo() -> str:
  """For `flatpak remote-add compass <file>`."""
  return '\n'.join([
      '[Flatpak Repo]',
      'Title=Compass',
      'Url=%s' % REMOTE_URL,
      'Homepage=%s' % HOMEPAGE,
      'Comment=The Compass browser',
  ]) + '\n'


def flatpakref_name(channel: str) -> str:
  return 'compass-%s.flatpakref' % channel


SITE_README = """\
# Compass's Flatpak remote

Generated by `compass release publish-linux`; don't edit by hand. Flatpak
reads it as the OCI remote %s (docs/building.md, "Linux releases"):
`index/static` lists the current image of each branch, `v2/` holds their
manifests and configs, and the layers are assets of the GitHub releases.
"""


def site_files(image: Image, old_index: bytes | None,
               channel: str) -> Dict[str, bytes]:
  """The files the release adds to (or changes on) the site branch."""
  return {
      'index/static': merge_index(old_index, image, ['latest', channel]),
      'v2/%s/manifests/%s' % (IMAGE_NAME, image.digest): image.manifest,
      'v2/%s/blobs/%s' % (IMAGE_NAME, image.config_digest): image.config,
      flatpakref_name(channel): flatpakref(channel).encode(),
      'compass.flatpakrepo': flatpakrepo().encode(),
      'README.md': (SITE_README % REMOTE_URL).encode(),
  }


# GitHub ----------------------------------------------------------------------


def tree_entries(blobs: Dict[str, str]) -> List[Dict]:
  """Git Data API tree entries for files already uploaded as blobs."""
  return [{'path': path, 'mode': '100644', 'type': 'blob', 'sha': sha}
          for path, sha in sorted(blobs.items())]


def b64(data: bytes) -> str:
  return base64.b64encode(data).decode()


class GitHub:
  """The few REST calls publishing needs, through `gh api` (signed in with
  write access to the repository)."""

  def __init__(self, repo: str = GITHUB_REPO):
    self.repo = repo

  def api(self, method: str, path: str, payload: Dict | None = None,
          missing_ok: bool = False):
    """The JSON reply, or None for a 404 when |missing_ok|."""
    import subprocess
    args = ['gh', 'api', '-X', method, 'repos/%s/%s' % (self.repo, path)]
    if payload is not None:
      args += ['--input', '-']
    try:
      done = subprocess.run(args, capture_output=True, text=True,
                            input=json.dumps(payload) if payload else None)
    except FileNotFoundError:
      raise RemoteError('needs the GitHub CLI (gh), signed in with write '
                        'access to %s' % self.repo)
    if done.returncode != 0:
      if missing_ok and 'HTTP 404' in done.stderr:
        return None
      raise RemoteError('gh api %s %s failed: %s' %
                        (method, path, done.stderr.strip()[-300:]))
    return json.loads(done.stdout) if done.stdout.strip() else {}


def read_site_file(github: GitHub, path: str) -> bytes | None:
  reply = github.api('GET', 'contents/%s?ref=%s' % (path, SITE_BRANCH),
                     missing_ok=True)
  if not reply or reply.get('encoding') != 'base64':
    return None
  return base64.b64decode(reply['content'])


def update_site(github: GitHub, files: Dict[str, bytes],
                message: str) -> str:
  """One commit on the site branch adding or replacing |files| (creating the
  branch the first time). Returns the commit."""
  head = github.api('GET', 'git/ref/heads/' + SITE_BRANCH, missing_ok=True)
  parents: List[str] = []
  tree: Dict = {}
  if head:
    parent = head['object']['sha']
    parents = [parent]
    tree['base_tree'] = github.api('GET', 'git/commits/' + parent)['tree'][
        'sha']
  blobs = {}
  for path, data in files.items():
    blobs[path] = github.api('POST', 'git/blobs', {
        'content': b64(data), 'encoding': 'base64'})['sha']
  tree['tree'] = tree_entries(blobs)
  tree_sha = github.api('POST', 'git/trees', tree)['sha']
  commit = github.api('POST', 'git/commits', {
      'message': message, 'tree': tree_sha, 'parents': parents})['sha']
  if head:
    github.api('PATCH', 'git/refs/heads/' + SITE_BRANCH,
               {'sha': commit, 'force': False})
  else:
    github.api('POST', 'git/refs',
               {'ref': 'refs/heads/' + SITE_BRANCH, 'sha': commit})
  return commit
