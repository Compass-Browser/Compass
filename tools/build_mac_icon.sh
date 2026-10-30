#!/bin/sh
# Copyright 2026 The Strata Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Compiles the app icon in assets/ into the two files the macOS bundle takes
# (chrome/BUILD.gn, :chrome_app_icon and :chrome_asset_catalog):
#
#   assets/mac/Assets.car  the Liquid Glass icon from assets/Compass.icon,
#                          named AppIcon (CFBundleIconName)
#   assets/mac/app.icns    the flat icon, assets/Compass.icns, for macOS
#                          before 26 (CFBundleIconFile)
#
# Run after changing either icon and commit the results. Needs Xcode 26+.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

# actool names the icon after its .icon bundle.
cp -R "$root/assets/Compass.icon" "$dir/AppIcon.icon"
mkdir "$dir/out"
xcrun actool --compile "$dir/out" --platform macosx \
  --minimum-deployment-target 12.0 --app-icon AppIcon \
  --output-partial-info-plist "$dir/partial.plist" \
  "$dir/AppIcon.icon" >/dev/null

mkdir -p "$root/assets/mac"
cp "$dir/out/Assets.car" "$root/assets/mac/Assets.car"
cp "$root/assets/Compass.icns" "$root/assets/mac/app.icns"
echo "wrote assets/mac/Assets.car and assets/mac/app.icns"
