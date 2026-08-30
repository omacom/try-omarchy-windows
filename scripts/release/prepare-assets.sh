#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
artifacts=${1:-}
[[ -n $artifacts && -d $artifacts ]] || { echo "Usage: $0 ARTIFACT_DIR" >&2; exit 2; }

runtime_lock="$repo_root/guest-build/runtime.lock.json"
readarray -t runtime_fields < <(python3 - "$runtime_lock" <<'PY'
import json
import pathlib
import sys

lock = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
print(lock["url"])
print(lock["filename"])
print(lock["sha256"])
PY
)
runtime_url=${runtime_fields[0]}
runtime_name=${runtime_fields[1]}
runtime_sha256=${runtime_fields[2]}
[[ $runtime_name != */* && $runtime_sha256 =~ ^[0-9a-f]{64}$ ]] || {
  echo "Invalid runtime lock" >&2
  exit 1
}

curl --fail --location --retry 3 --output "$artifacts/$runtime_name" "$runtime_url"
printf '%s  %s\n' "$runtime_sha256" "$artifacts/$runtime_name" | sha256sum --check

if grep -qE "[[:space:]]${runtime_name}$" "$artifacts/SHA256SUMS"; then
  echo "$runtime_name is already present in SHA256SUMS" >&2
  exit 1
fi
printf '%s  %s\n' "$runtime_sha256" "$runtime_name" >>"$artifacts/SHA256SUMS"

(
  cd "$artifacts"
  sha256sum --check SHA256SUMS
)

sha256sum "$artifacts/SHA256SUMS"
