#!/usr/bin/env bash
# Updates a Compass sync server that runs from docker compose on another
# machine, the way the public instance runs (docs/design/sync.md,
# "Decisions" 1): the compose project in DIR builds its image from DIR/src,
# a copy of this repository's server/, and keeps its database in DIR/data.
#
#   server/deploy/update-remote.sh --host example.org [--port 22] \
#       [--user root] [--dir /opt/compass-sync] [--service sync] \
#       [--local-port 8295] [--public-url https://sync.example.org] [--dry-run]
#
# Steps: back up the database (the server's own consistent copy, into
# DIR/backups), rsync server/ to DIR/src, build the image with COMMIT set to
# this checkout's short SHA, `docker compose up -d`, then check /health-check
# (it must name the new commit) and /healthz on the machine, and on
# --public-url if given. Nothing secret lives here: ssh uses your own keys
# and config. Run it from a clean checkout of what you mean to deploy.
set -euo pipefail

host=""
port=22
user=root
dir=/opt/compass-sync
service=sync
local_port=8295
public_url=""
dry_run=0

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-2}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) host="$2"; shift 2 ;;
    --port) port="$2"; shift 2 ;;
    --user) user="$2"; shift 2 ;;
    --dir) dir="$2"; shift 2 ;;
    --service) service="$2"; shift 2 ;;
    --local-port) local_port="$2"; shift 2 ;;
    --public-url) public_url="${2%/}"; shift 2 ;;
    --dry-run|-n) dry_run=1; shift ;;
    -h|--help) usage 0 ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
done
[[ -n "$host" ]] || { echo "--host is required" >&2; usage; }

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
server_dir="$(cd "$here/.." && pwd)"
repo="$(git -C "$server_dir" rev-parse --show-toplevel)"
commit="$(git -C "$repo" rev-parse --short HEAD)"
if [[ -n "$(git -C "$repo" status --porcelain -- server)" ]]; then
  echo "server/ has uncommitted changes; the image would not match $commit" >&2
  exit 1
fi
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
target="$user@$host"
ssh_cmd=(ssh -p "$port" -o BatchMode=yes)

run() {
  echo "+ $*"
  [[ $dry_run -eq 1 ]] || "$@"
}

remote() {
  # One argument: a script for the remote shell.
  run "${ssh_cmd[@]}" "$target" "$1"
}

echo "Updating $target:$dir to $commit"

# 1. Backup. The running server writes a consistent copy (safe while it
# runs); if it isn't running, the files can be copied as they are.
remote "set -e
cd '$dir'
mkdir -p backups
if docker compose ps --status running --services 2>/dev/null | grep -qx '$service'; then
  docker compose exec -T '$service' /compass-sync-server backup - > 'backups/pre-update-$stamp.db'
  test -s 'backups/pre-update-$stamp.db'
else
  tar -C '$dir' -czf 'backups/pre-update-$stamp.tar.gz' data
fi
ls -l backups | tail -n 5"

# 2. Sources.
run rsync -az --delete -e "ssh -p $port -o BatchMode=yes" \
  --exclude '/deploy/' "$server_dir/" "$target:$dir/src/"

# 3. Build and restart.
remote "set -e
cd '$dir'
docker compose build --build-arg COMMIT='$commit' --build-arg VERSION='$commit' \
  --build-arg BUILD_TIME='$stamp' '$service'
docker compose up -d
docker compose ps"

# 4. Health. /health-check answers JSON naming the running commit.
check_remote="set -e
cd '$dir'
for i in \$(seq 1 30); do
  if body=\$(curl -fsS http://127.0.0.1:$local_port/health-check 2>/dev/null) &&
     echo \"\$body\" | grep -q '$commit' &&
     curl -fsS http://127.0.0.1:$local_port/healthz | grep -qx ok; then
    echo \"healthy: \$body\"
    exit 0
  fi
  sleep 2
done
echo 'not healthy after 60 s' >&2
docker compose logs --tail 50 '$service' >&2 || true
exit 1"
remote "$check_remote"

if [[ -n "$public_url" ]]; then
  run curl -fsS "$public_url/healthz"
  if [[ $dry_run -eq 0 ]]; then
    curl -fsS "$public_url/health-check" | grep -q "$commit" ||
      { echo "$public_url/health-check doesn't name $commit" >&2; exit 1; }
    echo "public: $public_url runs $commit"
  fi
fi

echo "Done. Backup: $dir/backups/pre-update-$stamp.*"
