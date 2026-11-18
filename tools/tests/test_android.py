# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compass_tool import android  # noqa: E402


class MacHostTest(unittest.TestCase):

  def test_is_android(self):
    self.assertTrue(android.is_android('android-arm64'))
    self.assertTrue(android.is_android('android-x64'))
    self.assertFalse(android.is_android('win-x64'))
    self.assertFalse(android.is_android(None))

  def test_link_jdk(self):
    with tempfile.TemporaryDirectory() as tmp:
      src = Path(tmp)
      home = src / 'third_party/jdk/current/Contents/Home'
      (home / 'bin').mkdir(parents=True)
      (home / 'lib').mkdir()
      (home / 'bin' / 'javac').write_text('')
      self.assertEqual(['bin', 'lib'], android.link_jdk(src))
      current = src / 'third_party/jdk/current'
      self.assertTrue((current / 'bin' / 'javac').exists())
      self.assertEqual(Path('Contents/Home/bin'), Path(os.readlink(current / 'bin')))
      self.assertEqual([], android.link_jdk(src))  # Idempotent.

  def test_link_jdk_on_a_linux_jdk(self):
    with tempfile.TemporaryDirectory() as tmp:
      (Path(tmp) / 'third_party/jdk/current/bin').mkdir(parents=True)
      self.assertEqual([], android.link_jdk(Path(tmp)))

  def test_use_sdk_aapt2(self):
    with tempfile.TemporaryDirectory() as tmp:
      src = Path(tmp)
      pinned_dir = src / 'third_party/android_build_tools/aapt2/cipd'
      pinned_dir.mkdir(parents=True)
      pinned = pinned_dir / 'aapt2'
      pinned.write_bytes(android.ELF_MAGIC + b'rest')
      for version in ('36.0.0', '37.0.0'):
        tools = src / 'third_party/android_sdk/public/build-tools' / version
        tools.mkdir(parents=True)
        (tools / 'aapt2').write_bytes(b'\xcf\xfa\xed\xfe')
      self.assertTrue(android.use_sdk_aapt2(src))
      self.assertTrue(pinned.is_symlink())
      self.assertEqual(
          (src / 'third_party/android_sdk/public/build-tools/37.0.0/aapt2'
          ).resolve(), pinned.resolve())
      self.assertTrue((pinned_dir / 'aapt2.linux').is_file())
      self.assertFalse(android.use_sdk_aapt2(src))  # Idempotent.

  def test_use_sdk_aapt2_keeps_a_mac_binary(self):
    with tempfile.TemporaryDirectory() as tmp:
      pinned_dir = Path(tmp) / 'third_party/android_build_tools/aapt2/cipd'
      pinned_dir.mkdir(parents=True)
      (pinned_dir / 'aapt2').write_bytes(b'\xcf\xfa\xed\xfe')
      self.assertFalse(android.use_sdk_aapt2(Path(tmp)))



class ReleaseApkTest(unittest.TestCase):

  def test_parse_version_code(self):
    self.assertEqual(1263220819, android.parse_version_code(
        "package: name='it.cipi1965.compass' versionCode='1263220819' "
        "versionName='157.0.8103.51'"))
    with self.assertRaises(android.ApkError):
      android.parse_version_code("package: name='x'")

  def test_signer_and_keystore_digests_compare(self):
    digest = '2a33a6aae3e4f8bc5f560dc176eb049cb72f4f5977ccc5cb2d3856794c712c66'
    apksigner = ('Signer #1 certificate DN: CN=Compass, O=Compass Browser\n'
                 'Signer #1 certificate SHA-256 digest: %s\n' % digest)
    keytool = ('Certificate fingerprints:\n\t SHA1: 00:11\n\t SHA256: %s\n'
               % ':'.join(digest[i:i + 2] for i in range(0, 64, 2)).upper())
    self.assertEqual(digest, android.parse_signer_sha256(apksigner))
    self.assertEqual(digest, android.parse_keytool_sha256(keytool))
    with self.assertRaises(android.ApkError):
      android.parse_signer_sha256('DOES NOT VERIFY')

  def test_build_tools_picks_the_newest(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = Path(tmp) / 'third_party/android_sdk/public/build-tools'
      for version in ('9.0.0', '36.1.0', '37.0.0'):
        (root / version).mkdir(parents=True)
        (root / version / 'apksigner').write_text('')
      self.assertEqual('37.0.0', android.build_tools(Path(tmp)).name)


if __name__ == '__main__':
  unittest.main()
