# Copyright 2026 The Strata Authors
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
  if git(['apply', '--check', patch], repo, check=False).returncode == 0:
    return NOT_APPLIED
  if git(['apply', '--check', '-R', patch], repo, check=False).returncode == 0:
    return APPLIED
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
    exclude = ('strata',) if repo == src else ()
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


def status(patches_dir: Path, src: Path) -> Dict[Path, str]:
  return {patch: state(repo, patch)
          for repo, patches in patches_by_repo(patches_dir, src).items()
          for patch in patches}


def _display(patch: Path, patches_dir: Path) -> str:
  return patch.relative_to(patches_dir).as_posix()
