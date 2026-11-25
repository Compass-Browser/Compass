#!/bin/bash
# One of the website's Android screenshots (website/img/android-<name>.webp),
# from what the emulator shows now, with the status bar in demo mode (9:41,
# full battery and signal, no notifications).
#
#   tools/website/capture_android.sh <name>   # e.g. page, sidebar, commandbar
#   tools/website/capture_android.sh --done   # leave demo mode
#
# Android has no demo sidebar like iOS's -CompassDemo, so the content is set
# up by hand first: on the compassdemo emulator (see docs or
# `emulator -avd compassdemo -gpu swiftshader_indirect -no-audio`), a release
# APK, three spaces (Personal in blue, Work in purple, Reading in green) and
# the same Lisbon pages as the iOS demo. Needs cwebp (brew install webp).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
adb="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
out="$root/website/img"
width=600  # Twice the widest the site shows a phone's screen.

demo() { "$adb" shell am broadcast -a com.android.systemui.demo -e command "$@" >/dev/null; }

if [[ "${1:-}" == --done ]]; then
  demo exit
  exit
fi
name="${1:?usage: $0 <name> | --done}"

"$adb" shell settings put global sysui_demo_allowed 1
demo enter
demo clock -e hhmm 0941
demo battery -e level 100 -e plugged false -e powersave false
demo network -e wifi show -e level 4 -e fully true
demo network -e mobile hide
demo notifications -e visible false
sleep 1

mkdir -p "$out"
tmp="$(mktemp -t compass-android).png"
"$adb" exec-out screencap -p > "$tmp"
cwebp -quiet -q 82 -resize "$width" 0 "$tmp" -o "$out/android-$name.webp"
rm -f "$tmp"
echo "website/img/android-$name.webp"
