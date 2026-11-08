# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Applying, reverting and exporting patches to upstream files.

patches/<file-path-with-dashes>.patch         applies to src/
patches/<repo>/<file-path-with-dashes>.patch  applies to src/<repo>/ (a nested
                                              git repo such as v8)
"""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Dict, Iterable, List

from .process import git

SUFFIX = '.patch'
APPLIED, NOT_APPLIED, CONFLICT = 'applied', 'not applied', 'conflict'


def patch_name(path: str) -> str:
  return path.replace('/', '-') + SUFFIX


def patches_by_repo(patches_dir: Path, src: Path) -> Dict[Path, List[Path]]:
  """Maps each repository to its patches, in application order."""
  repos: Dict[Path, List[Path]] = {}
  for patch in sorted(patches_dir.rglob('*' + SUFFIX)):
    relative_dir = patch.parent.relative_to(patches_dir)
    repos.setdefault(src / relative_dir, []).append(patch)
  return repos


def state(repo: Path, patch: Path) -> str:
  # Reverse first: a patch that only adds lines can still apply forward once
  # applied, when its context repeats (it did for signing/parts.py, which
  # gained a copy of its hunk on every build).
  if git(['apply', '--check', '-R', patch], repo, check=False).returncode == 0:
    return APPLIED
  if git(['apply', '--check', patch], repo, check=False).returncode == 0:
    return NOT_APPLIED
  return CONFLICT


def apply(patches_dir: Path, src: Path, three_way=False) -> List[Path]:
  """Applies every patch that isn't applied yet. Returns the failures."""
  failures = []
  for repo, patches in patches_by_repo(patches_dir, src).items():
    for patch in patches:
      current = state(repo, patch)
      if current == APPLIED:
        continue
      if current == NOT_APPLIED:
        git(['apply', patch], repo)
        print('applied   ' + _display(patch, patches_dir))
        continue
      if three_way:
        result = git(['apply', '--3way', patch], repo, check=False)
        if result.returncode == 0:
          print('merged    ' + _display(patch, patches_dir))
          continue
      print('FAILED    ' + _display(patch, patches_dir))
      failures.append(patch)
  return failures


def revert(patches_dir: Path, src: Path) -> List[Path]:
  """Reverts every applied patch, last first. Returns the failures."""
  failures = []
  for repo, patches in reversed(list(patches_by_repo(patches_dir,
                                                     src).items())):
    for patch in reversed(patches):
      current = state(repo, patch)
      if current == NOT_APPLIED:
        continue
      if current == APPLIED:
        git(['apply', '-R', patch], repo)
        print('reverted  ' + _display(patch, patches_dir))
        continue
      print('FAILED    ' + _display(patch, patches_dir))
      failures.append(patch)
  return failures


def changed_files(repo: Path, exclude: Iterable[str] = ()) -> List[str]:
  tracked = git(['diff', '--name-only', 'HEAD'], repo).stdout.split('\n')
  untracked = git(['ls-files', '--others', '--exclude-standard'],
                  repo).stdout.split('\n')
  excluded = set(exclude)
  # Nested repositories show up as untracked directories; they get their own
  # patches subdirectory instead.
  return sorted(path for path in set(tracked + untracked)
                if path and path.split('/')[0] not in excluded and
                not (repo / path).is_dir())


def diff(repo: Path, path: str) -> bytes:
  """Diff of |path| against HEAD, as bytes so line endings are preserved."""
  if (git(['ls-files', '--error-unmatch', path], repo,
          check=False).returncode == 0):
    cmd = ['git', 'diff', '--full-index', '--binary', 'HEAD', '--', path]
  else:
    cmd = ['git', 'diff', '--no-index', '--full-index', '--binary', '--',
           '/dev/null', path]
  result = subprocess.run(cmd, cwd=str(repo), stdout=subprocess.PIPE)
  # `git diff --no-index` exits with 1 when the files differ.
  if result.returncode not in (0, 1):
    raise RuntimeError('git diff failed for ' + path)
  return result.stdout


def export(patches_dir: Path, src: Path,
           extra_repos: Iterable[str] = ()) -> None:
  """Writes one patch per modified file and removes stale patches."""
  repos = set(patches_by_repo(patches_dir, src))
  repos.add(src)
  repos.update(src / repo for repo in extra_repos)
  for repo in sorted(repos):
    target_dir = patches_dir / repo.relative_to(src)
    exclude = ('compass',) if repo == src else ()
    wanted = set()
    for path in changed_files(repo, exclude):
      content = diff(repo, path)
      if not content:
        continue
      name = patch_name(path)
      wanted.add(name)
      target = target_dir / name
      if not target.exists() or target.read_bytes() != content:
        target_dir.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        print('wrote     ' + _display(target, patches_dir))
    if target_dir.is_dir():
      for stale in target_dir.glob('*' + SUFFIX):
        if stale.name not in wanted:
          stale.unlink()
          print('removed   ' + _display(stale, patches_dir))


def target_path(patch: Path) -> str:
  """The file |patch| changes, relative to its repository."""
  old = None
  for line in patch.read_bytes().split(b'\n'):
    if line.startswith(b'+++ b/'):
      return line[len(b'+++ b/'):].decode()
    if line.startswith(b'--- a/') and old is None:
      old = line[len(b'--- a/'):].decode()
  if old is not None:
    return old
  # A binary patch has no ---/+++ lines, only "diff --git a/<path> b/<path>".
  first = patch.read_bytes().split(b'\n', 1)[0]
  if first.startswith(b'diff --git a/'):
    paths = first[len(b'diff --git a/'):].decode()
    half = (len(paths) - len(' b/')) // 2
    if paths[half:half + len(' b/')] == ' b/':
      return paths[:half]
  raise ValueError('no file in ' + str(patch))


def past_versions(patches_dir: Path, patch: Path) -> List[bytes]:
  """Every committed version of |patch| (which may be gone now), newest
  first, from the history of the repository holding |patches_dir|. Empty if
  that isn't a git repository."""
  root = patches_dir.parent
  if git(['rev-parse', '--git-dir'], root, check=False).returncode != 0:
    return []
  relative = patch.relative_to(root).as_posix()
  revisions = git(['log', '--format=%H', '--', relative], root,
                  check=False).stdout.split()
  versions = []
  for revision in revisions:
    result = subprocess.run(['git', 'show', revision + ':' + relative],
                            cwd=str(root), stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    if result.returncode == 0:
      versions.append(result.stdout)
  return versions


def same_change(a: bytes, b: bytes) -> bool:
  """Whether diffs |a| and |b| make the same change.

  Ignores the function name git prints after a hunk's "@@ ... @@": it comes
  from the diff driver, which differs between machines (e.g. an Objective-C
  driver for .mm files on one Mac and none on Windows), so a patch exported
  on one machine wouldn't match the same change diffed on another.
  """
  def normalized(text: bytes) -> List[bytes]:
    lines = []
    for line in text.replace(b'\r\n', b'\n').split(b'\n'):
      if line.startswith(b'@@ '):
        end = line.find(b' @@', 3)
        if end != -1:
          line = line[:end + 3]
      lines.append(line)
    return lines
  return normalized(a) == normalized(b)


def same_edits(a: bytes, b: bytes) -> bool:
  """Whether diffs |a| and |b| add and remove the same lines, ignoring
  headers, hunk positions and context."""
  def edits(text: bytes) -> List[bytes]:
    return [line for line in text.replace(b'\r\n', b'\n').split(b'\n')
            if line[:1] in (b'+', b'-')
            and not line.startswith((b'+++ ', b'--- '))]
  return edits(a) == edits(b)


def matches_a_version(change: bytes, versions: List[bytes]) -> bool:
  """Whether |change| (the checkout's diff) is one of |versions| applied.

  Also when only the added and removed lines match: a version written by
  hand has other context and index lines than git diff gives for the same
  change. Edits that aren't exported add lines of their own, so they still
  don't match.
  """
  return any(same_change(change, version) or same_edits(change, version)
             for version in versions)


def restore(repo: Path, path: str) -> None:
  """Puts |path| back as the checkout has it (removes it if it's new)."""
  if git(['ls-files', '--error-unmatch', path], repo,
         check=False).returncode == 0:
    git(['checkout', 'HEAD', '--', path], repo)
  else:
    (repo / path).unlink()


def sync(patches_dir: Path, src: Path) -> List[str]:
  """Brings the checkout in line with patches/, as after a pull.

  - A patch that isn't applied is applied.
  - A patch that conflicts because its file still has an older version of
    it is updated: the file goes back to the checkout's version and the patch
    is applied.
  - A file changed by a patch that's gone is put back.

  Only changes that match a committed version of a patch are undone, so
  edits of the checkout that aren't exported are never lost. Returns the
  problems left, empty if none.
  """
  problems = []
  for repo, repo_patches in patches_by_repo(patches_dir, src).items():
    for patch in repo_patches:
      current = state(repo, patch)
      path = target_path(patch)
      if current == APPLIED:
        if same_edits(diff(repo, path), patch.read_bytes()):
          continue
        # The patch reverses, but the file has more: when a new version's
        # hunks still fit around the old version's, the old one stays
        # underneath (it kept an ungated dep in chrome/browser/ui/startup).
        git(['apply', '-R', patch], repo)
        if matches_a_version(diff(repo, path),
                             past_versions(patches_dir, patch)):
          restore(repo, path)
          git(['apply', patch], repo)
          print('updated   ' + _display(patch, patches_dir))
        else:
          git(['apply', patch], repo)
        continue
      if current == NOT_APPLIED:
        if diff(repo, path) and matches_a_version(
            diff(repo, path), past_versions(patches_dir, patch)):
          # An older version would stay under the new one.
          restore(repo, path)
          git(['apply', patch], repo)
          print('updated   ' + _display(patch, patches_dir))
        else:
          git(['apply', patch], repo)
          print('applied   ' + _display(patch, patches_dir))
        continue
      if matches_a_version(diff(repo, path),
                            past_versions(patches_dir, patch)):
        restore(repo, path)
        if state(repo, patch) == NOT_APPLIED:
          git(['apply', patch], repo)
          print('updated   ' + _display(patch, patches_dir))
          continue
      problems.append('%s conflicts with %s in %s' %
                      (_display(patch, patches_dir), path, repo))

  # Patches deleted from patches/ leave their change behind.
  repos = set(patches_by_repo(patches_dir, src))
  repos.add(src)
  for repo in sorted(repos):
    patch_dir = patches_dir / repo.relative_to(src)
    exclude = ('compass',) if repo == src else ()
    for path in changed_files(repo, exclude):
      patch = patch_dir / patch_name(path)
      if patch.exists():
        continue
      versions = past_versions(patches_dir, patch)
      if versions and matches_a_version(diff(repo, path), versions):
        restore(repo, path)
        print('removed   ' + _display(patch, patches_dir))
  return problems


def status(patches_dir: Path, src: Path) -> Dict[Path, str]:
  return {patch: state(repo, patch)
          for repo, patches in patches_by_repo(patches_dir, src).items()
          for patch in patches}


def _display(patch: Path, patches_dir: Path) -> str:
  return patch.relative_to(patches_dir).as_posix()
