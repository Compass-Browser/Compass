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


if __name__ == '__main__':
  unittest.main()
