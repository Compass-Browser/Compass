#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Builds BergamotKit.xcframework, the translation engine for iOS.

The desktop builds Bergamot with GN in the Chromium checkout
(third_party/bergamot/BUILD.gn); a Chromium iOS build would need the iOS
checkout. So this asks the Mac arm64 build's GN for the engine's targets
(Bergamot, Marian, ssplit, PCRE2, SentencePiece, protobuf-lite, ruy,
cpuinfo, Abseil), their sources, defines and include folders, and compiles
them with Xcode's clang and Apple's libc++ for the device and the
simulator, with the C API in components/translatekit/ios/. The generated
files (PCRE2's headers, SentencePiece's protos) come from that build's gen/,
so it must have built the engine once (`compass build` of the app or
`translatekit_cli`).

  tools/translation/build_ios.py [--out-dir out/Release-arm64] [-j 10]

Writes ios/Frameworks/BergamotKit.xcframework (not checked in), with
headers and a module map, and its objects under
<checkout>/out/ios-bergamot/.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys

COMPASS = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(os.path.dirname(COMPASS), 'compass-chromium', 'src')
GN = os.path.join(SRC, 'buildtools', 'mac', 'gn')

ENGINE = '//compass/components/translatekit:engine'
# The targets whose sources go in: the engine's, without the toolchain's.
KEEP = ('//compass/', '//third_party/abseil-cpp', '//third_party/cpuinfo',
        '//third_party/protobuf', '//third_party/ruy',
        '//third_party/sentencepiece', '//third_party/zstd')
SKIP = ('//third_party/protobuf-javascript',)
# Chromium's, which Apple's toolchain doesn't want.
DROP_DEFINE_PREFIXES = ('_LIBCPP_', '_LIBCXXABI_', 'CR_', 'COMPONENT_BUILD',
                        'OFFICIAL_BUILD', 'DCHECK_ALWAYS_ON',
                        'PROTOBUF_USE_DLLS', 'ABSL_CONSUME_DLL',
                        'ABSL_BUILD_DLL')
MIN_IOS = '17.0'

SLICES = {
    'ios-arm64': ('iphoneos', f'arm64-apple-ios{MIN_IOS}'),
    'ios-arm64-simulator': ('iphonesimulator', f'arm64-apple-ios{MIN_IOS}-simulator'),
}

EXTRA_SOURCES = [
    '//compass/components/translatekit/ios/bergamot_kit.cc',
]


def gn_json(out_dir, label):
    result = subprocess.run([GN, 'desc', out_dir, label, '--format=json'],
                            cwd=SRC, check=True, capture_output=True, text=True)
    return json.loads(result.stdout)[label]


def gn_deps(out_dir, root):
    result = subprocess.run([GN, 'desc', out_dir, root, 'deps', '--all'],
                            cwd=SRC, check=True, capture_output=True, text=True)
    labels = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return [label for label in labels
            if label.startswith(KEEP) and not label.startswith(SKIP)]


def path_of(label_path, out_dir, sdk_path):
    """A GN path ("//a/b") as a file path; the Mac SDK's frameworks as the
    iOS SDK's."""
    if '/sdk/xcode_links/' in label_path:
        rest = label_path.split('.sdk/', 1)[1]
        return os.path.join(sdk_path, rest)
    assert label_path.startswith('//'), label_path
    return os.path.join(SRC, label_path[2:])


def targets(out_dir):
    # zstd: the models' compression (bk_zstd_decompress_file()).
    roots = [ENGINE, '//third_party/zstd:decompress']
    labels = list(dict.fromkeys(roots + [d for root in roots for d in gn_deps(out_dir, root)]))
    print(f'{len(labels)} targets', flush=True)
    described = {}
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        for label, desc in zip(labels, pool.map(lambda l: gn_json(out_dir, l), labels)):
            described[label] = desc
    return described


def compile_jobs(described, out_dir, slice_name, sdk, triple, sdk_path, obj_dir):
    jobs = []
    seen = set()
    engine_flags = None
    for label, desc in described.items():
        defines = [d for d in desc.get('defines', [])
                   if not d.startswith(DROP_DEFINE_PREFIXES)]
        includes = [path_of(i, out_dir, sdk_path) for i in desc.get('include_dirs', [])
                    if 'buildtools/third_party/libc++' not in i]
        cxx_std = '-std=c++23'
        exceptions = '-fexceptions' in desc.get('cflags_cc', [])
        rtti = '-frtti' in desc.get('cflags_cc', [])
        flags = ([f'-D{d}' for d in defines] + [f'-I{i}' for i in includes])
        if label == ENGINE:
            engine_flags = (flags, exceptions, rtti)
        for source in desc.get('sources', []):
            if not source.endswith(('.c', '.cc', '.cpp')):
                continue
            jobs.append((source, flags, exceptions, rtti))
            seen.add(source)
    for source in EXTRA_SOURCES:
        flags, exceptions, rtti = engine_flags
        jobs.append((source, flags, exceptions, rtti))
    commands = []
    for source, flags, exceptions, rtti in jobs:
        path = path_of(source, out_dir, sdk_path)
        name = hashlib.sha1(source.encode()).hexdigest()[:10] + '_' + os.path.basename(path) + '.o'
        obj = os.path.join(obj_dir, name)
        is_c = path.endswith('.c')
        command = ['xcrun', '--sdk', sdk, 'clang' if is_c else 'clang++',
                   '-target', triple, '-c', path, '-o', obj,
                   '-O2', '-g0', '-fPIC', '-fvisibility=hidden',
                   '-fno-strict-aliasing', '-Wno-everything']
        if is_c:
            command.append('-std=c11')
        else:
            command += ['-std=c++23', '-fvisibility-inlines-hidden',
                        '-fexceptions' if exceptions else '-fno-exceptions',
                        '-frtti' if rtti else '-fno-rtti']
        commands.append((command + flags, obj, source))
    return commands


def run(command):
    result = subprocess.run(command[0], capture_output=True, text=True)
    return command, result


def build_slice(described, out_dir, slice_name, jobs_count, build_root):
    sdk, triple = SLICES[slice_name]
    sdk_path = subprocess.run(['xcrun', '--sdk', sdk, '--show-sdk-path'],
                              check=True, capture_output=True, text=True).stdout.strip()
    obj_dir = os.path.join(build_root, slice_name, 'obj')
    os.makedirs(obj_dir, exist_ok=True)
    commands = compile_jobs(described, out_dir, slice_name, sdk, triple, sdk_path, obj_dir)
    print(f'{slice_name}: {len(commands)} files', flush=True)
    failures = []
    done = 0
    with concurrent.futures.ThreadPoolExecutor(jobs_count) as pool:
        for command, result in pool.map(run, commands):
            done += 1
            if result.returncode:
                failures.append((command[2], result.stderr))
            if done % 100 == 0:
                print(f'  {done}/{len(commands)}', flush=True)
    if failures:
        for source, stderr in failures[:10]:
            print(f'FAILED {source}\n{stderr[:3000]}', file=sys.stderr)
        sys.exit(f'{len(failures)} files failed')
    library = os.path.join(build_root, slice_name, 'libBergamotKit.a')
    if os.path.exists(library):
        os.remove(library)
    objects = [obj for _, obj, _ in commands]
    subprocess.run(['xcrun', 'libtool', '-static', '-no_warning_for_no_symbols',
                    '-o', library] + objects, check=True)
    return library


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out-dir', default='out/Release-arm64')
    parser.add_argument('-j', type=int, default=10)
    args = parser.parse_args()
    out_dir = args.out_dir
    build_root = os.path.join(SRC, 'out', 'ios-bergamot')
    described = targets(out_dir)
    libraries = [build_slice(described, out_dir, s, args.j, build_root) for s in SLICES]

    headers = os.path.join(build_root, 'include')
    shutil.rmtree(headers, ignore_errors=True)
    os.makedirs(headers)
    shutil.copy(os.path.join(COMPASS, 'components', 'translatekit', 'ios', 'bergamot_kit.h'), headers)
    with open(os.path.join(headers, 'module.modulemap'), 'w') as f:
        f.write('module BergamotKit {\n  header "bergamot_kit.h"\n  link "c++"\n'
                '  link framework "Accelerate"\n  export *\n}\n')
    output = os.path.join(COMPASS, 'ios', 'Frameworks', 'BergamotKit.xcframework')
    shutil.rmtree(output, ignore_errors=True)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    command = ['xcodebuild', '-create-xcframework']
    for library in libraries:
        command += ['-library', library, '-headers', headers]
    command += ['-output', output]
    subprocess.run(command, check=True)
    print(output)


if __name__ == '__main__':
    main()
