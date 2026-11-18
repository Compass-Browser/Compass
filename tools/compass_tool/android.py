# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Android builds from a Mac (docs/building.md, "Android from a Mac").

Chromium only supports Linux hosts for Android, but its CIPD packages come in
Mac flavors for nearly everything (the JDK, the SDK build tools, the NDK).
Two of them don't fit the build as is; `prepare_mac_host` fixes them up after
every gclient sync, which can put the originals back.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import List

# The --cpu value (layout.CROSS_TARGETS) and what it builds for.
# android-x64 is for the emulator on an x86_64 host, which can't run the
# arm64 build through its ARM translation.
TARGETS = {'android-arm64': 'arm64', 'android-x64': 'x64'}

ELF_MAGIC = b'\x7fELF'


def is_android(cpu: str | None) -> bool:
  return cpu in TARGETS


def _is_elf(path: Path) -> bool:
  with open(path, 'rb') as f:
    return f.read(4) == ELF_MAGIC


def link_jdk(src: Path) -> List[str]:
  """The Mac JDK is a bundle (current/Contents/Home/...); the build runs
  current/bin/java and lists current/lib/... as inputs. Links each entry of
  Contents/Home into current/. Returns what it linked."""
  current = src / 'third_party' / 'jdk' / 'current'
  home = current / 'Contents' / 'Home'
  linked = []
  if not home.is_dir():
    return linked
  for entry in sorted(home.iterdir()):
    link = current / entry.name
    if link.exists() or link.is_symlink():
      continue
    link.symlink_to(Path('Contents') / 'Home' / entry.name)
    linked.append(entry.name)
  return linked


def use_sdk_aapt2(src: Path) -> bool:
  """Chromium pins its own aapt2, Linux only; the SDK's build tools have a
  Mac one. Replaces the pinned one with a link to it. Returns whether it did."""
  pinned = src / 'third_party' / 'android_build_tools' / 'aapt2' / 'cipd' / 'aapt2'
  if not pinned.is_file() or pinned.is_symlink() or not _is_elf(pinned):
    return False
  build_tools = src / 'third_party' / 'android_sdk' / 'public' / 'build-tools'
  candidates = sorted(build_tools.glob('*/aapt2'))
  if not candidates:
    raise FileNotFoundError('no aapt2 in %s' % build_tools)
  pinned.rename(pinned.with_name('aapt2.linux'))
  pinned.symlink_to(os.path.relpath(candidates[-1], pinned.parent))
  return True


def prepare_mac_host(src: Path) -> None:
  """Fixes up the checkout's Android tools for a Mac host. Idempotent, and a
  no-op elsewhere."""
  if sys.platform != 'darwin':
    return
  for name in link_jdk(src):
    print('android: linked jdk/current/%s' % name)
  if use_sdk_aapt2(src):
    print('android: using the SDK\'s Mac aapt2')


# Release APKs (docs/building.md, "Android releases"): the GitHub release's
# APK is signed with the release key, the one Play App Signing signs with, so
# either can update the other.

class ApkError(Exception):
  pass


def build_tools(src: Path) -> Path:
  """The newest SDK build tools in the checkout (apksigner, aapt2)."""
  root = src / 'third_party' / 'android_sdk' / 'public' / 'build-tools'
  found = sorted((p for p in root.glob('*') if (p / 'apksigner').is_file()),
                 key=lambda p: [int(x) if x.isdigit() else x
                                for x in p.name.split('.')])
  if not found:
    raise ApkError('no SDK build tools with apksigner in %s' % root)
  return found[-1]


def java_env(src: Path) -> dict:
  """The checkout's JDK first on PATH: apksigner and keytool need one."""
  env = dict(os.environ)
  jdk = src / 'third_party' / 'jdk' / 'current'
  env['JAVA_HOME'] = str(jdk)
  env['PATH'] = str(jdk / 'bin') + os.pathsep + env.get('PATH', '')
  return env


def _run(cmd: List[str], src: Path) -> str:
  import subprocess
  done = subprocess.run(cmd, capture_output=True, text=True, env=java_env(src))
  if done.returncode != 0:
    raise ApkError('%s failed: %s' % (Path(cmd[0]).name,
                                      (done.stderr or done.stdout).strip()[-400:]))
  return done.stdout


def parse_version_code(badging: str) -> int:
  """versionCode from `aapt2 dump badging`'s first line."""
  import re
  match = re.search(r"versionCode='(\d+)'", badging)
  if not match:
    raise ApkError('no versionCode in the APK\'s badging')
  return int(match.group(1))


def apk_version_code(src: Path, apk: Path) -> int:
  return parse_version_code(
      _run([str(build_tools(src) / 'aapt2'), 'dump', 'badging', str(apk)], src))


def parse_signer_sha256(output: str) -> str:
  """The first signer's certificate SHA-256 from `apksigner verify
  --print-certs`, lowercase hex."""
  import re
  match = re.search(r'certificate SHA-256 digest: ([0-9a-fA-F]{64})', output)
  if not match:
    raise ApkError('the APK has no signer apksigner could read')
  return match.group(1).lower()


def parse_keytool_sha256(output: str) -> str:
  """The certificate SHA-256 from `keytool -list -v`, lowercase hex."""
  import re
  match = re.search(r'SHA256:\s*([0-9A-Fa-f:]{95})', output)
  if not match:
    raise ApkError('keytool printed no SHA-256 fingerprint')
  return match.group(1).replace(':', '').lower()


def apk_signer_sha256(src: Path, apk: Path) -> str:
  return parse_signer_sha256(_run(
      [str(build_tools(src) / 'apksigner'), 'verify', '--print-certs',
       str(apk)], src))


def keystore_sha256(src: Path, keystore: Path, alias: str,
                    password_file: Path) -> str:
  password = password_file.read_text().strip()
  return parse_keytool_sha256(_run(
      ['keytool', '-list', '-v', '-keystore', str(keystore), '-alias', alias,
       '-storepass', password], src))


def sign_apk(src: Path, apk: Path, out: Path, keystore: Path, alias: str,
             password_file: Path) -> None:
  """|apk| signed with |keystore|'s |alias| (v2 and v3 signatures, as the
  build's), written to |out|."""
  _run([str(build_tools(src) / 'apksigner'), 'sign', '--ks', str(keystore),
        '--ks-key-alias', alias, '--ks-pass', 'file:%s' % password_file,
        '--out', str(out), str(apk)], src)
  idsig = out.with_name(out.name + '.idsig')
  if idsig.exists():
    idsig.unlink()


def sign_bundle(src: Path, aab: Path, out: Path, keystore: Path, alias: str,
                password_file: Path) -> None:
  """|aab| signed as a JAR with |keystore|'s |alias| (Play's upload key),
  written to |out|. The build signs bundles with Chromium's debug key, whose
  signature goes first: a JAR holds one signer here."""
  import zipfile
  tmp = out.with_name(out.name + '.unsigned')
  with zipfile.ZipFile(aab) as zin, zipfile.ZipFile(tmp, 'w') as zout:
    for item in zin.infolist():
      name = item.filename
      if name.startswith('META-INF/') and (
          name == 'META-INF/MANIFEST.MF' or
          name.rsplit('.', 1)[-1] in ('SF', 'RSA', 'DSA', 'EC')):
        continue
      zout.writestr(item, zin.read(name))
  try:
    password = password_file.read_text().strip()
    _run(['jarsigner', '-keystore', str(keystore), '-storepass', password,
          '-sigalg', 'SHA256withRSA', '-digestalg', 'SHA-256',
          '-signedjar', str(out), str(tmp), alias], src)
  finally:
    if tmp.exists():
      tmp.unlink()


def bundle_signer_sha256(src: Path, aab: Path) -> str:
  """The SHA-256 of the certificate |aab| is signed with, as a JAR."""
  return parse_keytool_sha256(_run(['keytool', '-printcert', '-jarfile',
                                    str(aab)], src))
