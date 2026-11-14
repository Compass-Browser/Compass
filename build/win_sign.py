#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Authenticode signing through Azure Artifact Signing (docs/design/updates.md,
"Windows"): signtool with Microsoft's dlib, timestamped, since the
certificates last three days. The dlib signs in as the Entra app in
AZURE_TENANT_ID, AZURE_CLIENT_ID and AZURE_CLIENT_SECRET.

Used by `compass release build-win` for the installers, and by Chromium's
create_installer_archive.py (patched) for the binaries it packs.

  win_sign.py --dlib <Azure.CodeSigning.Dlib.dll> --metadata <metadata.json>
              [--skip-signed] FILE...
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys

TIMESTAMP_URL = 'http://timestamp.acs.microsoft.com'
BINARY_EXTENSIONS = ('.exe', '.dll')


def find_signtool():
  kits = os.path.join(
      os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
      'Windows Kits', '10', 'bin')
  found = sorted(glob.glob(os.path.join(kits, '10.*', 'x64', 'signtool.exe')))
  if not found:
    raise RuntimeError('no signtool.exe under %s (the Windows SDK)' % kits)
  return found[-1]


def is_signed(signtool, path):
  return subprocess.run([signtool, 'verify', '/pa', '/q', path],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL).returncode == 0


def sign(files, dlib, metadata, skip_signed=False):
  """Signs |files| and checks the signatures. |skip_signed| leaves files
  that already have a valid signature (Microsoft's DLLs) as they are."""
  signtool = find_signtool()
  if skip_signed:
    files = [f for f in files if not is_signed(signtool, f)]
  if not files:
    return
  subprocess.run([
      signtool, 'sign', '/fd', 'SHA256', '/tr', TIMESTAMP_URL, '/td', 'SHA256',
      '/dlib', dlib, '/dmdf', metadata
  ] + list(files), check=True)
  subprocess.run([signtool, 'verify', '/pa', '/q'] + list(files), check=True)


def sign_tree(root, dlib, metadata):
  """Signs every unsigned .exe and .dll under |root|."""
  files = []
  for dirpath, _, names in os.walk(root):
    files += [os.path.join(dirpath, n) for n in sorted(names)
              if n.lower().endswith(BINARY_EXTENSIONS)]
  sign(files, dlib, metadata, skip_signed=True)


def signed_copy(path, directory, dlib, metadata):
  """A signed copy of |path| in |directory|, for outputs of the build that
  must stay as they are (signing them in place makes siso build them
  again)."""
  copy = os.path.join(directory, os.path.basename(path))
  shutil.copyfile(path, copy)
  sign([copy], dlib, metadata)
  return copy


def main():
  parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
  parser.add_argument('--dlib', required=True)
  parser.add_argument('--metadata', required=True)
  parser.add_argument('--skip-signed', action='store_true')
  parser.add_argument('files', nargs='+')
  args = parser.parse_args()
  sign(args.files, args.dlib, args.metadata, args.skip_signed)


if __name__ == '__main__':
  sys.exit(main())
