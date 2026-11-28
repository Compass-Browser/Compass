#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Vendors Bergamot into third_party/bergamot/.

  update_bergamot.py [--keep-temp]

Fetches each project below at its pinned revision, copies the files page
translation needs (docs/design/translation.md, "Bergamot") into
third_party/bergamot/, replacing what's there, then applies
third_party/bergamot/patches/*.patch in name order. To move to a newer
Bergamot, change the revisions (the Bergamot one is what Firefox pins in
toolkit/components/translations/bergamot-translator/moz.yaml, and its
submodules' are the gitlinks at that revision), run this, and fix the
patches that no longer apply.

Left out on purpose:
- Marian's training code, faiss, LSH, ONNX export and gzip: the build
  defines WASM_COMPATIBLE_SOURCE, which compiles them out.
- Marian's copies of ruy, cpuinfo, zlib, SentencePiece and its
  protobuf-lite: Chromium's own are used.
- CUDA, FBGEMM, NCCL, the WASM bindings, emsdk, tests, benchmarks.
"""

import argparse
import fnmatch
import glob
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), 'third_party', 'bergamot')

# name -> (repository, revision)
SOURCES = {
    'translations': ('https://github.com/mozilla/translations.git',
                     'eea6e5a80aa4ddd86d9cc35ce9a65b79aa3ab96d'),
    'ssplit': ('https://github.com/browsermt/ssplit-cpp',
               'a311f9865ade34db1e8e080e6cc146f55dafb067'),
    'intgemm': ('https://github.com/kpu/intgemm',
                'f7401513da71758dacce52fed1c7855549abee59'),
    'onnxjs': ('https://github.com/browsermt/onnxjs',
               '924924b08e9596b41aeebada4a172f026be95f5a'),
    'simd_utils': ('https://github.com/browsermt/simd_utils.git',
                   'd0793d86aea9036a5bc77b9ca7791dff024168ca'),
    'pcre2': ('https://github.com/PCRE2Project/pcre2.git',
              '388ef3c412ab5269b8f76101a6bef2d416974a15'),  # pcre2-10.49
}

MARIAN = 'translations/inference/marian-fork'
MARIAN_DIRS = ['common', 'data', 'graph', 'layers', 'models', 'rnn',
               'translator', 'functional']
SOURCE_PATTERNS = ['*.h', '*.hpp', '*.cpp', '*.cc', '*.c', '*.in', '*.inc']

# (source under the temporary directory, destination under ROOT, patterns
# or None for everything, recursive)
COPIES = [
    ('translations/LICENSE', 'LICENSE', None, False),
    ('translations/inference/BERGAMOT_VERSION', 'BERGAMOT_VERSION', None,
     False),
    ('translations/inference/src/translator', 'src/translator',
     SOURCE_PATTERNS, False),
    (MARIAN + '/LICENSE.md', 'marian/LICENSE.md', None, False),
    (MARIAN + '/VERSION', 'marian/VERSION', None, False),
] + [
    (MARIAN + '/src/' + d, 'marian/src/' + d, SOURCE_PATTERNS, True)
    for d in MARIAN_DIRS
] + [
    (MARIAN + '/src/tensors', 'marian/src/tensors', SOURCE_PATTERNS, False),
    (MARIAN + '/src/tensors/cpu', 'marian/src/tensors/cpu', SOURCE_PATTERNS,
     False),
    (MARIAN + '/src/tensors/cpu/fbgemm', 'marian/src/tensors/cpu/fbgemm',
     ['*.h'], False),  # Included; their FBGEMM code is behind USE_FBGEMM.
    (MARIAN + '/src/marian.h', 'marian/src/marian.h', None, False),
    (MARIAN + '/src/training/training_state.h',
     'marian/src/training/training_state.h', None, False),
    # Marian's own small third-party code.
    (MARIAN + '/src/3rd_party', 'marian/src/3rd_party',
     ['any_type.h', 'avx_mathfun.h', 'sse_mathfun.h', 'threadpool.h',
      'ExceptionWithCallStack.h'], False),
] + [
    (MARIAN + '/src/3rd_party/' + d, 'marian/src/3rd_party/' + d, None, True)
    for d in ['CLI', 'yaml-cpp', 'spdlog', 'pathie-cpp', 'phf', 'half_float',
              'mio', 'cnpy']
] + [
    # Submodules.
    ('intgemm/intgemm', 'marian/src/3rd_party/intgemm/intgemm', None, True),
    ('intgemm/LICENSE', 'marian/src/3rd_party/intgemm/LICENSE', None, False),
    ('simd_utils', 'marian/src/3rd_party/simd_utils', ['*.h', 'LICENSE*'],
     False),
    ('onnxjs/LICENSE', 'marian/src/3rd_party/onnxjs/LICENSE', None, False),
    ('onnxjs/src/wasm-ops', 'marian/src/3rd_party/onnxjs/src/wasm-ops',
     ['common.h', 'gemm.h', 'gemm.cpp'], False),
    ('onnxjs/src/wasm-ops/utils',
     'marian/src/3rd_party/onnxjs/src/wasm-ops/utils', SOURCE_PATTERNS, True),
    ('ssplit/LICENSE.md', 'ssplit/LICENSE.md', None, False),
    ('ssplit/src/ssplit', 'ssplit/src/ssplit', SOURCE_PATTERNS, False),
    ('pcre2/LICENCE.md', 'pcre2/LICENCE.md', None, False),
    ('pcre2/src', 'pcre2/src',
     ['pcre2_*.c', 'pcre2_*.h', 'pcre2_chartables.c.dist', 'pcre2.h.generic',
      'config.h.generic', 'pcre2.h.in'], False),
]

# Never copied, whatever the patterns say.
EXCLUDE = ['*/CMakeLists.txt', '*.cu', '*Test.cpp', '*/test/*', '*/tests/*',
           '*/benchmarks/*', 'pcre2/src/pcre2test*', 'pcre2/src/pcre2grep*',
           'pcre2/src/pcre2_fuzzsupport.c', 'pcre2/src/pcre2_jit_test.c',
           'pcre2/src/pcre2_dftables.c', 'pcre2/src/pcre2posix*',
           'pcre2/src/pcre2demo.c', 'pcre2/src/pcre2_printint.c',
           'pcre2/src/sljit/*']


def run(args, cwd=None):
  subprocess.run(args, cwd=cwd, check=True, stdout=subprocess.DEVNULL)


def fetch(temp):
  for name, (url, revision) in SOURCES.items():
    path = os.path.join(temp, name)
    os.makedirs(path)
    run(['git', 'init', '-q'], cwd=path)
    run(['git', 'fetch', '-q', '--depth', '1', url, revision], cwd=path)
    run(['git', '-c', 'advice.detachedHead=false', 'checkout', '-q',
         'FETCH_HEAD'], cwd=path)
    print('fetched %s %s' % (name, revision[:12]))


def excluded(relative):
  return any(fnmatch.fnmatch(relative, p) for p in EXCLUDE)


def copy_file(source, destination):
  os.makedirs(os.path.dirname(destination), exist_ok=True)
  shutil.copyfile(source, destination)


def copy(temp):
  count = 0
  for source, destination, patterns, recursive in COPIES:
    source_path = os.path.join(temp, source)
    if os.path.isfile(source_path):
      copy_file(source_path, os.path.join(ROOT, destination))
      count += 1
      continue
    if not os.path.isdir(source_path):
      sys.exit('missing in the sources: ' + source)
    for directory, subdirectories, files in os.walk(source_path):
      subdirectories.sort()
      if not recursive:
        subdirectories[:] = []
      for name in sorted(files):
        full = os.path.join(directory, name)
        relative = os.path.relpath(full, temp)
        if excluded(relative):
          continue
        if patterns and not any(fnmatch.fnmatch(name, p) for p in patterns):
          continue
        target = os.path.join(ROOT, destination,
                              os.path.relpath(full, source_path))
        copy_file(full, target)
        count += 1
  return count


def clear():
  """Removes the vendored files, keeping ours (README, BUILD.gn, patches)."""
  keep = {'README.md', 'BUILD.gn', 'patches', 'shims', 'gen'}
  if not os.path.isdir(ROOT):
    os.makedirs(ROOT)
    return
  for name in os.listdir(ROOT):
    if name in keep:
      continue
    path = os.path.join(ROOT, name)
    if os.path.isdir(path):
      shutil.rmtree(path)
    else:
      os.remove(path)


def apply_patches():
  patches = sorted(glob.glob(os.path.join(ROOT, 'patches', '*.patch')))
  # Patches name their files from the repository's root
  # (third_party/bergamot/...).
  repository = os.path.dirname(os.path.dirname(ROOT))
  for patch in patches:
    subprocess.run(['git', 'apply', patch], cwd=repository, check=True)
    print('applied ' + os.path.basename(patch))
  return len(patches)


def main():
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('--keep-temp', action='store_true',
                      help='keep the fetched sources and print where')
  args = parser.parse_args()
  temp = tempfile.mkdtemp(prefix='bergamot-')
  try:
    fetch(temp)
    clear()
    print('copied %d files' % copy(temp))
    apply_patches()
  finally:
    if args.keep_temp:
      print('sources in ' + temp)
    else:
      shutil.rmtree(temp)


if __name__ == '__main__':
  main()
