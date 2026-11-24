#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Builds compass-on-device with Xcode's swiftc.

  build_helper.py --out PATH --cpu arm64|x64 --deployment-target 13.0
                  [--sdk PATH] SOURCE.swift [...]

Chromium's macOS toolchain has no Swift tool (build/toolchain/apple/
toolchain.gni defines one for iOS only), so tools/on_device/BUILD.gn runs
this as an action instead. FoundationModels is linked weakly: on a macOS
older than 26 the helper still starts, and reports the model unsupported.
"""

import argparse
import os
import subprocess
import sys

_ARCHS = {'arm64': 'arm64', 'x64': 'x86_64'}


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--out', required=True)
  parser.add_argument('--cpu', required=True, choices=sorted(_ARCHS))
  parser.add_argument('--deployment-target', required=True)
  parser.add_argument('--sdk', default='')
  parser.add_argument('sources', nargs='+')
  args = parser.parse_args()

  sdk = args.sdk or subprocess.check_output(
      ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
  # Chromium links the SDK into the out directory (sdk/xcode_links/); swiftc
  # finds FoundationModels' macro plugins (@Generable) next to the real one,
  # in its platform's Developer/usr/lib/swift/host/plugins.
  sdk = os.path.realpath(sdk)
  command = [
      'xcrun', '--sdk', 'macosx', 'swiftc',
      '-sdk', sdk,
      '-target', '%s-apple-macos%s' % (_ARCHS[args.cpu],
                                       args.deployment_target),
      '-swift-version', '6',
      '-O',
      '-module-name', 'compass_on_device',
      '-Xlinker', '-weak_framework', '-Xlinker', 'FoundationModels',
      '-o', args.out,
  ] + args.sources
  return subprocess.call(command)


if __name__ == '__main__':
  sys.exit(main())
