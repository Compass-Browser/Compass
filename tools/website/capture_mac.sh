#!/bin/bash
# The website's Mac screenshots (website/img/mac-*.webp): the installed
# Compass (/Applications/Compass.app, or COMPASS_APP) in a throwaway profile
# whose sidebar is tools/website/demo_sidebar.json (the iOS demo sidebar's
# spaces), in light appearance, one window per shot, captured without its
# shadow.
#
#   tools/website/capture_mac.sh
#   ONLY="mac-reading mac-split" tools/website/capture_mac.sh   # just those
#
# Shots: mac-personal (the Personal space on a Wikipedia page), mac-work and
# mac-reading (the other spaces), and each one's sidebar alone
# (mac-<space>-sidebar); mac-split (Work's split view) and mac-commandbar
# (the command bar over it), mac-claude (the Claude panel answering a
# question about the page, from your own Claude Code). Each shot brings the demo instance to the front
# for a moment, and the last two type into it (Esc, Cmd+L, a query) while
# it's frontmost. A first launch visits the demo's sites so
# the sidebar has their favicons. Your own Compass and profile aren't
# touched; light appearance is NSRequiresAquaSystemAppearance, set in
# Compass's defaults only while a shot's window opens. Needs cwebp
# (brew install webp).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
app="${COMPASS_APP:-/Applications/Compass.app}"
bin="$app/Contents/MacOS/Compass"
bid=$(defaults read "$app/Contents/Info.plist" CFBundleIdentifier)
work="${TMPDIR:-/tmp}/compass-website-shots"
profile="$work/profile"
out="$root/website/img"
width=2000  # The hero's window at twice its widest on the site.
port=9333   # The demo instance's DevTools port (cdp_eval.mjs), on 127.0.0.1.
mkdir -p "$work" "$out"

if defaults read "$bid" NSRequiresAquaSystemAppearance >/dev/null 2>&1; then
  echo "$bid already has NSRequiresAquaSystemAppearance set; not touching it."; exit 1
fi
cleanup() {
  defaults delete "$bid" NSRequiresAquaSystemAppearance 2>/dev/null || true
  [[ -n "${pid:-}" ]] && kill "$pid" 2>/dev/null || true
}
trap cleanup EXIT

# The IDs of a process's browser windows, largest first. Not only
# those on screen: a window sometimes opens on another desktop, and
# screencapture -l takes it from there too.
cat > "$work/windows.swift" <<'EOF'
import CoreGraphics
let pid = Int32(CommandLine.arguments[1])!
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
// The browser window opens at --window-size (1440 x 900).
let mine = list.filter {
  let b = $0[kCGWindowBounds as String] as! [String: Double]
  return ($0[kCGWindowOwnerPID as String] as? Int32) == pid && ($0[kCGWindowLayer as String] as? Int) == 0
    && b["Width"] == 1440 && b["Height"] == 900
}
for w in mine.sorted(by: {
  let a = $0[kCGWindowBounds as String] as! [String: Double], b = $1[kCGWindowBounds as String] as! [String: Double]
  return a["Width"]! * a["Height"]! > b["Width"]! * b["Height"]!
}) { print(w[kCGWindowNumber as String]!) }
EOF
swiftc -O "$work/windows.swift" -o "$work/windows" 2>/dev/null

# The demo sidebar with |active| as the shown space, without the Today tab
# that the launch opens (|url|), so it isn't there twice.
sidebar() {
  /usr/bin/python3 - "$root/tools/website/demo_sidebar.json" "$1" "$2" > "$dir/Default/Compass Sidebar.json" <<'EOF'
import json, sys
doc = json.load(open(sys.argv[1]))
for space in doc["spaces"]:
    if space["name"] == sys.argv[2]:
        doc["active"] = space["id"]
        space["today"] = [n for n in space["today"] if n.get("url") != sys.argv[3]]
print(json.dumps(doc, ensure_ascii=False))
EOF
}

launch() {
  defaults write "$bid" NSRequiresAquaSystemAppearance -bool YES
  "$bin" --user-data-dir="$dir" --no-first-run --no-default-browser-check \
    --window-size=1440,900 --window-position=40,40 --remote-debugging-port="$port" \
    "$@" >"$work/compass.log" 2>&1 &
  pid=$!
  sleep 4
  defaults delete "$bid" NSRequiresAquaSystemAppearance
}

quit() {
  kill "$pid"
  # Every process of the profile (helpers too), or the next launch hands its
  # URL to the one shutting down and opens no window.
  while kill -0 "$pid" 2>/dev/null || pgrep -f -- "--user-data-dir=$dir" >/dev/null; do sleep 0.5; done
  rm -f "$dir"/Singleton*
  pid=""
}

rm -rf "$work"/profile*
mkdir -p "$profile/Default"
dir="$profile"

# Favicons: visit every site in the demo sidebar once.
sidebar Personal ""
urls=$(/usr/bin/python3 -c '
import json, sys
doc = json.load(open(sys.argv[1]))
def walk(nodes):
    for n in nodes:
        if n.get("url"): yield n["url"]
        yield from walk(n.get("children", []))
seen = []
for u in list(walk(doc["favorites"])) + [u for s in doc["spaces"] for u in walk(s["pinned"] + s["today"])]:
    if u not in seen: seen.append(u)
print("\n".join(seen))' "$root/tools/website/demo_sidebar.json")
echo "Visiting the demo's sites for their favicons…"
# shellcheck disable=SC2086
launch $urls
sleep 25
quit

# A fresh copy of the warmed-up profile, its sidebar showing <space>, and
# Compass on it, opening <url> if one is given (else the empty page and its
# command bar); waits for the window. A copy each time: a launch into a
# profile another launch just quit sometimes opens no window.
open_demo() {  # <name> <space> <url> [prefs]
  dir="$work/profile-$1"
  rm -rf "$dir"
  cp -R "$profile" "$dir"
  rm -f "$dir"/Singleton*
  sidebar "$2" "$3"
  # <prefs>: JSON for the profile's Preferences before it starts.
  [[ -n "${4:-}" ]] && printf '%s' "$4" > "$dir/Default/Preferences"
  if [[ -n "$3" ]]; then launch "$3"; else launch; fi
  sleep 14
  local tries=0
  window=""
  until [[ -n "$window" ]] || (( tries++ > 20 )); do
    window=$("$work/windows" "$pid" | head -1)
    [[ -n "$window" ]] || sleep 1
  done
  [[ -n "$window" ]] || { echo "No window for $1."; exit 1; }
}

# The window opens on whichever desktop macOS picks, maybe one not showing,
# and a window that was never drawn can't be captured: the demo instance is
# brought to the front first (it takes the focus for a moment).
front() {
  osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $pid) to true" >/dev/null
  sleep 2
}

# website/img/<name>.webp from $work/<name>.png, and with |sidebar| its
# sidebar alone (the window's left 235 points), for the feature sections.
convert() {  # <name> [sidebar]
  cwebp -quiet -q 85 -alpha_q 100 -resize "$width" 0 "$work/$1.png" -o "$out/$1.webp"
  echo "website/img/$1.webp"
  if [[ "${2:-}" == sidebar ]]; then
    cwebp -quiet -q 85 -alpha_q 100 -crop 0 0 470 1800 "$work/$1.png" -o "$out/$1-sidebar.webp"
    echo "website/img/$1-sidebar.webp"
  fi
}

# Whether to take <name>: ONLY="mac-reading mac-split" takes just those.
wanted() { [[ -z "${ONLY:-}" || " $ONLY " == *" $1 "* ]]; }

shot() {  # <name> <space> <url>
  wanted "$1" || return 0
  open_demo "$@"
  local tries=0
  until front && screencapture -o -x -l "$window" "$work/$1.png" 2>/dev/null; do
    (( ++tries < 3 )) || { echo "Couldn't capture $1."; exit 1; }
  done
  convert "$1" sidebar
  quit
}

shot mac-personal Personal "https://en.wikipedia.org/wiki/Lisbon"
shot mac-work Work "https://webkit.org/blog/"
shot mac-reading Reading "https://en.wikipedia.org/wiki/Age_of_Discovery"

# Split view and the command bar, in the Work space: its split (WebKit Blog
# and MDN) is clicked in the sidebar page over the DevTools protocol; then,
# with the window in front, Esc closes the empty page's command bar, the
# split is captured, Cmd+L opens the bar over it with a query typed, and
# that's captured. The keys go only while the demo instance is frontmost
# (the bar closes when its window stops being the key window, so it's all
# one step).
if wanted mac-split || wanted mac-commandbar; then
open_demo mac-split Work ""
node "$root/tools/website/cdp_eval.mjs" "$port" chrome://compass-sidebar '(() => {
  const pane = document.querySelector("compass-sidebar-app").shadowRoot.querySelector(".split.row .tab.pane");
  for (const type of ["pointerdown", "mousedown", "pointerup", "mouseup", "click"]) {
    pane.dispatchEvent(new MouseEvent(type, {bubbles: true, composed: true, button: 0}));
  }
  return "split shown";
})()'
sleep 10
osascript <<APPLESCRIPT
tell application "System Events"
  set p to (first process whose unix id is $pid)
  set frontmost of p to true
  delay 2
  if frontmost of p then key code 53
  delay 1
  do shell script "screencapture -o -x -l $window '$work/mac-split.png'"
  if frontmost of p then keystroke "l" using command down
  delay 0.6
  if frontmost of p then keystroke "css grid"
  delay 2.5
  do shell script "screencapture -o -x -l $window '$work/mac-commandbar.png'"
  if frontmost of p then key code 53
end tell
APPLESCRIPT
convert mac-split
convert mac-commandbar
quit
fi

# The Claude panel, answering a question about the Lisbon page. Claude Code
# mode (compass.claude.backend 1): your own `claude` answers, signed in with
# your subscription, with none of its own tools (Compass's only, in the
# throwaway profile); no API key needed. The question goes in through the
# command bar page's "Ask Claude" row, which opens the panel and sends it
# with the page attached; once the reply stops growing, the panel scrolls
# back to the question and the window is captured.
if wanted mac-claude; then
open_demo mac-claude Personal "https://en.wikipedia.org/wiki/Lisbon" '{"compass":{"claude":{"backend":1}}}'
node "$root/tools/website/cdp_eval.mjs" "$port" chrome://compass-command-bar '(async () => {
  const app = document.querySelector("compass-command-bar-app");
  const q = "What should I see in Lisbon in one day?";
  app.typed_ = q;
  app.browserProxy_.handler.query(q, false);
  let ask;
  for (let i = 0; i < 50 && !ask; i++) {
    await new Promise((f) => setTimeout(f, 200));
    ask = (app.results_ || []).find((r) => r.title === "Ask Claude");
  }
  if (!ask) throw new Error("no Ask Claude row");
  app.browserProxy_.handler.accept(ask.id, q, false);
  return "asked";
})()'
# The panel's scrolling list of messages, in its page's shadow roots.
panel='(() => { const found = []; const walk = (root) => { for (const e of root.querySelectorAll("*")) { if (e.scrollHeight > e.clientHeight + 20 && getComputedStyle(e).overflowY !== "visible") found.push(e); if (e.shadowRoot) walk(e.shadowRoot); } }; walk(document); return found; })()'
last=0; same=0
for _ in $(seq 1 40); do
  sleep 5
  height=$(node "$root/tools/website/cdp_eval.mjs" "$port" chrome://compass-claude \
    "$panel.reduce((n, e) => Math.max(n, e.scrollHeight), 0)" 2>/dev/null || echo 0)
  if [[ "$height" == "$last" && "$height" -gt 300 ]]; then (( ++same >= 3 )) && break; else same=0; fi
  last=$height
done
node "$root/tools/website/cdp_eval.mjs" "$port" chrome://compass-claude "$panel.forEach((e) => e.scrollTop = 0) || 'top'" >/dev/null
tries=0
until front && screencapture -o -x -l "$window" "$work/mac-claude.png" 2>/dev/null; do
  (( ++tries < 3 )) || { echo "Couldn't capture mac-claude."; exit 1; }
done
convert mac-claude
# The panel and the page beside it, larger: the window's top right.
cwebp -quiet -q 85 -alpha_q 100 -crop 1500 0 1380 1200 "$work/mac-claude.png" -o "$out/mac-claude-panel.webp"
echo "website/img/mac-claude-panel.webp"
quit
fi
