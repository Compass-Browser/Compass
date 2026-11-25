#!/bin/bash
# App Review's screen recording (guideline 2.1), on a physical iPhone over USB.
# Builds the app and UITests/ReviewDemoTests, then records the iPhone's
# screen (as QuickTime's New Movie Recording does) while the test walks the
# typical flow from the Home Screen.
#
#   REVIEW_AI_KEY=sk-ant-… REVIEW_INVITE='https://…/invite#…' \
#     [CLEAN=1] ios/scripts/review-demo/record.sh [out.mov]
#
# DEVICE (UDID) defaults to the first wired physical iPhone. The Mac asks once
# for camera access for the terminal: the iPhone's screen is a capture device.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
ios="$(cd "$here/../.." && pwd)"
out="${1:-$ios/build/review-demo.mov}"
[[ "$out" = /* ]] || out="$PWD/$out"
mkdir -p "$(dirname "$out")"
work="$ios/build/review-demo"
mkdir -p "$work"

if [[ -z "${DEVICE:-}" ]]; then
  DEVICE=$(xcrun devicectl list devices --json-output "$work/devices.json" >/dev/null 2>&1 &&
    /usr/bin/python3 -c '
import json, sys
for d in json.load(open(sys.argv[1]))["result"]["devices"]:
    hp, cp = d.get("hardwareProperties", {}), d.get("connectionProperties", {})
    if hp.get("reality") == "physical" and hp.get("deviceType") == "iPhone" and cp.get("transportType") == "wired":
        print(hp["udid"]); break
' "$work/devices.json")
fi
[[ -n "$DEVICE" ]] || { echo "No wired iPhone found (set DEVICE=<udid>)."; exit 1; }
name=$(/usr/bin/python3 -c '
import json, sys
for d in json.load(open(sys.argv[1]))["result"]["devices"]:
    if d.get("hardwareProperties", {}).get("udid") == sys.argv[2]: print(d["deviceProperties"]["name"])
' "$work/devices.json" "$DEVICE" 2>/dev/null || true)
echo "iPhone: ${name:-?} ($DEVICE)"

swiftc -O "$here/record_iphone.swift" -o "$work/record_iphone"
(cd "$ios" && xcodegen generate --quiet)
xcodebuild build-for-testing -quiet -project "$ios/Compass.xcodeproj" -scheme Compass \
  -destination "id=$DEVICE" -derivedDataPath "$work/dd" -allowProvisioningUpdates

# CLEAN=1: uninstall Compass from the iPhone first (deletes its data there), so
# nothing from earlier runs or a synced chain's history is in the video.
if [[ "${CLEAN:-}" == 1 ]]; then
  xcrun devicectl device uninstall app --device "$DEVICE" it.cipi1965.compass >/dev/null 2>&1 || true
fi

rm -f "$out" "$out.stop"
"$work/record_iphone" "$out" "${name:-iPhone}" &
recorder=$!
trap 'touch "$out.stop"; wait $recorder 2>/dev/null || true; rm -f "$out.stop"' EXIT
sleep 3

t0=$(date +%s)
start=""; end=""
while IFS= read -r line; do
  case "$line" in
    *"Test Case"*"started"*) start=$(( $(date +%s) - t0 )) ;;
    *"Test Case"*"passed"*|*"Test Case"*"failed"*) end=$(( $(date +%s) - t0 )) ;;
  esac
  [[ "$line" =~ Test\ Case|error:|missing|passed|failed|skipped ]] && echo "$line"
done < <(TEST_RUNNER_REVIEW_DEMO=1 \
TEST_RUNNER_REVIEW_AI_KEY="${REVIEW_AI_KEY:-}" \
TEST_RUNNER_REVIEW_INVITE="${REVIEW_INVITE:-}" \
xcodebuild test-without-building -project "$ios/Compass.xcodeproj" -scheme Compass \
  -destination "id=$DEVICE" -derivedDataPath "$work/dd" \
  -only-testing:CompassUITests/ReviewDemoTests/testReviewDemo \
  -resultBundlePath "$work/result-$(date +%s).xcresult" 2>&1)

sleep 2
touch "$out.stop"; wait $recorder 2>/dev/null || true; rm -f "$out.stop"
trap - EXIT
echo "Recording (untrimmed): $out"
# The test's span, from the Home Screen before the launch to its end (the
# recorder started 3 s before t0).
if [[ -n "$start" && -n "$end" ]]; then
  trimmed="${out%.*}-trimmed.mp4"
  ffmpeg -v error -y -ss $((start + 3)) -to $((end + 4)) -i "$out" -c:v libx264 -crf 20 -preset medium \
    -pix_fmt yuv420p -c:a aac -movflags +faststart "$trimmed"
  echo "Recording: $trimmed"
fi
