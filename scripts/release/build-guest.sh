#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
output=""
contract_only=0

while (($#)); do
  case "$1" in
    --output)
      output=${2:-}
      shift 2
      ;;
    --contract-only)
      contract_only=1
      shift
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if ((contract_only == 0)) && [[ -z $output ]]; then
  echo "Usage: $0 --output DIR [--contract-only]" >&2
  exit 2
fi

source_lock="$repo_root/guest-build/source.lock.json"
readarray -t source_fields < <(python3 - "$source_lock" <<'PY'
import json
import pathlib
import sys

lock = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
print(lock["repository"])
print(lock["commit"])
PY
)
source_url=${source_fields[0]}
source_commit=${source_fields[1]}
[[ $source_commit =~ ^[0-9a-f]{40}$ ]] || { echo "Invalid guest source commit" >&2; exit 1; }

work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/try-omarchy-guest-source.XXXXXX")
cleanup() {
  rm -rf -- "$work"
}
trap cleanup EXIT

git -C "$work" init --quiet
git -C "$work" remote add origin "$source_url"
git -C "$work" fetch --quiet --depth=1 origin "$source_commit"
git -C "$work" checkout --quiet --detach FETCH_HEAD
test "$(git -C "$work" rev-parse HEAD)" = "$source_commit"
git -C "$work" config user.name "Try Omarchy Release"
git -C "$work" config user.email "actions@users.noreply.github.com"
git -C "$work" am "$repo_root"/guest-build/*.patch

"$work/guest/test"

if ((contract_only)); then
  exit 0
fi

mkdir -p "$output"
sudo bash "$work/guest/build-container.sh" --output "$output"
