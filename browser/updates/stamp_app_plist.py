#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Adds Compass's keys to the app's Info.plist (docs/design/updates.md).

- CompassVersion and CFBundleVersion: Compass's version. Sparkle compares
  CFBundleVersion with the feed's sparkle:version, so it must grow with every
  release, which upstream's BUILD.PATCH doesn't when only Compass changes.
  CFBundleShortVersionString stays upstream's: Chromium reads it.
- With a feed: Sparkle's SUFeedURL, SUPublicEDKey, and silent automatic
  checks and downloads.
"""

import argparse
import plistlib
import sys

# How often Sparkle checks, in seconds.
CHECK_INTERVAL = 4 * 60 * 60


def stamp(plist, version, feed_url, public_key):
  if version:
    if not all(part.isdigit() for part in version.split('.')):
      raise ValueError('compass_version must be numbers and dots: %r' %
                       version)
    plist['CFBundleVersion'] = version
    plist['CompassVersion'] = version
  if feed_url:
    if not public_key:
      raise ValueError('compass_update_feed_url needs '
                       'compass_sparkle_public_key')
    plist['SUFeedURL'] = feed_url
    plist['SUPublicEDKey'] = public_key
    plist['SUEnableAutomaticChecks'] = True
    plist['SUAutomaticallyUpdate'] = True
    plist['SUAllowsAutomaticUpdates'] = True
    plist['SUEnableSystemProfiling'] = False
    plist['SUScheduledCheckInterval'] = CHECK_INTERVAL
  return plist


def main(argv):
  parser = argparse.ArgumentParser()
  parser.add_argument('--input', required=True)
  parser.add_argument('--output', required=True)
  parser.add_argument('--version', default='')
  parser.add_argument('--feed-url', default='')
  parser.add_argument('--public-key', default='')
  args = parser.parse_args(argv)
  with open(args.input, 'rb') as f:
    plist = plistlib.load(f)
  try:
    stamp(plist, args.version, args.feed_url, args.public_key)
  except ValueError as e:
    print(e, file=sys.stderr)
    return 1
  with open(args.output, 'wb') as f:
    plistlib.dump(plist, f, fmt=plistlib.FMT_XML)
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
