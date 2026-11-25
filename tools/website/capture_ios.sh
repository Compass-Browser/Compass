#!/bin/bash
# The website's iPhone screenshots (website/img/ios-*.webp), from the demo
# sidebar in the simulator: runs ios/UITests/WebsiteScreenshotsTests with
# the status bar set to 9:41, full battery and full signal, in light
# appearance, then exports the shots and converts them to WebP.
#
#   tools/website/capture_ios.sh
#
# SIMULATOR names the simulator (default "iPhone 18 Pro"); it's booted if
# it isn't. Needs xcodegen and cwebp (brew install xcodegen webp).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
ios="$root/ios"
work="$ios/build/website-shots"
out="$root/website/img"
sim_name="${SIMULATOR:-iPhone 18 Pro}"
width=600  # Twice the widest the site shows a phone's screen.

udid=$(xcrun simctl list devices available -j | /usr/bin/python3 -c '
import json, sys
name = sys.argv[1]
for devices in json.load(sys.stdin)["devices"].values():
    for d in devices:
        if d["name"] == name:
            print(d["udid"]); sys.exit()
' "$sim_name")
[[ -n "$udid" ]] || { echo "No simulator named \"$sim_name\"."; exit 1; }
echo "Simulator: $sim_name ($udid)"
xcrun simctl boot "$udid" 2>/dev/null || true
xcrun simctl bootstatus "$udid" -b >/dev/null

xcrun simctl ui "$udid" appearance light
xcrun simctl status_bar "$udid" override --time 9:41 --dataNetwork wifi --wifiMode active \
  --wifiBars 3 --cellularMode active --cellularBars 4 --batteryState discharging --batteryLevel 100
trap 'xcrun simctl status_bar "$udid" clear' EXIT

rm -rf "$work/result.xcresult" "$work/attachments"
mkdir -p "$work" "$out"
(cd "$ios" && xcodegen generate --quiet)
TEST_RUNNER_WEBSITE_SHOTS=1 xcodebuild test -quiet -project "$ios/Compass.xcodeproj" -scheme Compass \
  -destination "id=$udid" -derivedDataPath "$work/dd" -resultBundlePath "$work/result.xcresult" \
  -only-testing:CompassUITests/WebsiteScreenshotsTests

xcrun xcresulttool export attachments --path "$work/result.xcresult" --output-path "$work/attachments" >/dev/null
# manifest.json: each attachment's file and the name the test gave it
# ("website-<name>", with a suffix added).
/usr/bin/python3 - "$work/attachments" <<'EOF' | while IFS=$'\t' read -r name file; do
import json, re, sys
d = sys.argv[1]
for test in json.load(open(f"{d}/manifest.json")):
    for a in test["attachments"]:
        m = re.match(r"website-([a-z0-9-]+)", a.get("suggestedHumanReadableName", ""))
        if m:
            print(f"{m.group(1)}\t{d}/{a['exportedFileName']}")
EOF
  cwebp -quiet -q 82 -resize "$width" 0 "$file" -o "$out/ios-$name.webp"
  echo "website/img/ios-$name.webp"
done
