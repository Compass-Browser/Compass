#!/bin/bash
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Starts or stops one headless Compass instance for the sync end-to-end run
# (docs/design/sync-slice-2-steps.md). Headless: no window, never takes
# focus. Sync is set up through chrome://compass-sync (see the steps), not
# switches; a restart is a stop and a start with the same NAME.
#
#   instance.sh start NAME PORT [extra browser switches...]
#   instance.sh stop NAME
#
# Environment:
#   E2E_DIR     where profiles and logs go (default ~/compass-sync-e2e)
#   COMPASS_APP the .app (default <chromium>/src/out/Debug/Compass.app)
set -euo pipefail

E2E_DIR="${E2E_DIR:-$HOME/compass-sync-e2e}"
if [ -z "${COMPASS_APP:-}" ]; then
  SRC="$(cd "$(dirname "$0")/.." && python3 -c \
    'from compass_tool.layout import detect; print(detect().src)')"
  COMPASS_APP="$SRC/out/Debug/Compass.app"
fi

action="${1:-}"; name="${2:-}"
dir="$E2E_DIR/$name"

case "$action" in
  start)
    port="$3"; shift 3
    mkdir -p "$dir"
    if [ -f "$dir/pid" ] && kill -0 "$(cat "$dir/pid")" 2>/dev/null; then
      echo "$name already running (pid $(cat "$dir/pid"))"; exit 1
    fi
    stamp="$(date +%H%M%S)"
    # --use-mock-keychain: os_crypt_async gets a fixed key, so the seed
    # stored by one run decrypts in the next without a Keychain prompt.
    "$COMPASS_APP/Contents/MacOS/Compass" \
      --headless \
      --user-data-dir="$dir/profile" \
      --remote-debugging-port="$port" \
      --use-mock-keychain \
      --no-first-run --no-default-browser-check \
      --disable-features=MacDisclaimGpuTccResponsibility \
      --enable-logging=stderr --log-level=0 \
      --vmodule=compass_sync_service=1,compass_sync_notifier=1,engine_components_factory=1,compass_spaces_sync_bridge=1,compass_profile_creator=1 \
      --log-net-log="$dir/netlog-$stamp.json" \
      --sync-short-nudge-delay-for-test \
      --sync-short-initial-retry-override \
      "$@" \
      about:blank > "$dir/stderr-$stamp.log" 2>&1 < /dev/null &
    echo $! > "$dir/pid"
    ln -sf "stderr-$stamp.log" "$dir/stderr.log"
    echo "$name: pid $(cat "$dir/pid"), port $port, log $dir/stderr-$stamp.log"
    ;;
  stop)
    if [ -f "$dir/pid" ]; then
      pid="$(cat "$dir/pid")"
      kill -TERM "$pid" 2>/dev/null || true
      for _ in $(seq 30); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
      kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" || true
      rm -f "$dir/pid"
      echo "$name stopped"
    fi
    ;;
  *)
    echo "usage: $0 start NAME PORT [switches...] | stop NAME" >&2; exit 2 ;;
esac
