# Copyright 2026 The Compass Authors
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

from compass_tool import overrides, patches  # noqa: E402
from compass_tool.layout import detect  # noqa: E402


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
        'deps = [\n  "//compass/browser",\n  ":a",\n]\n')
    (self.src / 'chrome' / 'new.h').write_text('// added\n')
    (self.src / 'compass').mkdir()
    (self.src / 'compass' / 'ignored.cc').write_text('')

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
    self.assertNotIn('compass', (self.src / 'chrome' / 'BUILD.gn').read_text())
    self.assertFalse((self.src / 'chrome' / 'new.h').exists())

    self.assertEqual(self.quietly(patches.apply, self.patches_dir, self.src),
                     [])
    self.assertIn('//compass/browser',
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


class PatchSyncTest(unittest.TestCase):
  """patches.sync(), which `compass build` runs: the checkout follows
  patches/ after a pull that adds, changes or removes patches."""

  def setUp(self):
    self.tmp = tempfile.TemporaryDirectory()
    # The Compass repository (base, holding patches/) and the Chromium
    # checkout (src, its own repository).
    self.base = Path(self.tmp.name)
    self.src = self.base / 'src'
    self.patches_dir = self.base / 'patches'
    (self.src / 'chrome').mkdir(parents=True)
    self.build_gn = self.src / 'chrome' / 'BUILD.gn'
    self.build_gn.write_text('deps = [\n  ":a",\n]\n')
    git(self.src, 'init', '-q')
    git(self.src, 'add', '-A')
    git(self.src, 'commit', '-qm', 'upstream')
    self.patches_dir.mkdir()
    git(self.base, 'init', '-q')
    (self.base / 'README').write_text('compass\n')
    git(self.base, 'add', 'README')
    git(self.base, 'commit', '-qm', 'compass')

  def tearDown(self):
    self.tmp.cleanup()

  def quietly(self, fn, *args, **kwargs):
    with redirect_stdout(StringIO()):
      return fn(*args, **kwargs)

  # Edits the checkout, exports and commits the patches, as a developer
  # does, then puts the checkout back as it was before the edit.
  def commit_patches(self, build_gn, new_h=None, keep_applied=False):
    before = {p: p.read_text() for p in [self.build_gn]}
    self.build_gn.write_text(build_gn)
    new = self.src / 'chrome' / 'new.h'
    if new_h is not None:
      new.write_text(new_h)
    elif new.exists():
      new.unlink()
    self.quietly(patches.export, self.patches_dir, self.src)
    git(self.base, 'add', '-A', 'patches')
    git(self.base, 'commit', '-qm', 'patches')
    if not keep_applied:
      for path, text in before.items():
        path.write_text(text)
      if new.exists():
        git(self.src, 'clean', '-qf', 'chrome/new.h')

  def test_applies_new_patches(self):
    self.commit_patches('deps = [\n  ":a",\n  ":b",\n]\n', new_h='// new\n')
    problems = self.quietly(patches.sync, self.patches_dir, self.src)
    self.assertEqual(problems, [])
    self.assertIn(':b', self.build_gn.read_text())
    self.assertTrue((self.src / 'chrome' / 'new.h').exists())

  def test_updates_patches_changed_by_a_pull(self):
    self.commit_patches('deps = [\n  ":a",\n  ":b",\n]\n', new_h='// v1\n',
                        keep_applied=True)
    # A pull brings a new version of both patches; the checkout still has
    # the old ones.
    self.commit_patches('deps = [\n  ":c",\n  ":a",\n]\n', new_h='// v2\n')
    self.build_gn.write_text('deps = [\n  ":a",\n  ":b",\n]\n')
    (self.src / 'chrome' / 'new.h').write_text('// v1\n')

    problems = self.quietly(patches.sync, self.patches_dir, self.src)
    self.assertEqual(problems, [])
    self.assertEqual(self.build_gn.read_text(), 'deps = [\n  ":c",\n  ":a",\n]\n')
    self.assertEqual((self.src / 'chrome' / 'new.h').read_text(), '// v2\n')
    self.assertTrue(all(
        state == patches.APPLIED
        for state in patches.status(self.patches_dir, self.src).values()))

  def test_updates_patches_exported_with_another_diff_driver(self):
    # The old version was exported on a machine whose diff driver puts a
    # function name after the hunk header; this one prints none.
    self.commit_patches('deps = [\n  ":a",\n  ":b",\n]\n', keep_applied=True)
    patch = self.patches_dir / 'chrome-BUILD.gn.patch'
    text = patch.read_bytes()
    self.assertIn(b' @@\n', text)
    patch.write_bytes(text.replace(b' @@\n', b' @@ deps = [\n', 1))
    git(self.base, 'commit', '-qa', '--amend', '--no-edit')
    self.commit_patches('deps = [\n  ":c",\n  ":a",\n]\n')
    self.build_gn.write_text('deps = [\n  ":a",\n  ":b",\n]\n')

    problems = self.quietly(patches.sync, self.patches_dir, self.src)
    self.assertEqual(problems, [])
    self.assertEqual(self.build_gn.read_text(), 'deps = [\n  ":c",\n  ":a",\n]\n')

  def test_same_change_ignores_hunk_header_context(self):
    a = b'--- a/x\n+++ b/x\n@@ -1,2 +1,3 @@ int f() {\n a\n+b\n'
    b = b'--- a/x\n+++ b/x\n@@ -1,2 +1,3 @@\n a\n+b\n'
    self.assertTrue(patches.same_change(a, b))
    self.assertFalse(patches.same_change(a, b.replace(b'+b', b'+c')))

  def test_keeps_edits_that_no_patch_has(self):
    self.commit_patches('deps = [\n  ":a",\n  ":b",\n]\n')
    self.build_gn.write_text('deps = [\n  ":local",\n]\n')
    problems = self.quietly(patches.sync, self.patches_dir, self.src)
    self.assertEqual(len(problems), 1)
    self.assertIn('chrome-BUILD.gn.patch', problems[0])
    self.assertEqual(self.build_gn.read_text(), 'deps = [\n  ":local",\n]\n')

  def test_undoes_patches_removed_by_a_pull(self):
    self.commit_patches('deps = [\n  ":a",\n  ":b",\n]\n', new_h='// new\n',
                        keep_applied=True)
    # A pull removes both patches.
    for patch in self.patches_dir.glob('*.patch'):
      patch.unlink()
    git(self.base, 'add', '-A', 'patches')
    git(self.base, 'commit', '-qm', 'no more patches')

    problems = self.quietly(patches.sync, self.patches_dir, self.src)
    self.assertEqual(problems, [])
    self.assertEqual(self.build_gn.read_text(), 'deps = [\n  ":a",\n]\n')
    self.assertFalse((self.src / 'chrome' / 'new.h').exists())

  def test_leaves_unexported_edits_of_unpatched_files(self):
    (self.src / 'chrome' / 'other.cc').write_text('// mine\n')
    problems = self.quietly(patches.sync, self.patches_dir, self.src)
    self.assertEqual(problems, [])
    self.assertTrue((self.src / 'chrome' / 'other.cc').exists())


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
    layout = detect(Path('/work/Compass'), environ={})
    self.assertFalse(layout.in_tree)
    self.assertEqual(layout.src, Path('/work/compass-chromium/src'))

  def test_environment_overrides_chromium_dir(self):
    layout = detect(Path('/work/Compass'),
                    environ={'COMPASS_CHROMIUM_DIR': '/big/disk/cr'})
    # resolve() adds the current drive on Windows.
    self.assertEqual(layout.src, Path('/big/disk/cr').resolve() / 'src')

  def test_detects_in_tree_layout(self):
    with tempfile.TemporaryDirectory() as tmp:
      src = Path(tmp) / 'src'
      (src / 'compass').mkdir(parents=True)
      (src / '.gn').write_text('')
      layout = detect(src / 'compass', environ={})
      self.assertTrue(layout.in_tree)
      self.assertEqual(layout.src, src)


if __name__ == '__main__':
  unittest.main()
