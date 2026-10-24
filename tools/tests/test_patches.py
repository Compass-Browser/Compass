# Copyright 2026 The Strata Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from strata_tool import overrides, patches  # noqa: E402
from strata_tool.layout import detect  # noqa: E402


def git(repo, *args):
  subprocess.run(['git', '-c', 'user.name=t', '-c', 'user.email=t@t'] +
                 list(args), cwd=repo, check=True, capture_output=True)


class PatchesTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.TemporaryDirectory()
    base = Path(self.tmp.name)
    self.src = base / 'src'
    self.patches_dir = base / 'patches'
    (self.src / 'chrome').mkdir(parents=True)
    (self.src / 'chrome' / 'BUILD.gn').write_text('deps = [\n  ":a",\n]\n')
    (self.src / 'README').write_text('upstream\n')
    git(self.src, 'init', '-q')
    git(self.src, 'add', '-A')
    git(self.src, 'commit', '-qm', 'upstream')
    self.patches_dir.mkdir()

  def tearDown(self):
    self.tmp.cleanup()

  def quietly(self, fn, *args, **kwargs):
    with redirect_stdout(StringIO()):
      return fn(*args, **kwargs)

  def modify(self):
    (self.src / 'chrome' / 'BUILD.gn').write_text(
        'deps = [\n  "//strata/browser",\n  ":a",\n]\n')
    (self.src / 'chrome' / 'new.h').write_text('// added\n')
    (self.src / 'strata').mkdir()
    (self.src / 'strata' / 'ignored.cc').write_text('')

  def test_export_writes_one_patch_per_file(self):
    self.modify()
    self.quietly(patches.export, self.patches_dir, self.src)
    names = sorted(p.name for p in self.patches_dir.glob('*.patch'))
    self.assertEqual(names, ['chrome-BUILD.gn.patch', 'chrome-new.h.patch'])

  def test_round_trip_revert_and_apply(self):
    self.modify()
    self.quietly(patches.export, self.patches_dir, self.src)

    self.assertEqual(self.quietly(patches.revert, self.patches_dir, self.src),
                     [])
    self.assertNotIn('strata', (self.src / 'chrome' / 'BUILD.gn').read_text())
    self.assertFalse((self.src / 'chrome' / 'new.h').exists())

    self.assertEqual(self.quietly(patches.apply, self.patches_dir, self.src),
                     [])
    self.assertIn('//strata/browser',
                  (self.src / 'chrome' / 'BUILD.gn').read_text())
    self.assertTrue((self.src / 'chrome' / 'new.h').exists())

  def test_apply_is_idempotent(self):
    self.modify()
    self.quietly(patches.export, self.patches_dir, self.src)
    self.assertEqual(self.quietly(patches.apply, self.patches_dir, self.src),
                     [])
    states = set(patches.status(self.patches_dir, self.src).values())
    self.assertEqual(states, {patches.APPLIED})

  def test_export_removes_stale_patches(self):
    self.modify()
    self.quietly(patches.export, self.patches_dir, self.src)
    (self.src / 'chrome' / 'new.h').unlink()
    self.quietly(patches.export, self.patches_dir, self.src)
    self.assertFalse((self.patches_dir / 'chrome-new.h.patch').exists())

  def test_reports_conflicts(self):
    self.modify()
    self.quietly(patches.export, self.patches_dir, self.src)
    self.quietly(patches.revert, self.patches_dir, self.src)
    (self.src / 'chrome' / 'BUILD.gn').write_text('something else\n')
    git(self.src, 'commit', '-qam', 'upstream moved on')
    failures = self.quietly(patches.apply, self.patches_dir, self.src)
    self.assertEqual([p.name for p in failures], ['chrome-BUILD.gn.patch'])

  def test_nested_repo_patches_apply_in_that_repo(self):
    v8 = self.src / 'v8'
    v8.mkdir()
    (v8 / 'v8.h').write_text('int x;\n')
    git(v8, 'init', '-q')
    git(v8, 'add', '-A')
    git(v8, 'commit', '-qm', 'v8')
    (v8 / 'v8.h').write_text('int x;\nint y;\n')
    self.quietly(patches.export, self.patches_dir, self.src, ['v8'])
    self.assertTrue((self.patches_dir / 'v8' / 'v8.h.patch').exists())
    self.quietly(patches.revert, self.patches_dir, self.src)
    self.assertEqual((v8 / 'v8.h').read_text(), 'int x;\n')


class OverridesTest(unittest.TestCase):

  def test_touches_upstream_when_override_appears_or_disappears(self):
    with tempfile.TemporaryDirectory() as tmp:
      base = Path(tmp)
      src, out, overrides_dir = base / 'src', base / 'out', base / 'cs'
      (src / 'chrome').mkdir(parents=True)
      (overrides_dir / 'chrome').mkdir(parents=True)
      (src / 'chrome' / 'a.cc').write_text('')
      override = overrides_dir / 'chrome' / 'a.cc'

      self.assertEqual(overrides.sync(overrides_dir, src, out), [])
      override.write_text('')
      self.assertEqual(overrides.sync(overrides_dir, src, out),
                       ['chrome/a.cc'])
      self.assertEqual(overrides.sync(overrides_dir, src, out), [])
      override.unlink()
      self.assertEqual(overrides.sync(overrides_dir, src, out),
                       ['chrome/a.cc'])

  def test_finds_orphans(self):
    with tempfile.TemporaryDirectory() as tmp:
      base = Path(tmp)
      (base / 'src').mkdir()
      (base / 'cs' / 'gone').mkdir(parents=True)
      (base / 'cs' / 'gone' / 'x.cc').write_text('')
      (base / 'cs' / 'README.md').write_text('')
      self.assertEqual(overrides.orphans(base / 'cs', base / 'src'),
                       ['gone/x.cc'])


class LayoutTest(unittest.TestCase):

  def test_linked_layout_defaults_to_sibling_directory(self):
    layout = detect(Path('/work/Strata'), environ={})
    self.assertFalse(layout.in_tree)
    self.assertEqual(layout.src, Path('/work/strata-chromium/src'))

  def test_environment_overrides_chromium_dir(self):
    layout = detect(Path('/work/Strata'),
                    environ={'STRATA_CHROMIUM_DIR': '/big/disk/cr'})
    self.assertEqual(layout.src, Path('/big/disk/cr/src'))

  def test_detects_in_tree_layout(self):
    with tempfile.TemporaryDirectory() as tmp:
      src = Path(tmp) / 'src'
      (src / 'strata').mkdir(parents=True)
      (src / '.gn').write_text('')
      layout = detect(src / 'strata', environ={})
      self.assertTrue(layout.in_tree)
      self.assertEqual(layout.src, src)


if __name__ == '__main__':
  unittest.main()
