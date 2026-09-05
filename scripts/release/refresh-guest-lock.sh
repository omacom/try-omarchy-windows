#!/usr/bin/env bash
# Refreshes the guest package lock against current Arch and, when it moved,
# writes the next numbered patch under guest-build/. Run it locally (needs
# Docker and sudo, like the guest build) or let the scheduled workflow do it.
#
#   scripts/release/refresh-guest-lock.sh            # writes guest-build/NNNN-Refresh-the-guest-package-lock.patch
#   scripts/release/refresh-guest-lock.sh --check    # exit 3 when the lock drifted, 0 when current
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
check_only=0
[[ ${1:-} == --check ]] && check_only=1

readarray -t source_fields < <(python3 - "$repo_root/guest-build/source.lock.json" <<'PY'
import json, pathlib, sys
lock = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
print(lock["repository"]); print(lock["commit"])
PY
)
source_url=${source_fields[0]}
source_commit=${source_fields[1]}

work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/try-omarchy-lock-refresh.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
git -C "$work" init --quiet
git -C "$work" remote add origin "$source_url"
git -C "$work" fetch --quiet --depth=1 origin "$source_commit"
git -C "$work" checkout --quiet --detach FETCH_HEAD
git -C "$work" config user.name "Try Omarchy Release"
git -C "$work" config user.email "actions@users.noreply.github.com"
git -C "$work" am --quiet "$repo_root"/guest-build/*.patch

sudo bash "$work/guest/build-container.sh" --refresh-package-lock "$work/guest/packages.lock.json"
sudo chown -- "$(id -u):$(id -g)" "$work/guest/packages.lock.json"

if git -C "$work" diff --quiet -- guest/packages.lock.json; then
  echo "guest package lock is current"
  exit 0
fi
if ((check_only)); then
  echo "guest package lock drifted:"
  git -C "$work" --no-pager diff --stat -- guest/packages.lock.json
  exit 3
fi

summary=$(python3 - "$work/guest/packages.lock.json" <<'PY'
import json, pathlib, subprocess, sys
path = pathlib.Path(sys.argv[1])
new = json.loads(path.read_text())["packages"]
old = json.loads(subprocess.check_output(["git", "-C", str(path.parents[1]), "show", "HEAD:guest/packages.lock.json"]))["packages"]
changed = [f"{name} {old[name]} -> {new[name]}" for name in sorted(new) if name in old and old[name] != new[name]]
added = [f"{name} {new[name]} (new)" for name in sorted(new) if name not in old]
removed = [f"{name} (removed)" for name in sorted(old) if name not in new]
print("\n".join(changed + added + removed))
PY
)
git -C "$work" add guest/packages.lock.json
git -C "$work" commit --quiet -m "Refresh the guest package lock" -m "$summary"
last=$(ls "$repo_root"/guest-build/*.patch | sed 's/.*\/\([0-9]*\)-.*/\1/' | sort -n | tail -1)
next=$(printf '%04d' $((10#$last + 1)))
git -C "$work" format-patch --quiet -1 --start-number "$((10#$next))" -o "$repo_root/guest-build/"
"$work/guest/test"
echo "wrote guest-build/$next-Refresh-the-guest-package-lock.patch"
printf '%s\n' "$summary"
